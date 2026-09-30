/*
 * Quocker OCI registry client. This module fetches and verifies OCI content;
 * it does not execute containers or boot an image as a VM yet.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-oci.h"
#include "quocker-registry-auth.h"
#include "quocker-registry-config.h"
#include "quocker-registry-http.h"
#include "quocker-rootfs.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define OCI_MANIFEST_ACCEPT                                                    \
  "application/vnd.oci.image.index.v1+json, "                                  \
  "application/vnd.oci.image.manifest.v1+json, "                               \
  "application/vnd.docker.distribution.manifest.list.v2+json, "                \
  "application/vnd.docker.distribution.manifest.v2+json"

typedef struct OciReference {
  char *registry;
  char *repository;
  char *selector;
  char *mirror;
  gboolean selector_is_digest;
} OciReference;

typedef struct HttpResponse {
  GByteArray *body;
  FILE *file;
  GChecksum *checksum;
  guint64 received;
  guint64 limit;
  char *www_authenticate;
  char *location;
  char *retry_after;
  char *content_type;
} HttpResponse;

typedef struct OciDescriptor {
  char *digest;
  guint64 size;
  char *media_type;
} OciDescriptor;

static const char *json_string_member(JsonObject *object, const char *key) {
  if (!object || !json_object_has_member(object, key)) {
    return NULL;
  }
  JsonNode *node = json_object_get_member(object, key);
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    return NULL;
  }
  return json_node_get_string(node);
}

static void oci_error(const char *format, ...) G_GNUC_PRINTF(1, 2);

static void oci_error(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  g_printerr("quocker: ");
  vfprintf(stderr, format, ap);
  g_printerr("\n");
  va_end(ap);
}

static void reference_clear(OciReference *ref) {
  g_free(ref->registry);
  g_free(ref->repository);
  g_free(ref->selector);
  g_free(ref->mirror);
  memset(ref, 0, sizeof(*ref));
}

static gboolean valid_component(const char *s) {
  if (!s || !*s || !g_ascii_isalnum(*s)) {
    return FALSE;
  }
  for (const char *p = s; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-' ||
          *p == ':' || *p == '@' || *p == '/')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean reference_parse(const char *text, OciReference *ref) {
  memset(ref, 0, sizeof(*ref));
  if (!valid_component(text)) {
    oci_error("invalid OCI image reference '%s'", text ? text : "(null)");
    return FALSE;
  }

  const char *slash = strchr(text, '/');
  const char *dot = strchr(text, '.');
  const char *colon = strchr(text, ':');
  gboolean explicit_registry =
      slash && (!strncmp(text, "localhost/", 10) || (dot && dot < slash) ||
                (colon && colon < slash));
  const char *name = text;
  if (explicit_registry) {
    ref->registry = g_strndup(text, slash - text);
    name = slash + 1;
  } else {
    ref->registry = g_strdup("registry-1.docker.io");
  }

  const char *digest_at = strchr(name, '@');
  const char *tag_colon = strrchr(name, ':');
  const char *last_slash = strrchr(name, '/');
  if (tag_colon && last_slash && tag_colon < last_slash) {
    tag_colon = NULL;
  }
  if (digest_at) {
    ref->repository = g_strndup(name, digest_at - name);
    ref->selector = g_strdup(digest_at + 1);
    ref->selector_is_digest = TRUE;
    if (!g_str_has_prefix(ref->selector, "sha256:") ||
        strlen(ref->selector) != 71) {
      oci_error("only sha256 image digests are currently supported");
      goto invalid;
    }
  } else if (tag_colon) {
    ref->repository = g_strndup(name, tag_colon - name);
    ref->selector = g_strdup(tag_colon + 1);
  } else {
    ref->repository = g_strdup(name);
    ref->selector = g_strdup("latest");
  }
  if (g_str_equal(ref->registry, "index.docker.io") ||
      g_str_equal(ref->registry, "docker.io")) {
    g_free(ref->registry);
    ref->registry = g_strdup("registry-1.docker.io");
  }
  if (g_str_equal(ref->registry, "registry-1.docker.io") &&
      !strchr(ref->repository, '/')) {
    char *official = g_strdup_printf("library/%s", ref->repository);
    g_free(ref->repository);
    ref->repository = official;
  }
  if (!*ref->repository || !*ref->selector) {
    goto invalid;
  }
  return TRUE;

invalid:
  reference_clear(ref);
  oci_error("invalid OCI image reference '%s'", text);
  return FALSE;
}

static size_t response_write(char *data, size_t size, size_t nmemb,
                             void *opaque) {
  HttpResponse *response = opaque;
  size_t length = size * nmemb;
  if (response->limit && length > response->limit - response->received) {
    return 0;
  }
  if (response->file) {
    if (fwrite(data, 1, length, response->file) != length) {
      return 0;
    }
    if (response->checksum) {
      g_checksum_update(response->checksum, (const guchar *)data, length);
    }
  } else {
    g_byte_array_append(response->body, (const guint8 *)data, length);
  }
  response->received += length;
  return length;
}

static char *header_value(const char *line, size_t length) {
  const char *colon = memchr(line, ':', length);
  if (!colon) {
    return NULL;
  }
  char *key = g_ascii_strdown(line, colon - line);
  char *value = NULL;
  if (g_str_equal(key, "www-authenticate") || g_str_equal(key, "location") ||
      g_str_equal(key, "retry-after")) {
    value = g_strndup(colon + 1, length - (colon + 1 - line));
  }
  g_free(key);
  if (value) {
    g_strstrip(value);
  }
  return value;
}

static size_t response_header(char *data, size_t size, size_t nmemb,
                              void *opaque) {
  HttpResponse *response = opaque;
  size_t length = size * nmemb;
  char *value = header_value(data, length);
  if (value) {
    if (length >= 16 &&
        g_ascii_strncasecmp(data, "www-authenticate:", 16) == 0) {
      g_free(response->www_authenticate);
      response->www_authenticate = value;
    } else if (length >= 12 &&
               g_ascii_strncasecmp(data, "retry-after:", 12) == 0) {
      g_free(response->retry_after);
      response->retry_after = value;
    } else {
      g_free(response->location);
      response->location = value;
    }
  }
  return length;
}

static void response_clear(HttpResponse *response) {
  if (response->body) {
    g_byte_array_unref(response->body);
  }
  g_free(response->www_authenticate);
  g_free(response->location);
  g_free(response->retry_after);
  g_free(response->content_type);
  memset(response, 0, sizeof(*response));
}

static char *challenge_parameter(const char *challenge, const char *key) {
  if (!challenge) {
    return NULL;
  }
  char *pattern = g_strdup_printf("(?:^|[, ])%s=\\\"([^\\\"]+)\\\"", key);
  GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
  GMatchInfo *match = NULL;
  char *value = NULL;
  if (g_regex_match(regex, challenge, 0, &match)) {
    value = g_match_info_fetch(match, 1);
  }
  g_match_info_free(match);
  g_regex_unref(regex);
  g_free(pattern);
  return value;
}

static gboolean token_realm_accepts_registry_credentials(const char *realm,
                                                          const char *registry) {
  CURLU *url = curl_url();
  if (!url || curl_url_set(url, CURLUPART_URL, realm, 0) != CURLUE_OK) {
    curl_url_cleanup(url);
    return FALSE;
  }
  char *host = NULL;
  gboolean valid = curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK;
  gboolean accepted =
      valid && (g_ascii_strcasecmp(host, registry) == 0 ||
                ((g_str_equal(registry, "registry-1.docker.io") ||
                  g_str_equal(registry, "docker.io") ||
                  g_str_equal(registry, "index.docker.io")) &&
                 g_ascii_strcasecmp(host, "auth.docker.io") == 0));
  curl_free(host);
  curl_url_cleanup(url);
  return accepted;
}

static char *token_from_challenge(const char *challenge,
                                  const char *registry) {
  if (!challenge || !g_str_has_prefix(challenge, "Bearer ")) {
    return NULL;
  }
  char *realm = challenge_parameter(challenge, "realm");
  char *service = challenge_parameter(challenge, "service");
  char *scope = challenge_parameter(challenge, "scope");
  if (!realm || !service || !scope) {
    oci_error("registry Bearer challenge is missing realm, service, or scope");
  }
  if (!realm || !g_str_has_prefix(realm, "https://")) {
    oci_error("registry requested unsupported or insecure authentication");
    g_free(realm);
    g_free(service);
    g_free(scope);
    return NULL;
  }
  CURL *curl = curl_easy_init();
  if (!curl) {
    g_free(realm);
    g_free(service);
    g_free(scope);
    return NULL;
  }
  GString *url = g_string_new(realm);
  char *escaped_service = service ? curl_easy_escape(curl, service, 0) : NULL;
  char *escaped_scope = scope ? curl_easy_escape(curl, scope, 0) : NULL;
  if (escaped_service) {
    g_string_append_printf(url, "%cservice=%s", strchr(realm, '?') ? '&' : '?',
                           escaped_service);
  }
  if (escaped_scope) {
    g_string_append_printf(url, "%cscope=%s", strchr(url->str, '?') ? '&' : '?',
                           escaped_scope);
  }
  HttpResponse response = {.body = g_byte_array_new(), .limit = 1024 * 1024};
  curl_easy_setopt(curl, CURLOPT_URL, url->str);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "quocker/0.1");
  QuockerRegistryCredentials *credentials = NULL;
  GError *credential_error = NULL;
  if (token_realm_accepts_registry_credentials(realm, registry) &&
      quocker_registry_credentials_load(registry, &credentials,
                                        &credential_error) &&
      credentials) {
    char *user_password = g_strdup_printf("%s:%s", credentials->username,
                                          credentials->secret);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, user_password);
    explicit_bzero(user_password, strlen(user_password));
    g_free(user_password);
  } else if (credential_error) {
    oci_error("could not read Docker registry credentials: %s",
              credential_error->message);
    g_clear_error(&credential_error);
    curl_easy_cleanup(curl);
    response_clear(&response);
    curl_free(escaped_service);
    curl_free(escaped_scope);
    g_string_free(url, TRUE);
    g_free(realm);
    g_free(service);
    g_free(scope);
    quocker_registry_credentials_free(credentials);
    return NULL;
  }
  CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  char *token = NULL;
  if (code == CURLE_OK && status == 200) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (json_parser_load_from_data(parser, (const char *)response.body->data,
                                   response.body->len, &error)) {
      JsonNode *root_node = json_parser_get_root(parser);
      JsonObject *root = JSON_NODE_HOLDS_OBJECT(root_node)
                             ? json_node_get_object(root_node)
                             : NULL;
      const char *candidate = NULL;
      candidate = json_string_member(root, "token");
      if (!candidate) {
        candidate = json_string_member(root, "access_token");
      }
      token = g_strdup(candidate);
    } else {
      g_clear_error(&error);
    }
    g_object_unref(parser);
  } else {
    oci_error("registry token request failed (HTTP %ld): %s", status,
              curl_easy_strerror(code));
  }
  curl_easy_cleanup(curl);
  response_clear(&response);
  quocker_registry_credentials_free(credentials);
  curl_free(escaped_service);
  curl_free(escaped_scope);
  g_string_free(url, TRUE);
  g_free(realm);
  g_free(service);
  g_free(scope);
  return token;
}

static gboolean http_request(const char *url, const char *accept,
                             const char *token, HttpResponse *response,
                             long *status_out) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    oci_error("could not initialize HTTPS client");
    return FALSE;
  }
  char *current_url = g_strdup(url);
  char *current_token = g_strdup(token);
  gboolean result = FALSE;
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "quocker/0.1");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
  for (guint redirects = 0; redirects <= 5; redirects++) {
    struct curl_slist *headers = NULL;
    if (accept) {
      char *header = g_strdup_printf("Accept: %s", accept);
      headers = curl_slist_append(headers, header);
      g_free(header);
    }
    if (current_token) {
      char *header = g_strdup_printf("Authorization: Bearer %s", current_token);
      headers = curl_slist_append(headers, header);
      g_free(header);
    }
    curl_easy_setopt(curl, CURLOPT_URL, current_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode code;
    long status = 0;
    for (guint attempt = 0;; attempt++) {
      code = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      *status_out = status;
      gboolean retryable = quocker_registry_http_status_retryable(status) ||
          (code != CURLE_OK && quocker_registry_curl_error_retryable(code));
      if (!retryable) {
        break;
      }
      if (attempt == 3) {
        if (quocker_registry_http_status_retryable(status)) {
          oci_error("registry returned HTTP %ld after 3 retries", status);
        } else {
          oci_error("registry request failed after 3 retries: %s",
                    curl_easy_strerror(code));
        }
        break;
      }

      guint delay = quocker_registry_retry_delay_seconds(response->retry_after,
                                                          attempt,
                                                          time(NULL));
      gboolean reset_ok = TRUE;
      if (response->file) {
        reset_ok = fflush(response->file) == 0 &&
                   ftruncate(fileno(response->file), 0) == 0 &&
                   fseeko(response->file, 0, SEEK_SET) == 0;
        if (reset_ok && response->checksum) {
          g_checksum_reset(response->checksum);
        }
      } else if (response->body) {
        g_byte_array_set_size(response->body, 0);
      }
      response->received = 0;
      g_clear_pointer(&response->www_authenticate, g_free);
      g_clear_pointer(&response->location, g_free);
      g_clear_pointer(&response->retry_after, g_free);
      if (!reset_ok) {
        oci_error("cannot reset temporary registry response for retry");
        break;
      }
      g_usleep((gulong)delay * G_USEC_PER_SEC);
    }
    if (code != CURLE_OK && !quocker_registry_http_status_retryable(status)) {
      if (!quocker_registry_curl_error_retryable(code)) {
        oci_error("request to registry failed: %s", curl_easy_strerror(code));
      }
      curl_slist_free_all(headers);
      break;
    }
    if (quocker_registry_http_status_retryable(status)) {
      curl_slist_free_all(headers);
      break;
    }
    if ((status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308) &&
        response->location) {
      if (redirects == 5) {
        oci_error("registry response exceeded the redirect limit");
        curl_slist_free_all(headers);
        break;
      }
      GError *uri_error = NULL;
      char *next_url = g_uri_resolve_relative(current_url, response->location,
                                              G_URI_FLAGS_NONE, &uri_error);
      GUri *from_uri = g_uri_parse(current_url, G_URI_FLAGS_NONE, NULL);
      GUri *to_uri =
          next_url ? g_uri_parse(next_url, G_URI_FLAGS_NONE, NULL) : NULL;
      if (!next_url || !from_uri || !to_uri ||
          g_strcmp0(g_uri_get_scheme(to_uri), "https") != 0 ||
          !g_uri_get_host(to_uri)) {
        oci_error("registry supplied an invalid or non-HTTPS redirect");
        g_clear_error(&uri_error);
        g_free(next_url);
        if (from_uri)
          g_uri_unref(from_uri);
        if (to_uri)
          g_uri_unref(to_uri);
        curl_slist_free_all(headers);
        break;
      }
      if (g_ascii_strcasecmp(g_uri_get_host(from_uri),
                             g_uri_get_host(to_uri)) != 0 ||
          g_uri_get_port(from_uri) != g_uri_get_port(to_uri)) {
        g_clear_pointer(&current_token, g_free);
      }
      if (response->file) {
        fflush(response->file);
        if (ftruncate(fileno(response->file), 0) != 0 ||
            fseeko(response->file, 0, SEEK_SET) != 0) {
          oci_error("cannot reset temporary blob for registry redirect");
          g_free(next_url);
          g_uri_unref(from_uri);
          g_uri_unref(to_uri);
          if (uri_error)
            g_error_free(uri_error);
          curl_slist_free_all(headers);
          break;
        }
        if (response->checksum) {
          g_checksum_reset(response->checksum);
        }
      } else if (response->body) {
        g_byte_array_set_size(response->body, 0);
      }
      response->received = 0;
      g_clear_pointer(&response->location, g_free);
      g_clear_pointer(&response->www_authenticate, g_free);
      g_clear_pointer(&response->retry_after, g_free);
      g_free(current_url);
      current_url = next_url;
      g_uri_unref(from_uri);
      g_uri_unref(to_uri);
      if (uri_error)
        g_error_free(uri_error);
      curl_slist_free_all(headers);
      continue;
    }
    curl_slist_free_all(headers);
    result = TRUE;
    break;
  }
  g_free(current_url);
  g_free(current_token);
  curl_easy_cleanup(curl);
  return result;
}

static char *registry_url(const OciReference *ref, const char *mirror,
                          const char *suffix) {
  const char *origin = ref->mirror && *ref->mirror ? ref->mirror :
                       mirror && *mirror ? mirror : NULL;
  if (!origin) {
    origin = g_str_equal(ref->registry, "registry-1.docker.io")
                 ? "https://registry-1.docker.io"
                 : NULL;
  }
  if (!origin) {
    origin = g_strdup_printf("https://%s", ref->registry);
  }
  char *base = g_strdup(origin);
  g_strchomp(base);
  while (strlen(base) && base[strlen(base) - 1] == '/') {
    base[strlen(base) - 1] = '\0';
  }
  char *escaped_repo = g_uri_escape_string(ref->repository, "/", TRUE);
  char *result = g_strdup_printf("%s/v2/%s/%s", base, escaped_repo, suffix);
  g_free(escaped_repo);
  if (origin != ref->mirror && origin != mirror && origin != NULL &&
      !g_str_equal(origin, "https://registry-1.docker.io")) {
    g_free((char *)origin);
  }
  g_free(base);
  return result;
}

static GBytes *fetch_manifest(const OciReference *ref, const char *mirror,
                              const char *selector, const char *expected_digest,
                              char **media_type_out) {
  char *suffix = g_strdup_printf("manifests/%s", selector);
  char *url = registry_url(ref, mirror, suffix);
  g_free(suffix);
  HttpResponse response = {.body = g_byte_array_new(),
                           .limit = 16 * 1024 * 1024};
  long status = 0;
  gboolean ok =
      http_request(url, OCI_MANIFEST_ACCEPT, NULL, &response, &status);
  if (ok && status == 401) {
    char *token = NULL;
    if (ref->mirror) {
      oci_error("registry mirror authentication is not configured; refusing "
                "to send source-registry credentials to the mirror");
    } else {
      token = token_from_challenge(response.www_authenticate, ref->registry);
    }
    response_clear(&response);
    response.body = g_byte_array_new();
    response.limit = 16 * 1024 * 1024;
    if (token) {
      ok = http_request(url, OCI_MANIFEST_ACCEPT, token, &response, &status);
    } else {
      ok = FALSE;
    }
    g_free(token);
  }
  if (!ok || status != 200) {
    oci_error("cannot fetch manifest for %s (HTTP %ld)", ref->repository,
              status);
    response_clear(&response);
    g_free(url);
    return NULL;
  }
  if (expected_digest) {
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, response.body->data, response.body->len);
    char *actual =
        g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    if (!g_str_equal(actual, expected_digest)) {
      oci_error("manifest digest mismatch: expected %s, received %s",
                expected_digest, actual);
      g_free(actual);
      response_clear(&response);
      g_free(url);
      return NULL;
    }
    g_free(actual);
  }
  if (media_type_out) {
    *media_type_out = g_strdup("application/json");
  }
  GBytes *bytes = g_byte_array_free_to_bytes(response.body);
  response.body = NULL;
  response_clear(&response);
  g_free(url);
  return bytes;
}

static JsonObject *json_object_from_bytes(GBytes *bytes,
                                          JsonParser **parser_out) {
  gsize length;
  const char *data = g_bytes_get_data(bytes, &length);
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if (!json_parser_load_from_data(parser, data, length, &error)) {
    oci_error("invalid registry JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    oci_error("registry response must be a JSON object");
    g_object_unref(parser);
    return NULL;
  }
  *parser_out = parser;
  return json_node_get_object(root);
}

static OciDescriptor descriptor_read(JsonObject *object) {
  OciDescriptor descriptor = {0};
  if (!object) {
    return descriptor;
  }
  const char *digest = json_string_member(object, "digest");
  JsonNode *size_node = json_object_get_member(object, "size");
  if (!digest || !size_node || !JSON_NODE_HOLDS_VALUE(size_node) ||
      (json_node_get_value_type(size_node) != G_TYPE_INT64 &&
       json_node_get_value_type(size_node) != G_TYPE_INT)) {
    return descriptor;
  }
  descriptor.digest = g_strdup(digest);
  gint64 signed_size = json_node_get_int(size_node);
  if (signed_size >= 0) {
    descriptor.size = signed_size;
  }
  descriptor.media_type = g_strdup(json_string_member(object, "mediaType"));
  if (!descriptor.digest || !g_str_has_prefix(descriptor.digest, "sha256:") ||
      strlen(descriptor.digest) != 71 || signed_size < 0) {
    g_clear_pointer(&descriptor.digest, g_free);
  }
  return descriptor;
}

static void descriptor_clear(OciDescriptor *descriptor) {
  g_free(descriptor->digest);
  g_free(descriptor->media_type);
  memset(descriptor, 0, sizeof(*descriptor));
}

static gboolean cached_blob_valid(const char *path, const char *digest,
                                  guint64 expected_size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return FALSE;
  }
  struct stat st;
  gboolean valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                   (guint64)st.st_size == expected_size;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  guint8 buffer[64 * 1024];
  while (valid) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      valid = FALSE;
      break;
    }
    if (count == 0) {
      break;
    }
    g_checksum_update(checksum, buffer, count);
  }
  if (valid) {
    char *actual =
        g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
    valid = g_str_equal(actual, digest);
    g_free(actual);
  }
  g_checksum_free(checksum);
  close(fd);
  return valid;
}

static guint64 cache_limit(void) {
  const char *setting = g_getenv("QUOCKER_OCI_CACHE_LIMIT");
  if (!setting || !*setting) {
    return 20ULL * 1024 * 1024 * 1024;
  }
  char *end = NULL;
  errno = 0;
  guint64 value = g_ascii_strtoull(setting, &end, 10);
  if (!g_ascii_isdigit(*setting) || errno || !end || *end || value == 0) {
    oci_error("QUOCKER_OCI_CACHE_LIMIT must be a positive byte count");
    return 0;
  }
  return value;
}

static guint64 cache_usage_recursive(const char *directory, guint depth) {
  if (depth > 256) {
    return G_MAXUINT64;
  }
  DIR *dir = opendir(directory);
  if (!dir) {
    return G_MAXUINT64;
  }
  guint64 total = 0;
  while (TRUE) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno) {
        total = G_MAXUINT64;
      }
      break;
    }
    if (g_str_equal(entry->d_name, ".") ||
        g_str_equal(entry->d_name, "..")) {
      continue;
    }
    char *path = g_build_filename(directory, entry->d_name, NULL);
    struct stat st;
    if (lstat(path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        guint64 nested = cache_usage_recursive(path, depth + 1);
        if (G_MAXUINT64 - total < nested) {
          total = G_MAXUINT64;
        } else {
          total += nested;
        }
      } else if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || G_MAXUINT64 - total < (guint64)st.st_size) {
          total = G_MAXUINT64;
        } else {
          total += st.st_size;
        }
      }
    } else if (errno != ENOENT) {
      total = G_MAXUINT64;
    }
    g_free(path);
  }
  if (closedir(dir) < 0) {
    total = G_MAXUINT64;
  }
  return total;
}

static gboolean cache_room(const char *directory, guint64 incoming) {
  guint64 limit = cache_limit();
  guint64 current = cache_usage_recursive(directory, 0);
  if (current == G_MAXUINT64) {
    oci_error("cannot safely account for OCI cache usage in %s", directory);
    return FALSE;
  }
  if (!limit || current > limit || incoming > limit - current) {
    oci_error("OCI cache limit exceeded (%" G_GUINT64_FORMAT
              " bytes used, requesting %" G_GUINT64_FORMAT
              " bytes; set QUOCKER_OCI_CACHE_LIMIT or run 'quocker prune')",
              current, incoming);
    return FALSE;
  }
  return TRUE;
}

static gboolean cache_admit(const char *directory, guint64 incoming) {
  char *blob_root = g_path_get_dirname(directory);
  char *cache_directory = g_path_get_dirname(blob_root);
  gboolean ok = cache_room(cache_directory, incoming);
  g_free(blob_root);
  g_free(cache_directory);
  return ok;
}

gboolean quocker_oci_cache_has_room(const char *cache_directory,
                                    guint64 incoming_bytes) {
  return cache_room(cache_directory, incoming_bytes);
}

static gboolean cache_directory_prepare(const char *path, gboolean create) {
  struct stat st;
  if (create && g_mkdir_with_parents(path, 0700) < 0) {
    return FALSE;
  }
  if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & (S_IWGRP | S_IWOTH))) {
    errno = EPERM;
    return FALSE;
  }
  return TRUE;
}

static int cache_lock(const char *cache_directory) {
  if (!cache_directory_prepare(cache_directory, TRUE)) {
    oci_error("cannot create OCI cache directory %s: %s", cache_directory,
              g_strerror(errno));
    return -1;
  }
  char *path = g_build_filename(cache_directory, ".lock", NULL);
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
      flock(fd, LOCK_EX) < 0) {
    oci_error("cannot lock OCI cache: %s", g_strerror(errno));
    if (fd >= 0) {
      close(fd);
    }
    fd = -1;
  }
  g_free(path);
  return fd;
}

static void cache_unlock(int fd) {
  if (fd >= 0) {
    flock(fd, LOCK_UN);
    close(fd);
  }
}

int quocker_oci_cache_lock(const char *cache_directory) {
  return cache_lock(cache_directory);
}

void quocker_oci_cache_unlock(int lock_fd) { cache_unlock(lock_fd); }

static char *cache_reference_file(const char *cache_directory,
                                  const char *state_directory,
                                  const char *service,
                                  char **state_path_out) {
  char *absolute_directory = g_canonicalize_filename(state_directory, NULL);
  char *state_name = g_strdup_printf("%s.state", service);
  char *state_path = g_build_filename(absolute_directory, state_name, NULL);
  char *identity = g_strdup_printf("%s\n%s", state_path, service);
  char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
  char *references = g_build_filename(cache_directory, "refs", NULL);
  char *filename = g_strdup_printf("%s.json", key);
  char *path = g_build_filename(references, filename, NULL);
  g_free(filename);
  g_free(references);
  g_free(key);
  g_free(identity);
  g_free(state_name);
  g_free(absolute_directory);
  if (state_path_out) {
    *state_path_out = state_path;
  } else {
    g_free(state_path);
  }
  return path;
}

static gboolean cache_disk_mark_unlocked(const char *cache_directory,
                                         const char *disk_path) {
  char *rootfs = g_build_filename(cache_directory, "rootfs", NULL);
  char *rootfs_absolute = g_canonicalize_filename(rootfs, NULL);
  char *disk_absolute = g_canonicalize_filename(disk_path, NULL);
  char *parent = g_path_get_dirname(disk_absolute);
  char *name = g_path_get_basename(disk_absolute);
  gboolean valid = g_str_equal(parent, rootfs_absolute) &&
                   strlen(name) == 69 && g_str_has_suffix(name, ".ext4") &&
                   cache_directory_prepare(rootfs_absolute, FALSE);
  for (guint i = 0; valid && i < 64; i++) {
    valid = g_ascii_isxdigit(name[i]);
  }
  struct stat disk_stat;
  valid = valid && lstat(disk_absolute, &disk_stat) == 0 &&
          S_ISREG(disk_stat.st_mode);
  char *marker_path = g_strconcat(disk_absolute, ".quocker", NULL);
  char *expected = valid ? g_strdup_printf("sha256:%.*s", 64, name) : NULL;
  struct stat marker_stat;
  if (valid && lstat(marker_path, &marker_stat) == 0) {
    gchar *contents = NULL;
    valid = S_ISREG(marker_stat.st_mode) && marker_stat.st_uid == geteuid() &&
            !(marker_stat.st_mode & (S_IWGRP | S_IWOTH)) &&
            g_file_get_contents(marker_path, &contents, NULL, NULL) &&
            g_strcmp0(contents, expected) == 0;
    g_free(contents);
  } else if (valid && errno == ENOENT) {
    valid = g_file_set_contents(marker_path, expected, -1, NULL);
  } else if (valid) {
    valid = FALSE;
  }
  if (!valid) {
    oci_error("cannot mark OCI base disk for safe cache pruning");
  }
  g_free(expected);
  g_free(marker_path);
  g_free(name);
  g_free(parent);
  g_free(disk_absolute);
  g_free(rootfs_absolute);
  g_free(rootfs);
  return valid;
}

static gboolean cache_reference_write(const char *cache_directory,
                                      const char *state_directory,
                                      const char *service,
                                      const char *overlay_path,
                                      const char *base_disk_path) {
  if (!cache_directory || !state_directory || !service || !*service ||
      strchr(service, '/') || !overlay_path || !base_disk_path) {
    oci_error("invalid VM image cache reference parameters");
    return FALSE;
  }
  char *references = g_build_filename(cache_directory, "refs", NULL);
  char *state_path = NULL;
  char *path = cache_reference_file(cache_directory, state_directory, service,
                                    &state_path);
  gboolean ok = service && *service && !strchr(service, '/') &&
                cache_directory_prepare(references, TRUE);
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "service");
  json_builder_add_string_value(builder, service ? service : "");
  json_builder_set_member_name(builder, "state");
  json_builder_add_string_value(builder, state_path);
  json_builder_set_member_name(builder, "overlay");
  char *absolute_overlay = g_canonicalize_filename(overlay_path, NULL);
  json_builder_add_string_value(builder, absolute_overlay);
  g_free(absolute_overlay);
  json_builder_set_member_name(builder, "base");
  char *absolute_base = g_canonicalize_filename(base_disk_path, NULL);
  json_builder_add_string_value(builder, absolute_base);
  g_free(absolute_base);
  json_builder_end_object(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, json_builder_get_root(builder));
  gsize length = 0;
  char *contents = json_generator_to_data(generator, &length);
  if (ok) {
    ok = g_file_set_contents(path, contents, length, NULL);
  }
  if (ok && !cache_disk_mark_unlocked(cache_directory, base_disk_path)) {
    g_unlink(path);
    ok = FALSE;
  }
  if (!ok) {
    oci_error("could not record VM image cache reference");
  }
  g_free(contents);
  g_object_unref(generator);
  g_object_unref(builder);
  g_free(path);
  g_free(state_path);
  g_free(references);
  return ok;
}

gboolean quocker_oci_cache_reference_locked(const char *cache_directory,
                                           const char *state_directory,
                                           const char *service,
                                           const char *overlay_path,
                                           const char *base_disk_path) {
  return cache_reference_write(cache_directory, state_directory, service,
                               overlay_path, base_disk_path);
}

gboolean quocker_oci_cache_reference(const char *cache_directory,
                                    const char *state_directory,
                                    const char *service,
                                    const char *overlay_path,
                                    const char *base_disk_path) {
  if (!cache_directory) {
    oci_error("invalid VM image cache reference parameters");
    return FALSE;
  }
  int lock_fd = cache_lock(cache_directory);
  if (lock_fd < 0) {
    return FALSE;
  }
  gboolean ok = cache_reference_write(cache_directory, state_directory,
                                      service, overlay_path, base_disk_path);
  cache_unlock(lock_fd);
  return ok;
}

gboolean quocker_oci_cache_unreference(const char *cache_directory,
                                       const char *state_directory,
                                       const char *service) {
  if (!cache_directory || !state_directory || !service || !*service ||
      strchr(service, '/')) {
    oci_error("invalid VM image cache reference parameters");
    return FALSE;
  }
  struct stat cache_stat;
  if (lstat(cache_directory, &cache_stat) < 0 && errno == ENOENT) {
    return TRUE;
  }
  int lock_fd = cache_lock(cache_directory);
  if (lock_fd < 0) {
    return FALSE;
  }
  char *path = cache_reference_file(cache_directory, state_directory, service,
                                    NULL);
  gboolean ok = g_unlink(path) == 0 || errno == ENOENT;
  if (!ok) {
    oci_error("could not remove VM image cache reference: %s",
              g_strerror(errno));
  }
  g_free(path);
  cache_unlock(lock_fd);
  return ok;
}

static gboolean cache_reference_read(const char *path, const char *filename,
                                     const char *rootfs_path,
                                     const char *blobs_path,
                                     GHashTable *keep_files,
                                     GHashTable *keep_blobs,
                                     gboolean *stale) {
  *stale = FALSE;
  struct stat ref_stat;
  if (lstat(path, &ref_stat) < 0 || !S_ISREG(ref_stat.st_mode) ||
      ref_stat.st_uid != geteuid() ||
      (ref_stat.st_mode & (S_IWGRP | S_IWOTH)) ||
      ref_stat.st_size > 65536) {
    oci_error("refusing to use an unsafe OCI cache reference %s", filename);
    return FALSE;
  }
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL)) {
    oci_error("cannot read OCI cache reference %s", filename);
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  gboolean parsed = json_parser_load_from_data(parser, contents, length, &error);
  g_free(contents);
  if (!parsed || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    oci_error("invalid OCI cache reference %s", filename);
    g_clear_error(&error);
    g_object_unref(parser);
    return FALSE;
  }
  JsonObject *object = json_node_get_object(json_parser_get_root(parser));
  const char *state = json_string_member(object, "state");
  const char *service = json_string_member(object, "service");
  const char *overlay = json_string_member(object, "overlay");
  const char *base = json_string_member(object, "base");
  if (!state || !g_path_is_absolute(state) || !service || !*service ||
      strchr(service, '/') || !overlay ||
      !g_path_is_absolute(overlay) || !base || !g_path_is_absolute(base)) {
    oci_error("invalid paths in OCI cache reference %s", filename);
    g_object_unref(parser);
    return FALSE;
  }
  char *identity = g_strdup_printf("%s\n%s", state, service);
  char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
  char *expected_name = g_strdup_printf("%s.json", key);
  gboolean valid_key = g_str_equal(filename, expected_name);
  g_free(expected_name);
  g_free(key);
  g_free(identity);
  if (!valid_key || !g_str_has_prefix(base, rootfs_path) ||
      base[strlen(rootfs_path)] != G_DIR_SEPARATOR ||
      strchr(base + strlen(rootfs_path) + 1, G_DIR_SEPARATOR)) {
    oci_error("OCI cache reference %s points outside the image cache",
              filename);
    g_object_unref(parser);
    return FALSE;
  }
  const char *base_name = strrchr(base, G_DIR_SEPARATOR);
  base_name = base_name ? base_name + 1 : base;
  if (strlen(base_name) != 69 || !g_str_has_suffix(base_name, ".ext4")) {
    oci_error("OCI cache reference %s has an invalid base disk name",
              filename);
    g_object_unref(parser);
    return FALSE;
  }
  for (guint i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(base_name[i])) {
      oci_error("OCI cache reference %s has an invalid base disk digest",
                filename);
      g_object_unref(parser);
      return FALSE;
    }
  }
  struct stat overlay_stat;
  struct stat base_stat;
  gboolean overlay_present = lstat(overlay, &overlay_stat) == 0;
  gboolean base_present = lstat(base, &base_stat) == 0;
  if (!overlay_present || !base_present) {
    *stale = TRUE;
    g_object_unref(parser);
    return TRUE;
  }
  if (!S_ISREG(overlay_stat.st_mode) || !S_ISREG(base_stat.st_mode)) {
    oci_error("OCI cache reference %s points to a non-regular VM disk",
              filename);
    g_object_unref(parser);
    return FALSE;
  }
  char *manifest_digest = g_strndup(base_name, 64);
  char *manifest_path = g_build_filename(blobs_path, manifest_digest, NULL);
  struct stat manifest_stat;
  if (lstat(manifest_path, &manifest_stat) < 0 ||
      !S_ISREG(manifest_stat.st_mode) || manifest_stat.st_size < 2 ||
      manifest_stat.st_size > 16 * 1024 * 1024) {
    oci_error("referenced OCI manifest is missing or unsafe for %s", filename);
    g_free(manifest_path);
    g_free(manifest_digest);
    g_object_unref(parser);
    return FALSE;
  }
  gchar *manifest_contents = NULL;
  gsize manifest_length = 0;
  if (!g_file_get_contents(manifest_path, &manifest_contents, &manifest_length,
                           NULL)) {
    oci_error("cannot read referenced OCI manifest for %s", filename);
    g_free(manifest_path);
    g_free(manifest_digest);
    g_object_unref(parser);
    return FALSE;
  }
  GChecksum *manifest_checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(manifest_checksum, (const guchar *)manifest_contents,
                    manifest_length);
  gboolean manifest_valid =
      g_str_equal(g_checksum_get_string(manifest_checksum), manifest_digest);
  g_checksum_free(manifest_checksum);
  JsonParser *manifest_parser = json_parser_new();
  GError *manifest_error = NULL;
  manifest_valid = manifest_valid &&
                   json_parser_load_from_data(manifest_parser,
                                              manifest_contents,
                                              manifest_length,
                                              &manifest_error) &&
                   JSON_NODE_HOLDS_OBJECT(
                       json_parser_get_root(manifest_parser));
  g_free(manifest_contents);
  g_clear_error(&manifest_error);
  if (!manifest_valid) {
    oci_error("referenced OCI manifest failed digest or format validation for %s",
              filename);
    g_object_unref(manifest_parser);
    g_free(manifest_path);
    g_free(manifest_digest);
    g_object_unref(parser);
    return FALSE;
  }
  JsonObject *manifest = json_node_get_object(
      json_parser_get_root(manifest_parser));
  JsonNode *config_node = json_object_get_member(manifest, "config");
  JsonNode *layers_node = json_object_get_member(manifest, "layers");
  if (!JSON_NODE_HOLDS_OBJECT(config_node) ||
      !JSON_NODE_HOLDS_ARRAY(layers_node)) {
    oci_error("referenced OCI manifest has invalid descriptors for %s",
              filename);
    g_object_unref(manifest_parser);
    g_free(manifest_path);
    g_free(manifest_digest);
    g_object_unref(parser);
    return FALSE;
  }
  GPtrArray *descriptor_nodes = g_ptr_array_new();
  g_ptr_array_add(descriptor_nodes, json_object_get_member(
                                       json_node_get_object(config_node),
                                       "digest"));
  JsonArray *layers = json_node_get_array(layers_node);
  for (guint i = 0; i < json_array_get_length(layers); i++) {
    JsonNode *layer_node = json_array_get_element(layers, i);
    JsonNode *digest_node = JSON_NODE_HOLDS_OBJECT(layer_node)
                                ? json_object_get_member(
                                      json_node_get_object(layer_node),
                                      "digest")
                                : NULL;
    g_ptr_array_add(descriptor_nodes, digest_node);
  }
  gboolean descriptors_valid = TRUE;
  g_hash_table_add(keep_blobs, g_strdup(manifest_digest));
  for (guint i = 0; i < descriptor_nodes->len; i++) {
    JsonNode *digest_node = g_ptr_array_index(descriptor_nodes, i);
    const char *digest = digest_node && JSON_NODE_HOLDS_VALUE(digest_node) &&
                                 json_node_get_value_type(digest_node) ==
                                     G_TYPE_STRING
                             ? json_node_get_string(digest_node)
                             : NULL;
    if (!digest || !g_str_has_prefix(digest, "sha256:") ||
        strlen(digest) != 71) {
      descriptors_valid = FALSE;
      break;
    }
    const char *hex = digest + 7;
    for (guint j = 0; j < 64; j++) {
      if (!g_ascii_isxdigit(hex[j])) {
        descriptors_valid = FALSE;
        break;
      }
    }
    if (!descriptors_valid) {
      break;
    }
    g_hash_table_add(keep_blobs, g_strdup(hex));
  }
  g_ptr_array_free(descriptor_nodes, TRUE);
  g_object_unref(manifest_parser);
  g_free(manifest_path);
  g_free(manifest_digest);
  if (!descriptors_valid) {
    oci_error("referenced OCI manifest contains invalid blob digests for %s",
              filename);
    g_object_unref(parser);
    return FALSE;
  }
  g_hash_table_add(keep_files, g_strdup(base_name));
  char *rootfs_name = g_strdup(base_name);
  rootfs_name[strlen(rootfs_name) - strlen(".ext4")] = '\0';
  g_hash_table_add(keep_files, g_strdup(rootfs_name));
  char *completion_name = g_strconcat(rootfs_name, ".complete", NULL);
  g_hash_table_add(keep_files, completion_name);
  char *marker_name = g_strconcat(base_name, ".quocker", NULL);
  g_hash_table_add(keep_files, marker_name);
  g_free(rootfs_name);
  g_object_unref(parser);
  return TRUE;
}

static GHashTable *cache_referenced_disks(const char *cache_directory,
                                          GHashTable *keep_blobs,
                                          gboolean *ok) {
  *ok = FALSE;
  GHashTable *keep_files =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char *references = g_build_filename(cache_directory, "refs", NULL);
  if (lstat(references, &(struct stat){0}) < 0 && errno == ENOENT) {
    *ok = TRUE;
    g_free(references);
    return keep_files;
  }
  if (!cache_directory_prepare(references, FALSE)) {
    oci_error("refusing to read an unsafe OCI cache reference directory");
    g_free(references);
    g_hash_table_destroy(keep_files);
    return NULL;
  }
  char *rootfs_path = g_build_filename(cache_directory, "rootfs", NULL);
  char *rootfs_absolute = g_canonicalize_filename(rootfs_path, NULL);
  char *blobs_root = g_build_filename(cache_directory, "blobs", "sha256", NULL);
  char *blobs_absolute = g_canonicalize_filename(blobs_root, NULL);
  GDir *directory = g_dir_open(references, 0, NULL);
  gboolean valid = directory != NULL;
  const char *entry;
  while (valid && (entry = g_dir_read_name(directory))) {
    if (!g_str_has_suffix(entry, ".json")) {
      oci_error("unexpected file in OCI cache references: %s", entry);
      valid = FALSE;
      break;
    }
    char *path = g_build_filename(references, entry, NULL);
    gboolean stale = FALSE;
    valid = cache_reference_read(path, entry, rootfs_absolute, blobs_absolute,
                                 keep_files, keep_blobs, &stale);
    if (valid && stale && g_unlink(path) < 0 && errno != ENOENT) {
      oci_error("could not remove stale OCI cache reference %s", entry);
      valid = FALSE;
    }
    g_free(path);
  }
  if (directory) {
    g_dir_close(directory);
  }
  g_free(rootfs_absolute);
  g_free(rootfs_path);
  g_free(blobs_absolute);
  g_free(blobs_root);
  g_free(references);
  *ok = valid;
  if (!valid) {
    g_hash_table_destroy(keep_files);
    return NULL;
  }
  return keep_files;
}

static char *bytes_digest(GBytes *bytes) {
  gsize length;
  const guint8 *data = g_bytes_get_data(bytes, &length);
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, data, length);
  char *digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  return digest;
}

static gboolean cache_manifest(GBytes *bytes, const char *digest,
                               const char *cache_directory) {
  gsize length;
  const guint8 *data = g_bytes_get_data(bytes, &length);
  char *path =
      g_build_filename(cache_directory, digest + strlen("sha256:"), NULL);
  if (cached_blob_valid(path, digest, length)) {
    g_free(path);
    return TRUE;
  }
  g_unlink(path);
  if (!cache_admit(cache_directory, length)) {
    g_free(path);
    return FALSE;
  }
  char *temporary = g_strdup_printf("%s/.manifest.XXXXXX", cache_directory);
  int fd = g_mkstemp(temporary);
  gboolean ok = fd >= 0;
  gsize offset = 0;
  while (ok && offset < length) {
    ssize_t written = write(fd, data + offset, length - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ok = FALSE;
    } else {
      offset += written;
    }
  }
  if (ok && fsync(fd) != 0) {
    ok = FALSE;
  }
  if (fd >= 0 && close(fd) != 0) {
    ok = FALSE;
  }
  if (ok && g_rename(temporary, path) < 0) {
    ok = FALSE;
  }
  if (!ok) {
    oci_error("cannot cache selected image manifest: %s", g_strerror(errno));
    g_unlink(temporary);
  }
  g_free(path);
  g_free(temporary);
  return ok;
}

static gboolean platform_match(JsonObject *descriptor, const char *os,
                               const char *architecture) {
  if (!descriptor) {
    return FALSE;
  }
  if (!json_object_has_member(descriptor, "platform")) {
    return FALSE;
  }
  JsonNode *platform_node = json_object_get_member(descriptor, "platform");
  if (!JSON_NODE_HOLDS_OBJECT(platform_node)) {
    return FALSE;
  }
  JsonObject *platform = json_node_get_object(platform_node);
  const char *candidate_os = json_string_member(platform, "os");
  const char *candidate_arch = json_string_member(platform, "architecture");
  const char *candidate_variant = json_string_member(platform, "variant");
  if (!candidate_variant) {
    candidate_variant = "";
  }
  const char *wanted_variant =
      strchr(architecture, '/') ? strchr(architecture, '/') + 1 : "";
  char *wanted_arch = g_strdup(architecture);
  char *slash = strchr(wanted_arch, '/');
  if (slash) {
    *slash = '\0';
  }
  gboolean match =
      g_strcmp0(candidate_os, os) == 0 &&
      g_strcmp0(candidate_arch, wanted_arch) == 0 &&
      (!*wanted_variant || g_strcmp0(candidate_variant, wanted_variant) == 0);
  g_free(wanted_arch);
  return match;
}

static gboolean fetch_blob(const OciReference *ref, const char *mirror,
                           const OciDescriptor *descriptor,
                           const char *cache_directory) {
  const char *hex = descriptor->digest + strlen("sha256:");
  char *final_path = g_build_filename(cache_directory, hex, NULL);
  if (cached_blob_valid(final_path, descriptor->digest, descriptor->size)) {
    g_free(final_path);
    return TRUE;
  }
  /* Individual compressed layers larger than 16 GiB are rejected to bound
   * cache writes from hostile manifests. */
  if (descriptor->size > 16ULL * 1024 * 1024 * 1024) {
    oci_error("refusing OCI blob larger than the 16 GiB per-blob limit");
    g_free(final_path);
    return FALSE;
  }
  /* A bad cache entry may be a symlink. Unlink the directory entry only. */
  g_unlink(final_path);
  if (!cache_admit(cache_directory, descriptor->size)) {
    g_free(final_path);
    return FALSE;
  }
  char *temporary = g_strdup_printf("%s/.%s.XXXXXX", cache_directory, hex);
  int fd = g_mkstemp(temporary);
  if (fd < 0) {
    oci_error("cannot create OCI cache file: %s", g_strerror(errno));
    g_free(final_path);
    g_free(temporary);
    return FALSE;
  }
  FILE *file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
    g_unlink(temporary);
    g_free(final_path);
    g_free(temporary);
    return FALSE;
  }
  HttpResponse response = {.file = file,
                           .checksum = g_checksum_new(G_CHECKSUM_SHA256),
                           .limit = descriptor->size};
  char *suffix = g_strdup_printf("blobs/%s", descriptor->digest);
  char *url = registry_url(ref, mirror, suffix);
  g_free(suffix);
  long status = 0;
  gboolean ok = http_request(url, NULL, NULL, &response, &status);
  if (ok && status == 401) {
    char *token = NULL;
    if (ref->mirror) {
      oci_error("registry mirror authentication is not configured; refusing "
                "to send source-registry credentials to the mirror");
    } else {
      token = token_from_challenge(response.www_authenticate, ref->registry);
    }
    if (token) {
      /* Reset any partial response before retrying an authorized GET. */
      fclose(file);
      file = NULL;
      file = g_fopen(temporary, "wb");
      response.file = file;
      response.received = 0;
      g_checksum_reset(response.checksum);
      g_free(response.www_authenticate);
      response.www_authenticate = NULL;
      if (file) {
        ok = http_request(url, NULL, token, &response, &status);
      } else {
        ok = FALSE;
      }
      g_free(token);
    } else {
      ok = FALSE;
    }
  }
  if (file) {
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
      ok = FALSE;
    }
    if (fclose(file) != 0) {
      ok = FALSE;
    }
  }
  file = NULL;
  const char *digest = g_checksum_get_string(response.checksum);
  char *actual = g_strdup_printf("sha256:%s", digest);
  if (!ok || status != 200 || response.received != descriptor->size ||
      !g_str_equal(actual, descriptor->digest)) {
    oci_error(
        "blob fetch or verification failed for %s (HTTP %ld, %" G_GUINT64_FORMAT
        " of %" G_GUINT64_FORMAT " bytes)%s%s",
        descriptor->digest, status, response.received, descriptor->size,
        response.www_authenticate ? "; challenge: " : "",
        response.www_authenticate ? response.www_authenticate : "");
    ok = FALSE;
  }
  if (ok && g_rename(temporary, final_path) < 0) {
    oci_error("cannot publish cached OCI blob: %s", g_strerror(errno));
    ok = FALSE;
  }
  if (!ok) {
    g_unlink(temporary);
  }
  g_free(actual);
  response_clear(&response);
  g_free(url);
  g_free(final_path);
  g_free(temporary);
  return ok;
}

