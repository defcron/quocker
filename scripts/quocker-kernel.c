/*
 * Quocker guest kernel catalog selection.
 *
 * Catalogs are local administrator-controlled policy. Kernel and initrd
 * digests are checked before a selection is returned.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-kernel.h"

static gboolean file_digest_matches(const char *path, const char *expected,
                                    GError **error);

#include <errno.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KERNEL_CATALOG_MAX_BYTES (1024 * 1024)
#define KERNEL_ASSET_MAX_BYTES (1024ULL * 1024 * 1024)
#define INITRD_ASSET_MAX_BYTES (512ULL * 1024 * 1024)
#define KERNEL_SIGNATURE_MAX_BYTES 1024

static void curl_initialize(void);

typedef struct KernelCandidate {
  QuockerKernel *kernel;
  GPtrArray *distro_ids;
  gint priority;
  guint distro_score;
  guint module_score;
} KernelCandidate;

static GQuark kernel_error_quark(void) {
  return g_quark_from_static_string("quocker-kernel-error");
}

static void set_kernel_error(GError **error, const char *format, ...)
    G_GNUC_PRINTF(2, 3);

static void set_kernel_error(GError **error, const char *format, ...) {
  if (!error || *error) {
    return;
  }
  va_list args;
  va_start(args, format);
  char *message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error_literal(error, kernel_error_quark(), 1, message);
  g_free(message);
}

void quocker_kernel_free(QuockerKernel *kernel) {
  if (!kernel) {
    return;
  }
  g_free(kernel->id);
  g_free(kernel->os);
  g_free(kernel->architecture);
  g_free(kernel->variant);
  g_free(kernel->version);
  if (kernel->features) {
    g_ptr_array_free(kernel->features, TRUE);
  }
  if (kernel->module_releases) {
    g_ptr_array_free(kernel->module_releases, TRUE);
  }
  if (kernel->observed_module_releases) {
    g_ptr_array_free(kernel->observed_module_releases, TRUE);
  }
  g_free(kernel->kernel_path);
  g_free(kernel->kernel_url);
  g_free(kernel->kernel_digest);
  g_free(kernel->initrd_path);
  g_free(kernel->initrd_url);
  g_free(kernel->initrd_digest);
  g_free(kernel->reason);
  g_free(kernel);
}

gboolean quocker_kernel_verify_assets(const QuockerKernel *kernel,
                                      GError **error) {
  if (!kernel || !kernel->kernel_path || !kernel->kernel_digest ||
      !kernel->initrd_path || !kernel->initrd_digest) {
    set_kernel_error(error, "selected kernel is missing a verified asset");
    return FALSE;
  }
  return file_digest_matches(kernel->kernel_path, kernel->kernel_digest,
                             error) &&
         file_digest_matches(kernel->initrd_path, kernel->initrd_digest, error);
}

static void kernel_candidate_free(KernelCandidate *candidate) {
  if (!candidate) {
    return;
  }
  quocker_kernel_free(candidate->kernel);
  g_ptr_array_free(candidate->distro_ids, TRUE);
  g_free(candidate);
}

static gboolean valid_identifier(const char *value) {
  if (!value || !*value || strlen(value) > 128) {
    return FALSE;
  }
  for (const char *p = value; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean valid_kernel_version(const char *value) {
  if (!value || !*value || strlen(value) > 64) {
    return FALSE;
  }
  gboolean have_digit = FALSE;
  guint component_digits = 0;
  for (const char *p = value; *p; p++) {
    if (g_ascii_isdigit(*p)) {
      have_digit = TRUE;
      if (++component_digits > 10) {
        return FALSE;
      }
    } else if (*p == '.' && have_digit && p[1]) {
      have_digit = FALSE;
      component_digits = 0;
    } else {
      return FALSE;
    }
  }
  return have_digit;
}

static gboolean valid_module_release(const char *value) {
  if (!value || !*value || strlen(value) > 128) {
    return FALSE;
  }
  for (const char *p = value; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '+' ||
          *p == '-')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gint compare_kernel_versions(const char *left, const char *right) {
  gchar **left_parts = g_strsplit(left, ".", -1);
  gchar **right_parts = g_strsplit(right, ".", -1);
  guint i = 0;
  gint comparison = 0;
  while (left_parts[i] || right_parts[i]) {
    guint64 left_value =
        left_parts[i] ? g_ascii_strtoull(left_parts[i], NULL, 10) : 0;
    guint64 right_value =
        right_parts[i] ? g_ascii_strtoull(right_parts[i], NULL, 10) : 0;
    if (left_value != right_value) {
      comparison = left_value > right_value ? 1 : -1;
      break;
    }
    i++;
  }
  g_strfreev(left_parts);
  g_strfreev(right_parts);
  return comparison;
}

static gboolean string_array_has(const GPtrArray *array, const char *value) {
  for (guint i = 0; array && i < array->len; i++) {
    if (g_str_equal(value, g_ptr_array_index((GPtrArray *)array, i))) {
      return TRUE;
    }
  }
  return FALSE;
}

static char *canonical_architecture(const char *architecture) {
  if (g_str_equal(architecture, "amd64") ||
      g_str_equal(architecture, "x86_64")) {
    return g_strdup("x86_64");
  }
  if (g_str_equal(architecture, "arm64") ||
      g_str_equal(architecture, "aarch64")) {
    return g_strdup("aarch64");
  }
  if (g_str_equal(architecture, "386") || g_str_equal(architecture, "i386")) {
    return g_strdup("i386");
  }
  if (g_str_equal(architecture, "arm") ||
      g_str_equal(architecture, "ppc64le") ||
      g_str_equal(architecture, "s390x") ||
      g_str_equal(architecture, "riscv64") ||
      g_str_equal(architecture, "loong64")) {
    return g_strdup(architecture);
  }
  return NULL;
}

static gboolean parse_platform(const char *platform, char **os_out,
                               char **architecture_out, char **variant_out) {
  gchar **parts = g_strsplit(platform ? platform : "", "/", 3);
  guint count = g_strv_length(parts);
  if (count < 2 || count > 3 || !*parts[0] || !*parts[1] ||
      (count == 3 && !*parts[2])) {
    g_strfreev(parts);
    return FALSE;
  }
  char *arch = canonical_architecture(parts[1]);
  if (!arch) {
    g_strfreev(parts);
    return FALSE;
  }
  *os_out = g_ascii_strdown(parts[0], -1);
  *architecture_out = arch;
  *variant_out = count == 3 ? g_ascii_strdown(parts[2], -1) : NULL;
  g_strfreev(parts);
  return TRUE;
}

static const char *json_string(JsonObject *object, const char *key) {
  if (!json_object_has_member(object, key)) {
    return NULL;
  }
  JsonNode *node = json_object_get_member(object, key);
  return JSON_NODE_HOLDS_VALUE(node) &&
                 json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

static gboolean valid_sha256(const char *digest) {
  if (!digest || strlen(digest) != 71 || !g_str_has_prefix(digest, "sha256:")) {
    return FALSE;
  }
  for (const char *p = digest + 7; *p; p++) {
    if (!g_ascii_isxdigit(*p)) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean valid_https_url(const char *url, gboolean catalog_source) {
  if (!url || !*url || strlen(url) > 8192) {
    return FALSE;
  }
  curl_initialize();
  CURLU *parsed = curl_url();
  if (!parsed) {
    return FALSE;
  }
  CURLUcode result = curl_url_set(parsed, CURLUPART_URL, url, 0);
  char *scheme = NULL;
  char *host = NULL;
  char *user = NULL;
  char *password = NULL;
  char *fragment = NULL;
  char *query = NULL;
  gboolean valid = result == CURLUE_OK &&
                   curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) ==
                       CURLUE_OK &&
                   curl_url_get(parsed, CURLUPART_HOST, &host, 0) ==
                       CURLUE_OK &&
                   g_ascii_strcasecmp(scheme, "https") == 0 && host && *host &&
                   curl_url_get(parsed, CURLUPART_USER, &user, 0) !=
                       CURLUE_OK &&
                   curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) !=
                       CURLUE_OK &&
                   curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) !=
                       CURLUE_OK;
  if (catalog_source &&
      curl_url_get(parsed, CURLUPART_QUERY, &query, 0) == CURLUE_OK) {
    valid = FALSE;
  }
  curl_free(scheme);
  curl_free(host);
  curl_free(user);
  curl_free(password);
  curl_free(fragment);
  curl_free(query);
  curl_url_cleanup(parsed);
  return valid;
}

static char *asset_cache_path(const char *digest) {
  return g_build_filename(g_get_user_cache_dir(), "quocker", "kernels",
                          digest + strlen("sha256:"), NULL);
}

static gboolean file_digest_matches(const char *path, const char *expected,
                                    GError **error) {
  if (!g_path_is_absolute(path) || !valid_sha256(expected)) {
    set_kernel_error(error,
                     "kernel catalog contains an invalid path or digest");
    return FALSE;
  }
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    set_kernel_error(error, "cannot open kernel asset %s: %s", path,
                     g_strerror(errno));
    return FALSE;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    set_kernel_error(error, "kernel asset %s is not a regular file", path);
    return FALSE;
  }
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  guchar buffer[128 * 1024];
  gboolean ok = TRUE;
  while (TRUE) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      set_kernel_error(error, "cannot read kernel asset %s: %s", path,
                       g_strerror(errno));
      ok = FALSE;
      break;
    }
    if (!count) {
      break;
    }
    g_checksum_update(checksum, buffer, count);
  }
  close(fd);
  if (ok) {
    char *actual =
        g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
    ok = g_ascii_strcasecmp(actual, expected) == 0;
    if (!ok) {
      set_kernel_error(error, "kernel asset digest mismatch for %s", path);
    }
    g_free(actual);
  }
  g_checksum_free(checksum);
  return ok;
}

typedef struct KernelDownload {
  int fd;
  guint64 received;
  guint64 limit;
} KernelDownload;

typedef struct KernelBuffer {
  GByteArray *data;
  gsize limit;
} KernelBuffer;

static size_t kernel_write_file(char *data, size_t size, size_t count,
                                void *opaque) {
  KernelDownload *download = opaque;
  if (size && count > G_MAXSIZE / size) {
    return 0;
  }
  size_t length = size * count;
  if ((guint64)length > download->limit - download->received) {
    return 0;
  }
  size_t offset = 0;
  while (offset < length) {
    ssize_t written = write(download->fd, data + offset, length - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return 0;
    }
    offset += written;
  }
  download->received += length;
  return length;
}

static size_t kernel_write_buffer(char *data, size_t size, size_t count,
                                  void *opaque) {
  KernelBuffer *buffer = opaque;
  if (size && count > G_MAXSIZE / size) {
    return 0;
  }
  size_t length = size * count;
  if (length > buffer->limit - buffer->data->len) {
    return 0;
  }
  g_byte_array_append(buffer->data, (const guint8 *)data, length);
  return length;
}

static void curl_initialize(void) {
  static gsize initialized;
  if (g_once_init_enter(&initialized)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_once_init_leave(&initialized, 1);
  }
}

static gboolean download_verified_asset(const char *url, const char *path,
                                        const char *digest, guint64 expected,
                                        guint64 limit, GError **error) {
  if (!valid_https_url(url, FALSE) || !g_path_is_absolute(path) ||
      !valid_sha256(digest) || expected == 0 || expected > limit) {
    set_kernel_error(error, "signed catalog has invalid kernel asset metadata");
    return FALSE;
  }
  struct stat st;
  if (lstat(path, &st) == 0) {
    return file_digest_matches(path, digest, error);
  }
  if (errno != ENOENT) {
    set_kernel_error(error, "cannot inspect kernel cache asset %s: %s", path,
                     g_strerror(errno));
    return FALSE;
  }
  char *directory = g_path_get_dirname(path);
  if (g_mkdir_with_parents(directory, 0700) < 0 || lstat(directory, &st) < 0 ||
      !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & (S_IWGRP | S_IWOTH))) {
    set_kernel_error(error, "kernel cache directory %s is unsafe", directory);
    g_free(directory);
    return FALSE;
  }
  char *temporary = g_strdup_printf("%s/.quocker-kernel-XXXXXX", directory);
  int fd = g_mkstemp(temporary);
  if (fd < 0) {
    set_kernel_error(error, "cannot create kernel cache file in %s: %s",
                     directory, g_strerror(errno));
    g_free(temporary);
    g_free(directory);
    return FALSE;
  }
  KernelDownload download = {.fd = fd, .limit = expected};
  curl_initialize();
  CURL *curl = curl_easy_init();
  CURLcode result = CURLE_FAILED_INIT;
  long status = 0;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)expected);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "quocker/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, kernel_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
  }
  gboolean ok = result == CURLE_OK && status >= 200 && status < 300 &&
                download.received == expected && fchmod(fd, 0600) == 0 &&
                fsync(fd) == 0;
  if (close(fd) < 0) {
    ok = FALSE;
  }
  if (!ok) {
    set_kernel_error(error,
                     "kernel asset download failed (%s, HTTP %ld, %" G_GUINT64_FORMAT
                     "/%" G_GUINT64_FORMAT " bytes)",
                     curl_easy_strerror(result), status, download.received,
                     expected);
  } else if (!file_digest_matches(temporary, digest, error)) {
    ok = FALSE;
  } else if (g_rename(temporary, path) < 0) {
    set_kernel_error(error, "cannot publish kernel asset %s: %s", path,
                     g_strerror(errno));
    ok = FALSE;
  }
  if (!ok) {
    g_unlink(temporary);
  }
  g_free(temporary);
  g_free(directory);
  return ok;
}

gboolean quocker_kernel_fetch_assets(QuockerKernel *kernel,
                                     GError **error) {
  if (!kernel) {
    set_kernel_error(error, "selected kernel is required");
    return FALSE;
  }
  gboolean ok = TRUE;
  if (kernel->kernel_url) {
    ok = download_verified_asset(kernel->kernel_url, kernel->kernel_path,
                                 kernel->kernel_digest, kernel->kernel_size,
                                 KERNEL_ASSET_MAX_BYTES, error);
  }
  if (ok && kernel->initrd_url) {
    ok = download_verified_asset(kernel->initrd_url, kernel->initrd_path,
                                 kernel->initrd_digest, kernel->initrd_size,
                                 INITRD_ASSET_MAX_BYTES, error);
  }
  if (ok) {
    ok = quocker_kernel_verify_assets(kernel, error);
    kernel->assets_verified = ok;
  }
  return ok;
}

static gboolean read_regular_file(const char *path, gsize limit,
                                  gboolean trusted_owner, gchar **data_out,
                                  gsize *size_out, GError **error) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (guint64)st.st_size > limit ||
      (trusted_owner && ((st.st_uid != 0 && st.st_uid != geteuid()) ||
                         (st.st_mode & (S_IWGRP | S_IWOTH))))) {
    set_kernel_error(error, "file %s is missing, unsafe, or too large", path);
    if (fd >= 0) {
      close(fd);
    }
    return FALSE;
  }
  gsize size = (gsize)st.st_size;
  gchar *data = g_malloc(size + 1);
  gsize offset = 0;
  while (offset < size) {
    ssize_t count = read(fd, data + offset, size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    offset += count;
  }
  close(fd);
  if (offset != size || memchr(data, '\0', size)) {
    set_kernel_error(error, "file %s is truncated or contains NUL bytes", path);
    g_free(data);
    return FALSE;
  }
  data[size] = '\0';
  *data_out = data;
  *size_out = size;
  return TRUE;
}

static gboolean verify_catalog_signature_text(const char *public_key_path,
                                              const gchar *catalog,
                                              gsize catalog_size,
                                              const char *signature_text_in,
                                              gsize signature_text_size,
                                              GError **error) {
  if (!public_key_path || !*public_key_path) {
    set_kernel_error(error, "a trusted kernel catalog public key is required");
    return FALSE;
  }
  gchar *key_pem = NULL;
  gsize key_size = 0;
  if (!read_regular_file(public_key_path, 64 * 1024, TRUE, &key_pem, &key_size,
                         error)) {
    return FALSE;
  }
  BIO *key_bio = BIO_new_mem_buf(key_pem, key_size);
  EVP_PKEY *key =
      key_bio ? PEM_read_bio_PUBKEY(key_bio, NULL, NULL, NULL) : NULL;
  BIO_free(key_bio);
  g_free(key_pem);
  if (!key || EVP_PKEY_base_id(key) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(key);
    set_kernel_error(error, "kernel catalog key must be an Ed25519 public key");
    return FALSE;
  }

  if (!signature_text_in || signature_text_size > KERNEL_SIGNATURE_MAX_BYTES ||
      memchr(signature_text_in, '\0', signature_text_size)) {
    EVP_PKEY_free(key);
    set_kernel_error(error, "kernel catalog signature is missing or too large");
    return FALSE;
  }
  gchar *signature_text = g_strndup(signature_text_in, signature_text_size);
  g_strstrip(signature_text);
  for (const char *p = signature_text; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '+' || *p == '/' || *p == '=')) {
      g_free(signature_text);
      EVP_PKEY_free(key);
      set_kernel_error(error, "kernel catalog signature is not valid base64");
      return FALSE;
    }
  }
  gsize signature_size = 0;
  guchar *signature = g_base64_decode(signature_text, &signature_size);
  g_free(signature_text);
  if (signature_size != 64) {
    g_free(signature);
    EVP_PKEY_free(key);
    set_kernel_error(error, "kernel catalog signature must be 64 bytes");
    return FALSE;
  }
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  gboolean valid =
      context && EVP_DigestVerifyInit(context, NULL, NULL, NULL, key) == 1 &&
      EVP_DigestVerify(context, signature, signature_size,
                       (const unsigned char *)catalog, catalog_size) == 1;
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  g_free(signature);
  if (!valid) {
    set_kernel_error(error, "kernel catalog signature verification failed");
  }
  return valid;
}

static gboolean verify_catalog_signature(const char *catalog_path,
                                         const char *public_key_path,
                                         const gchar *catalog,
                                         gsize catalog_size, GError **error) {
  char *signature_path = g_strconcat(catalog_path, ".sig", NULL);
  gchar *signature_text = NULL;
  gsize signature_text_size = 0;
  if (!read_regular_file(signature_path, KERNEL_SIGNATURE_MAX_BYTES, FALSE,
                         &signature_text, &signature_text_size, error)) {
    g_free(signature_path);
    return FALSE;
  }
  g_free(signature_path);
  gboolean ok = verify_catalog_signature_text(
      public_key_path, catalog, catalog_size, signature_text,
      signature_text_size, error);
  g_free(signature_text);
  return ok;
}

static gboolean catalog_asset_size(JsonObject *object, const char *field,
                                   guint64 limit, const char *url,
                                   guint64 *size_out, GError **error) {
  *size_out = 0;
  if (!url) {
    if (json_object_has_member(object, field)) {
      set_kernel_error(error, "kernel catalog %s requires a corresponding URL",
                       field);
      return FALSE;
    }
    return TRUE;
  }
  JsonNode *node = json_object_get_member(object, field);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_INT64 ||
      json_node_get_int(node) <= 0 || (guint64)json_node_get_int(node) > limit) {
    set_kernel_error(error, "kernel catalog %s is missing or out of range",
                     field);
    return FALSE;
  }
  *size_out = (guint64)json_node_get_int(node);
  return TRUE;
}

static KernelCandidate *candidate_parse(JsonObject *object, GError **error) {
  const char *id = json_string(object, "id");
  const char *os = json_string(object, "os");
  const char *architecture = json_string(object, "architecture");
  const char *variant = json_string(object, "variant");
  const char *kernel_path = json_string(object, "kernel");
  const char *kernel_url = json_string(object, "kernel_url");
  const char *kernel_digest = json_string(object, "kernel_sha256");
  const char *initrd_path = json_string(object, "initrd");
  const char *initrd_url = json_string(object, "initrd_url");
  const char *initrd_digest = json_string(object, "initrd_sha256");
  if ((json_object_has_member(object, "kernel") && !kernel_path) ||
      (json_object_has_member(object, "kernel_url") && !kernel_url) ||
      (json_object_has_member(object, "initrd") && !initrd_path) ||
      (json_object_has_member(object, "initrd_url") && !initrd_url)) {
    set_kernel_error(error, "kernel catalog asset paths and URLs must be strings");
    return NULL;
  }
  if (!valid_identifier(id) || !os || !architecture ||
      (!kernel_path && !kernel_url) || (!initrd_path && !initrd_url) ||
      (kernel_path && kernel_url) || (initrd_path && initrd_url) ||
      !kernel_digest || !initrd_digest || !valid_sha256(kernel_digest) ||
      !valid_sha256(initrd_digest)) {
    set_kernel_error(error, "kernel catalog entry is missing a required field");
    return NULL;
  }
  if ((kernel_url && !valid_https_url(kernel_url, FALSE)) ||
      (initrd_url && !valid_https_url(initrd_url, FALSE))) {
    set_kernel_error(error, "kernel catalog asset URLs must use HTTPS");
    return NULL;
  }
  guint64 kernel_size = 0;
  guint64 initrd_size = 0;
  if (!catalog_asset_size(object, "kernel_size", KERNEL_ASSET_MAX_BYTES,
                          kernel_url, &kernel_size, error) ||
      !catalog_asset_size(object, "initrd_size", INITRD_ASSET_MAX_BYTES,
                          initrd_url, &initrd_size, error)) {
    return NULL;
  }
  char *cached_kernel_path = kernel_url ? asset_cache_path(kernel_digest) : NULL;
  char *cached_initrd_path = initrd_url ? asset_cache_path(initrd_digest) : NULL;
  char *canonical = canonical_architecture(architecture);
  if (!canonical || !valid_identifier(os) ||
      (variant && !valid_identifier(variant))) {
    g_free(canonical);
    g_free(cached_kernel_path);
    g_free(cached_initrd_path);
    set_kernel_error(error, "kernel catalog entry has an invalid platform");
    return NULL;
  }
  KernelCandidate *candidate = g_new0(KernelCandidate, 1);
  candidate->kernel = g_new0(QuockerKernel, 1);
  candidate->kernel->id = g_strdup(id);
  candidate->kernel->os = g_ascii_strdown(os, -1);
  candidate->kernel->architecture = canonical;
  candidate->kernel->variant = variant ? g_ascii_strdown(variant, -1) : NULL;
  candidate->kernel->features = g_ptr_array_new_with_free_func(g_free);
  candidate->kernel->module_releases = g_ptr_array_new_with_free_func(g_free);
  candidate->kernel->kernel_path =
      g_strdup(kernel_path ? kernel_path : cached_kernel_path);
  candidate->kernel->kernel_url = g_strdup(kernel_url);
  candidate->kernel->kernel_size = kernel_size;
  candidate->kernel->kernel_digest = g_strdup(kernel_digest);
  candidate->kernel->initrd_path =
      g_strdup(initrd_path ? initrd_path : cached_initrd_path);
  candidate->kernel->initrd_url = g_strdup(initrd_url);
  candidate->kernel->initrd_size = initrd_size;
  candidate->kernel->initrd_digest = g_strdup(initrd_digest);
  candidate->distro_ids = g_ptr_array_new_with_free_func(g_free);
  g_free(cached_kernel_path);
  cached_kernel_path = NULL;
  g_free(cached_initrd_path);
  cached_initrd_path = NULL;
  if (json_object_has_member(object, "version")) {
    const char *version = json_string(object, "version");
    if (!valid_kernel_version(version)) {
      set_kernel_error(error,
                       "kernel catalog version must be numeric dotted form");
      kernel_candidate_free(candidate);
      return NULL;
    }
    candidate->kernel->version = g_strdup(version);
  }
  if (json_object_has_member(object, "features")) {
    JsonNode *features_node = json_object_get_member(object, "features");
    if (!JSON_NODE_HOLDS_ARRAY(features_node)) {
      set_kernel_error(error, "kernel catalog features must be an array");
      kernel_candidate_free(candidate);
      return NULL;
    }
    JsonArray *features = json_node_get_array(features_node);
    for (guint i = 0; i < json_array_get_length(features); i++) {
      const char *feature = json_array_get_string_element(features, i);
      if (!valid_identifier(feature)) {
        set_kernel_error(error, "kernel catalog has an invalid feature");
        kernel_candidate_free(candidate);
        return NULL;
      }
      if (!string_array_has(candidate->kernel->features, feature)) {
        g_ptr_array_add(candidate->kernel->features, g_strdup(feature));
      }
    }
  }
  if (json_object_has_member(object, "module_releases")) {
    JsonNode *releases_node = json_object_get_member(object, "module_releases");
    if (!JSON_NODE_HOLDS_ARRAY(releases_node)) {
      set_kernel_error(error,
                       "kernel catalog module_releases must be an array");
      kernel_candidate_free(candidate);
      return NULL;
    }
    JsonArray *releases = json_node_get_array(releases_node);
    for (guint i = 0; i < json_array_get_length(releases); i++) {
      const char *release = json_array_get_string_element(releases, i);
      if (!valid_module_release(release)) {
        set_kernel_error(error, "kernel catalog has an invalid module release");
        kernel_candidate_free(candidate);
        return NULL;
      }
      if (!string_array_has(candidate->kernel->module_releases, release)) {
        g_ptr_array_add(candidate->kernel->module_releases, g_strdup(release));
      }
    }
  }
  if (json_object_has_member(object, "distro_ids")) {
    JsonNode *node = json_object_get_member(object, "distro_ids");
    if (!JSON_NODE_HOLDS_ARRAY(node)) {
      set_kernel_error(error, "kernel catalog distro_ids must be an array");
      kernel_candidate_free(candidate);
      return NULL;
    }
    JsonArray *ids = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(ids); i++) {
      const char *distro_id = json_array_get_string_element(ids, i);
      if (!valid_identifier(distro_id)) {
        set_kernel_error(error, "kernel catalog has an invalid distro ID");
        kernel_candidate_free(candidate);
        return NULL;
      }
      g_ptr_array_add(candidate->distro_ids, g_ascii_strdown(distro_id, -1));
    }
  }
  if (json_object_has_member(object, "priority")) {
    JsonNode *node = json_object_get_member(object, "priority");
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64) {
      set_kernel_error(error, "kernel catalog priority must be an integer");
      kernel_candidate_free(candidate);
      return NULL;
    }
    gint64 priority = json_node_get_int(node);
    if (priority < G_MININT || priority > G_MAXINT) {
      set_kernel_error(error, "kernel catalog priority is outside its range");
      kernel_candidate_free(candidate);
      return NULL;
    }
    candidate->priority = priority;
  }
  if (!g_path_is_absolute(candidate->kernel->kernel_path) ||
      !g_path_is_absolute(candidate->kernel->initrd_path)) {
    set_kernel_error(
        error, "kernel catalog entry has an invalid asset path or digest");
    kernel_candidate_free(candidate);
    g_free(cached_kernel_path);
    g_free(cached_initrd_path);
    return NULL;
  }
  g_free(cached_kernel_path);
  g_free(cached_initrd_path);
  return candidate;
}

static gboolean download_https_buffer(const char *url, gsize limit,
                                      gchar **contents_out, gsize *size_out,
                                      GError **error) {
  *contents_out = NULL;
  *size_out = 0;
  if (!valid_https_url(url, FALSE)) {
    set_kernel_error(error, "kernel download URL must use HTTPS");
    return FALSE;
  }
  KernelBuffer buffer = {.data = g_byte_array_new(), .limit = limit};
  curl_initialize();
  CURL *curl = curl_easy_init();
  CURLcode result = CURLE_FAILED_INIT;
  long status = 0;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)limit);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "quocker/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, kernel_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
  }
  if (result != CURLE_OK || status < 200 || status >= 300) {
    set_kernel_error(error, "HTTPS request failed (%s, HTTP %ld)",
                     curl_easy_strerror(result), status);
    g_byte_array_unref(buffer.data);
    return FALSE;
  }
  g_byte_array_append(buffer.data, (const guint8 *)"", 1);
  *size_out = buffer.data->len - 1;
  *contents_out = (gchar *)g_byte_array_free(buffer.data, FALSE);
  return TRUE;
}

static gboolean validate_catalog_document(const gchar *contents,
                                          gsize contents_size,
                                          GError **error) {
  if (memchr(contents, '\0', contents_size)) {
    set_kernel_error(error, "kernel catalog contains NUL bytes");
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, contents, contents_size,
                                                error);
  if (!parsed || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    if (parsed) {
      set_kernel_error(error, "kernel catalog root must be an object");
    }
    g_object_unref(parser);
    return FALSE;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonNode *version = json_object_get_member(root, "version");
  JsonNode *entries_node = json_object_get_member(root, "kernels");
  if (!version || !JSON_NODE_HOLDS_VALUE(version) ||
      json_node_get_value_type(version) != G_TYPE_INT64 ||
      json_node_get_int(version) != 1 || !entries_node ||
      !JSON_NODE_HOLDS_ARRAY(entries_node) ||
      json_array_get_length(json_node_get_array(entries_node)) == 0) {
    set_kernel_error(error,
                     "kernel catalog must use version 1 and contain kernels");
    g_object_unref(parser);
    return FALSE;
  }
  JsonArray *entries = json_node_get_array(entries_node);
  GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean ok = TRUE;
  for (guint i = 0; i < json_array_get_length(entries); i++) {
    JsonNode *entry = json_array_get_element(entries, i);
    if (!JSON_NODE_HOLDS_OBJECT(entry)) {
      set_kernel_error(error, "kernel catalog entries must be objects");
      ok = FALSE;
      break;
    }
    KernelCandidate *candidate =
        candidate_parse(json_node_get_object(entry), error);
    if (!candidate) {
      ok = FALSE;
      break;
    }
    if (g_hash_table_contains(ids, candidate->kernel->id)) {
      set_kernel_error(error, "kernel catalog contains duplicate ID '%s'",
                       candidate->kernel->id);
      ok = FALSE;
    } else {
      g_hash_table_add(ids, g_strdup(candidate->kernel->id));
    }
    kernel_candidate_free(candidate);
    if (!ok) {
      break;
    }
  }
  g_hash_table_destroy(ids);
  g_object_unref(parser);
  return ok;
}

static gboolean write_atomic_catalog_file(const char *directory,
                                          const char *contents, gsize size,
                                          char **path_out, GError **error) {
  char *template = g_build_filename(directory, ".quocker-catalog-XXXXXX", NULL);
  int fd = g_mkstemp(template);
  if (fd < 0) {
    set_kernel_error(error, "cannot create catalog staging file: %s",
                     g_strerror(errno));
    g_free(template);
    return FALSE;
  }
  size_t offset = 0;
  while (offset < size) {
    ssize_t count = write(fd, contents + offset, size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      set_kernel_error(error, "cannot write catalog staging file: %s",
                       g_strerror(errno));
      close(fd);
      g_unlink(template);
      g_free(template);
      return FALSE;
    }
    offset += count;
  }
  gboolean ok = fchmod(fd, 0600) == 0 && fsync(fd) == 0;
  if (close(fd) < 0) {
    ok = FALSE;
  }
  if (!ok) {
    set_kernel_error(error, "cannot sync catalog staging file: %s",
                     g_strerror(errno));
    g_unlink(template);
    g_free(template);
    return FALSE;
  }
  *path_out = template;
  return TRUE;
}

gboolean quocker_kernel_catalog_install_signed(
    const gchar *catalog, gsize catalog_size, const gchar *signature,
    gsize signature_size, const char *catalog_path,
    const char *public_key_path, GError **error) {
  if (!catalog || !catalog_size || catalog_size > KERNEL_CATALOG_MAX_BYTES ||
      !catalog_path || !*catalog_path || !public_key_path ||
      !*public_key_path ||
      !verify_catalog_signature_text(public_key_path, catalog, catalog_size,
                                     signature, signature_size, error) ||
      !validate_catalog_document(catalog, catalog_size, error)) {
    if (error && !*error) {
      set_kernel_error(error, "signed kernel catalog input is invalid");
    }
    return FALSE;
  }
  char *absolute = g_canonicalize_filename(catalog_path, NULL);
  char *directory = g_path_get_dirname(absolute);
  struct stat st;
  gboolean ok = g_mkdir_with_parents(directory, 0700) == 0 &&
                lstat(directory, &st) == 0 && S_ISDIR(st.st_mode) &&
                (st.st_uid == 0 || st.st_uid == geteuid()) &&
                !(st.st_mode & (S_IWGRP | S_IWOTH));
  if (!ok) {
    set_kernel_error(error, "catalog directory %s is unsafe or unavailable",
                     directory);
  }
  char *catalog_temp = NULL;
  char *signature_temp = NULL;
  if (ok) {
    ok = write_atomic_catalog_file(directory, signature, signature_size,
                                   &signature_temp, error) &&
         write_atomic_catalog_file(directory, catalog, catalog_size,
                                   &catalog_temp, error);
  }
  if (ok) {
    char *signature_path = g_strconcat(absolute, ".sig", NULL);
    if (g_rename(signature_temp, signature_path) < 0 ||
        g_rename(catalog_temp, absolute) < 0) {
      set_kernel_error(error, "cannot publish verified kernel catalog: %s",
                       g_strerror(errno));
      ok = FALSE;
    } else {
      int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (directory_fd < 0 || fsync(directory_fd) < 0) {
        set_kernel_error(error, "cannot sync kernel catalog directory: %s",
                         g_strerror(errno));
        ok = FALSE;
      }
      if (directory_fd >= 0) {
        close(directory_fd);
      }
    }
    g_free(signature_path);
  }
  if (signature_temp) {
    g_unlink(signature_temp);
  }
  if (catalog_temp) {
    g_unlink(catalog_temp);
  }
  g_free(signature_temp);
  g_free(catalog_temp);
  g_free(directory);
  g_free(absolute);
  return ok;
}

gboolean quocker_kernel_catalog_update(const char *url,
                                      const char *catalog_path,
                                      const char *public_key_path,
                                      GError **error) {
  if (!valid_https_url(url, TRUE) || !catalog_path || !*catalog_path ||
      !public_key_path || !*public_key_path) {
    set_kernel_error(error,
                     "catalog update requires HTTPS URL, output path, and trusted key");
    return FALSE;
  }
  gchar *catalog = NULL;
  gsize catalog_size = 0;
  if (!download_https_buffer(url, KERNEL_CATALOG_MAX_BYTES, &catalog,
                             &catalog_size, error)) {
    return FALSE;
  }
  char *signature_url = g_strconcat(url, ".sig", NULL);
  gchar *signature = NULL;
  gsize signature_size = 0;
  gboolean ok = download_https_buffer(signature_url,
                                      KERNEL_SIGNATURE_MAX_BYTES, &signature,
                                      &signature_size, error);
  g_free(signature_url);
  if (ok) {
    ok = quocker_kernel_catalog_install_signed(
        catalog, catalog_size, signature, signature_size, catalog_path,
        public_key_path, error);
  }
  if (ok) {
    g_print("Installed verified kernel catalog: %s\n", catalog_path);
  }
  g_free(signature);
  g_free(catalog);
  return ok;
}

static guint candidate_distro_score(KernelCandidate *candidate,
                                    const QuockerDistroInfo *distro) {
  if (!candidate->distro_ids->len) {
    return 1;
  }
  if (!distro) {
    return 0;
  }
  for (guint i = 0; i < candidate->distro_ids->len; i++) {
    const char *id = g_ptr_array_index(candidate->distro_ids, i);
    if (g_strcmp0(id, distro->id) == 0) {
      return 3;
    }
  }
  if (distro->id_like) {
    gchar **like = g_strsplit_set(distro->id_like, " \t", -1);
    for (guint i = 0; like[i]; i++) {
      for (guint j = 0; j < candidate->distro_ids->len; j++) {
        if (g_strcmp0(like[i], g_ptr_array_index(candidate->distro_ids, j)) ==
            0) {
          g_strfreev(like);
          return 2;
        }
      }
    }
    g_strfreev(like);
  }
  return 0;
}

static gboolean verify_selected_asset_or_defer(const char *path,
                                               const char *url,
                                               const char *digest,
                                               gboolean *verified,
                                               GError **error) {
  struct stat st;
  if (lstat(path, &st) < 0) {
    if (errno == ENOENT && url) {
      *verified = FALSE;
      return TRUE;
    }
    set_kernel_error(error, "cannot inspect kernel asset %s: %s", path,
                     g_strerror(errno));
    return FALSE;
  }
  if (!file_digest_matches(path, digest, error)) {
    return FALSE;
  }
  *verified = TRUE;
  return TRUE;
}

static gboolean candidate_platform_matches(KernelCandidate *candidate,
                                           const char *os,
                                           const char *architecture,
                                           const char *variant) {
  return g_strcmp0(candidate->kernel->os, os) == 0 &&
         g_strcmp0(candidate->kernel->architecture, architecture) == 0 &&
         g_strcmp0(candidate->kernel->variant, variant) == 0;
}

gboolean quocker_kernel_select_for_image(
    const char *catalog_path, const char *public_key_path, const char *platform,
    const QuockerDistroInfo *distro, const char *requested_id,
    const char *minimum_version, const GPtrArray *required_features,
    const GPtrArray *required_module_releases,
    QuockerKernel **kernel_out, GError **error) {
  if (kernel_out) {
    *kernel_out = NULL;
  }
  if (!catalog_path || !kernel_out) {
    set_kernel_error(error, "kernel catalog path and result are required");
    return FALSE;
  }
  if ((minimum_version && !valid_kernel_version(minimum_version)) ||
      (required_features && required_features->len > 256) ||
      (required_module_releases && required_module_releases->len > 256)) {
    set_kernel_error(error, "image kernel requirements are invalid");
    return FALSE;
  }
  for (guint i = 0; required_module_releases &&
                     i < required_module_releases->len;
       i++) {
    if (!valid_module_release(g_ptr_array_index(
            (GPtrArray *)required_module_releases, i))) {
      set_kernel_error(error, "image kernel module-release requirement is invalid");
      return FALSE;
    }
  }
  char *os = NULL;
  char *architecture = NULL;
  char *variant = NULL;
  if (!parse_platform(platform, &os, &architecture, &variant)) {
    set_kernel_error(error, "unsupported guest platform '%s'",
                     platform ? platform : "(missing)");
    return FALSE;
  }
  int catalog_fd =
      open(catalog_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  struct stat catalog_stat;
  if (catalog_fd < 0 || fstat(catalog_fd, &catalog_stat) < 0 ||
      !S_ISREG(catalog_stat.st_mode) ||
      (catalog_stat.st_uid != 0 && catalog_stat.st_uid != geteuid()) ||
      (catalog_stat.st_mode & (S_IWGRP | S_IWOTH)) ||
      catalog_stat.st_size < 0 ||
      catalog_stat.st_size > KERNEL_CATALOG_MAX_BYTES) {
    set_kernel_error(error, "kernel catalog is missing, unsafe, or too large");
    if (catalog_fd >= 0) {
      close(catalog_fd);
    }
    g_free(os);
    g_free(architecture);
    g_free(variant);
    return FALSE;
  }
  gsize contents_size = (gsize)catalog_stat.st_size;
  gchar *contents = g_malloc(contents_size + 1);
  gsize read_size = 0;
  while (read_size < contents_size) {
    ssize_t count =
        read(catalog_fd, contents + read_size, contents_size - read_size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    read_size += count;
  }
  close(catalog_fd);
  if (read_size != contents_size || memchr(contents, '\0', contents_size)) {
    set_kernel_error(error,
                     "kernel catalog is truncated or contains NUL bytes");
    g_free(contents);
    g_free(os);
    g_free(architecture);
    g_free(variant);
    return FALSE;
  }
  contents[contents_size] = '\0';
  if (!verify_catalog_signature(catalog_path, public_key_path, contents,
                                contents_size, error)) {
    g_free(contents);
    g_free(os);
    g_free(architecture);
    g_free(variant);
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  gboolean parsed =
      json_parser_load_from_data(parser, contents, contents_size, error);
  g_free(contents);
  if (!parsed || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    if (parsed) {
      set_kernel_error(error, "kernel catalog root must be an object");
    }
    g_object_unref(parser);
    g_free(os);
    g_free(architecture);
    g_free(variant);
    return FALSE;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonNode *version_node = json_object_get_member(root, "version");
  if (!version_node || !JSON_NODE_HOLDS_VALUE(version_node) ||
      json_node_get_value_type(version_node) != G_TYPE_INT64 ||
      json_node_get_int(version_node) != 1 ||
      !json_object_has_member(root, "kernels") ||
      !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "kernels"))) {
    set_kernel_error(error,
                     "kernel catalog must use version 1 and contain kernels");
    g_object_unref(parser);
    g_free(os);
    g_free(architecture);
    g_free(variant);
    return FALSE;
  }
  JsonArray *entries = json_object_get_array_member(root, "kernels");
  KernelCandidate *best = NULL;
  gboolean ok = TRUE;
  for (guint i = 0; i < json_array_get_length(entries); i++) {
    JsonNode *entry_node = json_array_get_element(entries, i);
    if (!JSON_NODE_HOLDS_OBJECT(entry_node)) {
      set_kernel_error(error, "kernel catalog entries must be objects");
      ok = FALSE;
      break;
    }
    KernelCandidate *candidate =
        candidate_parse(json_node_get_object(entry_node), error);
    if (!candidate) {
      ok = FALSE;
      break;
    }
    gboolean platform_match =
        candidate_platform_matches(candidate, os, architecture, variant);
    gboolean explicit_match =
        requested_id && g_strcmp0(candidate->kernel->id, requested_id) == 0;
    gboolean requirements_match =
        (!minimum_version ||
         (candidate->kernel->version &&
          compare_kernel_versions(candidate->kernel->version,
                                  minimum_version) >= 0));
    candidate->module_score = 0;
    if (distro && distro->kernel_module_releases) {
      for (guint j = 0; j < distro->kernel_module_releases->len; j++) {
        candidate->module_score += string_array_has(
            candidate->kernel->module_releases,
            g_ptr_array_index(distro->kernel_module_releases, j));
      }
    }
    for (guint j = 0;
         requirements_match && required_features && j < required_features->len;
         j++) {
      const char *feature =
          g_ptr_array_index((GPtrArray *)required_features, j);
      requirements_match =
          valid_identifier(feature) &&
          string_array_has(candidate->kernel->features, feature);
    }
    if (requirements_match && required_module_releases &&
        required_module_releases->len) {
      gboolean module_requirement_match = FALSE;
      for (guint j = 0; j < required_module_releases->len; j++) {
        module_requirement_match |= string_array_has(
            candidate->kernel->module_releases,
            g_ptr_array_index((GPtrArray *)required_module_releases, j));
      }
      requirements_match = module_requirement_match;
    }
    candidate->distro_score = candidate_distro_score(candidate, distro);
    gboolean distro_match = candidate->distro_score > 0;
    if (!platform_match || !requirements_match ||
        (requested_id ? !explicit_match : !distro_match)) {
      kernel_candidate_free(candidate);
      continue;
    }
    if (!best || (requested_id && explicit_match) ||
        candidate->distro_score > best->distro_score ||
        (candidate->distro_score == best->distro_score &&
         candidate->module_score > best->module_score) ||
        (candidate->distro_score == best->distro_score &&
         candidate->module_score == best->module_score &&
         candidate->priority > best->priority) ||
        (candidate->distro_score == best->distro_score &&
         candidate->module_score == best->module_score &&
         candidate->priority == best->priority &&
         g_strcmp0(candidate->kernel->id, best->kernel->id) < 0)) {
      kernel_candidate_free(best);
      best = candidate;
      if (requested_id) {
        break;
      }
    } else {
      kernel_candidate_free(candidate);
    }
  }
  if (ok && !best) {
    if (requested_id) {
      set_kernel_error(error,
                       "no catalog kernel '%s' matches guest platform %s",
                       requested_id, platform);
    } else {
      set_kernel_error(
          error, "no catalog kernel matches guest platform %s and distro hints",
          platform);
    }
    ok = FALSE;
  }
  if (ok) {
    gboolean kernel_verified = FALSE;
    gboolean initrd_verified = FALSE;
    ok = verify_selected_asset_or_defer(
             best->kernel->kernel_path, best->kernel->kernel_url,
             best->kernel->kernel_digest, &kernel_verified, error) &&
         verify_selected_asset_or_defer(
             best->kernel->initrd_path, best->kernel->initrd_url,
             best->kernel->initrd_digest, &initrd_verified, error);
    best->kernel->assets_verified = kernel_verified && initrd_verified;
  }
  if (ok) {
    best->kernel->module_release_evidence =
        distro && distro->kernel_module_releases &&
        distro->kernel_module_releases->len;
    best->kernel->module_release_match = best->module_score > 0;
    best->kernel->observed_module_releases =
        g_ptr_array_new_with_free_func(g_free);
    if (best->kernel->module_release_evidence) {
      for (guint i = 0; i < distro->kernel_module_releases->len; i++) {
        g_ptr_array_add(best->kernel->observed_module_releases,
                        g_strdup(g_ptr_array_index(
                            distro->kernel_module_releases, i)));
      }
    }
    const char *selection_reason =
        requested_id              ? "explicit kernel selection"
        : best->distro_score == 3 ? "exact guest distro match"
        : best->distro_score == 2 ? "guest distro-family match"
                                  : "generic platform fallback";
    best->kernel->reason = g_strdup_printf(
        "%s%s", selection_reason,
        best->kernel->module_release_evidence
            ? best->kernel->module_release_match
                  ? "; module release evidence matched"
                  : "; module release evidence unmatched (advisory)"
            : "");
    *kernel_out = g_steal_pointer(&best->kernel);
  }
  kernel_candidate_free(best);
  g_object_unref(parser);
  g_free(os);
  g_free(architecture);
  g_free(variant);
  return ok;
}

gboolean quocker_kernel_select(const char *catalog_path,
                               const char *public_key_path,
                               const char *platform,
                               const QuockerDistroInfo *distro,
                               const char *requested_id,
                               QuockerKernel **kernel_out, GError **error) {
  return quocker_kernel_select_for_image(catalog_path, public_key_path,
                                         platform, distro, requested_id, NULL,
                                         NULL, NULL, kernel_out, error);
}