static gboolean config_platform_valid(const char *cache_directory,
                                      const OciDescriptor *config,
                                      const char *os, const char *architecture,
                                      QuockerImageDefaults **defaults_out) {
  if (defaults_out) {
    *defaults_out = NULL;
  }
  if (config->size > 16 * 1024 * 1024) {
    oci_error("image config exceeds the 16 MiB metadata limit");
    return FALSE;
  }
  char *path = g_build_filename(cache_directory,
                                config->digest + strlen("sha256:"), NULL);
  gchar *data = NULL;
  gsize length = 0;
  GError *error = NULL;
  if (!g_file_get_contents(path, &data, &length, &error)) {
    oci_error("cannot read cached image config: %s", error->message);
    g_error_free(error);
    g_free(path);
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  gboolean valid = FALSE;
  if (json_parser_load_from_data(parser, data, length, &error)) {
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *object = json_node_get_object(root);
      const char *image_os = json_string_member(object, "os");
      const char *image_arch = json_string_member(object, "architecture");
      char *wanted_arch = g_strdup(architecture);
      char *variant = strchr(wanted_arch, '/');
      if (variant) {
        *variant = '\0';
      }
      valid = g_strcmp0(image_os, os) == 0 &&
              g_strcmp0(image_arch, wanted_arch) == 0;
      g_free(wanted_arch);
      if (!valid) {
        oci_error("image config platform %s/%s does not match requested %s/%s",
                  image_os ? image_os : "(missing)",
                  image_arch ? image_arch : "(missing)", os, architecture);
      } else {
        GError *config_error = NULL;
        valid = quocker_image_defaults_parse(data, length, defaults_out,
                                             &config_error);
        if (!valid) {
          oci_error("invalid OCI image runtime config: %s",
                    config_error ? config_error->message : "invalid fields");
          g_clear_error(&config_error);
        }
      }
    }
  } else {
    oci_error("invalid OCI image config JSON: %s", error->message);
    g_error_free(error);
  }
  g_object_unref(parser);
  g_free(data);
  g_free(path);
  return valid;
}

static gboolean platform_parse(const char *value, char **os_out,
                               char **arch_out) {
  struct utsname system;
  char *host_platform = NULL;
  if ((!value || !*value) && uname(&system) == 0) {
    const char *architecture =
        g_str_equal(system.machine, "x86_64")    ? "amd64"
        : g_str_equal(system.machine, "aarch64") ? "arm64"
        : g_str_equal(system.machine, "i686")    ? "386"
        : g_str_equal(system.machine, "armv7l")  ? "arm/v7"
        : g_str_equal(system.machine, "ppc64le") ? "ppc64le"
        : g_str_equal(system.machine, "s390x")   ? "s390x"
                                                 : NULL;
    if (architecture) {
      host_platform = g_strdup_printf("linux/%s", architecture);
    }
  }
  const char *platform = value && *value ? value : host_platform;
  if (!platform) {
    oci_error("host architecture cannot be mapped to a supported OCI platform; "
              "specify service.platform explicitly");
    return FALSE;
  }
  gchar **parts = g_strsplit(platform, "/", 3);
  gboolean valid =
      parts[0] && parts[1] && (g_str_equal(parts[0], "linux")) &&
      (g_str_equal(parts[1], "amd64") || g_str_equal(parts[1], "arm64") ||
       g_str_equal(parts[1], "arm") || g_str_equal(parts[1], "386") ||
       g_str_equal(parts[1], "ppc64le") || g_str_equal(parts[1], "s390x"));
  if (valid) {
    *os_out = g_strdup(parts[0]);
    *arch_out = parts[2] ? g_strdup_printf("%s/%s", parts[1], parts[2])
                         : g_strdup(parts[1]);
  } else {
    oci_error("unsupported OCI platform '%s'", platform);
  }
  g_strfreev(parts);
  g_free(host_platform);
  return valid;
}

void quocker_oci_image_free(QuockerOciImage *image) {
  if (!image) {
    return;
  }
  g_free(image->rootfs_path);
  g_free(image->os);
  g_free(image->architecture);
  g_free(image->manifest_digest);
  g_free(image->config_digest);
  quocker_image_defaults_free(image->defaults);
  g_free(image);
}

gboolean quocker_oci_pull(const char *reference, const char *platform,
                          const char *cache_directory, const char *mirror_url,
                          QuockerOciImage **image_out) {
  if (image_out) {
    *image_out = NULL;
  }
  static gsize curl_initialized;
  if (g_once_init_enter(&curl_initialized)) {
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_once_init_leave(&curl_initialized, code == CURLE_OK ? 1 : 2);
  }
  OciReference ref;
  if (!reference_parse(reference, &ref)) {
    return FALSE;
  }
  char *os = NULL;
  char *architecture = NULL;
  char *resolved_manifest_digest = NULL;
  int lock_fd = -1;
  char *rootfs_path = NULL;
  char *config_digest = NULL;
  if (!platform_parse(platform, &os, &architecture)) {
    reference_clear(&ref);
    return FALSE;
  }
  GError *mirror_error = NULL;
  ref.mirror = quocker_registry_mirror_resolve(ref.registry, mirror_url,
                                                &mirror_error);
  if (mirror_error) {
    oci_error("invalid registry mirror configuration: %s",
              mirror_error->message);
    g_clear_error(&mirror_error);
    g_free(os);
    g_free(architecture);
    reference_clear(&ref);
    return FALSE;
  }
  lock_fd = cache_lock(cache_directory);
  if (lock_fd < 0) {
    g_free(os);
    g_free(architecture);
    reference_clear(&ref);
    return FALSE;
  }
  char *blob_root = g_build_filename(cache_directory, "blobs", NULL);
  char *blobs = g_build_filename(blob_root, "sha256", NULL);
  if (!cache_directory_prepare(blob_root, TRUE) ||
      !cache_directory_prepare(blobs, TRUE)) {
    oci_error("cannot create OCI cache directory %s: %s", blobs,
              g_strerror(errno));
    cache_unlock(lock_fd);
    g_free(blob_root);
    g_free(blobs);
    g_free(os);
    g_free(architecture);
    reference_clear(&ref);
    return FALSE;
  }
  g_free(blob_root);
  GBytes *manifest_bytes =
      fetch_manifest(&ref, mirror_url, ref.selector, NULL, NULL);
  if (!manifest_bytes) {
    goto fail;
  }
  JsonParser *manifest_parser = NULL;
  JsonObject *manifest =
      json_object_from_bytes(manifest_bytes, &manifest_parser);
  if (!manifest) {
    g_bytes_unref(manifest_bytes);
    goto fail;
  }
  if (json_object_has_member(manifest, "manifests")) {
    JsonNode *manifests_node = json_object_get_member(manifest, "manifests");
    if (!JSON_NODE_HOLDS_ARRAY(manifests_node)) {
      oci_error("image index 'manifests' member must be an array");
      g_object_unref(manifest_parser);
      g_bytes_unref(manifest_bytes);
      goto fail;
    }
    JsonArray *manifests = json_node_get_array(manifests_node);
    OciDescriptor selected = {0};
    for (guint i = 0; i < json_array_get_length(manifests); i++) {
      JsonObject *candidate = json_array_get_object_element(manifests, i);
      if (platform_match(candidate, os, architecture)) {
        selected = descriptor_read(candidate);
        if (selected.digest) {
          break;
        }
      }
    }
    if (!selected.digest) {
      oci_error("image %s has no manifest for %s/%s", reference, os,
                architecture);
      g_object_unref(manifest_parser);
      g_bytes_unref(manifest_bytes);
      goto fail;
    }
    char *selector = g_strdup(selected.digest);
    resolved_manifest_digest = g_strdup(selected.digest);
    g_object_unref(manifest_parser);
    g_bytes_unref(manifest_bytes);
    manifest_bytes =
        fetch_manifest(&ref, mirror_url, selector, selected.digest, NULL);
    g_free(selector);
    descriptor_clear(&selected);
    if (!manifest_bytes) {
      goto fail;
    }
    manifest = json_object_from_bytes(manifest_bytes, &manifest_parser);
    if (!manifest) {
      g_bytes_unref(manifest_bytes);
      goto fail;
    }
  }
  JsonNode *config_node = json_object_get_member(manifest, "config");
  JsonNode *layers_node = json_object_get_member(manifest, "layers");
  if (!JSON_NODE_HOLDS_OBJECT(config_node) ||
      !JSON_NODE_HOLDS_ARRAY(layers_node)) {
    oci_error("registry response is not an OCI or Docker v2 image manifest");
    g_object_unref(manifest_parser);
    g_bytes_unref(manifest_bytes);
    goto fail;
  }
  if (!resolved_manifest_digest) {
    resolved_manifest_digest = bytes_digest(manifest_bytes);
  }
  gboolean manifest_cached =
      cache_manifest(manifest_bytes, resolved_manifest_digest, blobs);
  OciDescriptor config = descriptor_read(json_node_get_object(config_node));
  JsonArray *layers = json_node_get_array(layers_node);
  QuockerImageDefaults *image_defaults = NULL;
  config_digest = g_strdup(config.digest);
  gboolean ok =
      manifest_cached && config.digest && config.size <= 16 * 1024 * 1024 &&
      fetch_blob(&ref, mirror_url, &config, blobs) &&
      config_platform_valid(blobs, &config, os, architecture, &image_defaults);
  if (!config.digest || config.size > 16 * 1024 * 1024) {
    oci_error("image config descriptor is invalid or exceeds the 16 MiB limit");
  }
  GPtrArray *layer_paths = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; ok && i < json_array_get_length(layers); i++) {
    OciDescriptor layer =
        descriptor_read(json_array_get_object_element(layers, i));
    if (!layer.digest) {
      oci_error("layer %u has an invalid descriptor", i);
      ok = FALSE;
    } else {
      ok = fetch_blob(&ref, mirror_url, &layer, blobs);
      if (ok) {
        char *path =
            g_build_filename(blobs, layer.digest + strlen("sha256:"), NULL);
        g_ptr_array_add(layer_paths, path);
      }
    }
    descriptor_clear(&layer);
  }
  if (ok) {
    ok = quocker_rootfs_materialize(resolved_manifest_digest, layer_paths,
                                    cache_directory, &rootfs_path);
  }
  if (ok) {
    QuockerDistroInfo distro = {0};
    char *distro_text = NULL;
    if (quocker_rootfs_detect_distro(rootfs_path, &distro)) {
      GString *hints = g_string_new(NULL);
      if (distro.id) {
        g_string_append_printf(hints, "; distro %s%s%s%s", distro.id,
                               distro.version_id ? " " : "",
                               distro.version_id ? distro.version_id : "",
                               distro.id_like ? " (like: " : "");
        if (distro.id_like) {
          g_string_append_printf(hints, "%s)", distro.id_like);
        }
      }
      if (distro.kernel_module_releases->len) {
        g_string_append_printf(hints, "; %u kernel module release(s)",
                               distro.kernel_module_releases->len);
      }
      distro_text = g_string_free(hints, FALSE);
    }
    g_print(
        "Pulled %s (%s/%s%s; manifest %s; config %s; %u layers; rootfs %s)\n",
        reference, os, architecture, distro_text ? distro_text : "",
        resolved_manifest_digest, config.digest, json_array_get_length(layers),
        rootfs_path);
    g_free(distro_text);
    quocker_distro_info_clear(&distro);
  }
  g_ptr_array_free(layer_paths, TRUE);
  if (ok && image_out) {
    QuockerOciImage *image = g_new0(QuockerOciImage, 1);
    image->rootfs_path = g_steal_pointer(&rootfs_path);
    image->os = g_steal_pointer(&os);
    image->architecture = g_steal_pointer(&architecture);
    image->manifest_digest = g_steal_pointer(&resolved_manifest_digest);
    image->config_digest = g_steal_pointer(&config_digest);
    image->defaults = g_steal_pointer(&image_defaults);
    *image_out = image;
  }
  descriptor_clear(&config);
  g_free(rootfs_path);
  g_free(config_digest);
  quocker_image_defaults_free(image_defaults);
  g_object_unref(manifest_parser);
  g_bytes_unref(manifest_bytes);
  g_free(blobs);
  g_free(os);
  g_free(architecture);
  g_free(resolved_manifest_digest);
  reference_clear(&ref);
  cache_unlock(lock_fd);
  return ok;

fail:
  g_free(rootfs_path);
  g_free(config_digest);
  g_free(blobs);
  g_free(os);
  g_free(architecture);
  g_free(resolved_manifest_digest);
  reference_clear(&ref);
  cache_unlock(lock_fd);
  return FALSE;
}

gboolean quocker_oci_cache_prune(const char *cache_directory) {
  int lock_fd = cache_lock(cache_directory);
  if (lock_fd < 0) {
    return FALSE;
  }
  gboolean references_ok = FALSE;
  GHashTable *keep_blobs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *keep_disks =
      cache_referenced_disks(cache_directory, keep_blobs, &references_ok);
  if (!references_ok || !keep_disks) {
    if (keep_disks) {
      g_hash_table_destroy(keep_disks);
    }
    g_hash_table_destroy(keep_blobs);
    cache_unlock(lock_fd);
    return FALSE;
  }
  guint64 removed_bytes = 0;
  guint removed_files = 0;
  char *blob_root = g_build_filename(cache_directory, "blobs", NULL);
  struct stat st;
  if (lstat(blob_root, &st) == 0 &&
      !cache_directory_prepare(blob_root, FALSE)) {
    oci_error("refusing to prune an unsafe OCI cache directory");
    g_free(blob_root);
    g_hash_table_destroy(keep_disks);
    g_hash_table_destroy(keep_blobs);
    cache_unlock(lock_fd);
    return FALSE;
  }
  char *blobs = g_build_filename(blob_root, "sha256", NULL);
  g_free(blob_root);
  if (lstat(blobs, &st) == 0 && !cache_directory_prepare(blobs, FALSE)) {
    oci_error("refusing to prune an unsafe OCI blob directory");
    g_free(blobs);
    g_hash_table_destroy(keep_disks);
    g_hash_table_destroy(keep_blobs);
    cache_unlock(lock_fd);
    return FALSE;
  }
  GDir *dir = g_dir_open(blobs, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      char *path = g_build_filename(blobs, name, NULL);
      struct stat entry_stat;
      if (!g_hash_table_contains(keep_blobs, name) &&
          lstat(path, &entry_stat) == 0 && !S_ISDIR(entry_stat.st_mode) &&
          g_unlink(path) == 0) {
        if (S_ISREG(entry_stat.st_mode)) {
          removed_bytes += entry_stat.st_size;
        }
        removed_files++;
      }
      g_free(path);
    }
    g_dir_close(dir);
  }
  g_free(blobs);
  guint64 rootfs_bytes = 0;
  if (!quocker_rootfs_cache_prune_except(cache_directory, keep_disks,
                                         &rootfs_bytes)) {
    g_hash_table_destroy(keep_disks);
    g_hash_table_destroy(keep_blobs);
    cache_unlock(lock_fd);
    return FALSE;
  }
  guint kept_disks = g_hash_table_size(keep_disks);
  g_hash_table_destroy(keep_disks);
  g_hash_table_destroy(keep_blobs);
  removed_bytes += rootfs_bytes;
  g_print("Removed OCI cache content (%u registry blobs, %" G_GUINT64_FORMAT
          " bytes); retained %u referenced VM base disk(s)\n",
          removed_files, removed_bytes, kept_disks);
  cache_unlock(lock_fd);
  return TRUE;
}
