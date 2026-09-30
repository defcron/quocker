/*
 * Docker CLI config authentication reader.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-registry-auth.h"

#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DOCKER_CONFIG_MAX_BYTES (1024 * 1024)
#define CREDENTIAL_HELPER_MAX_BYTES (64 * 1024)
#define CREDENTIAL_HELPER_TIMEOUT_MS 10000

static GQuark registry_auth_error_quark(void) {
  return g_quark_from_static_string("quocker-registry-auth-error");
}

static void auth_error(GError **error, const char *message) {
  if (error && !*error) {
    g_set_error_literal(error, registry_auth_error_quark(), 1, message);
  }
}

void quocker_registry_credentials_free(QuockerRegistryCredentials *credentials) {
  if (!credentials) {
    return;
  }
  if (credentials->secret) {
    explicit_bzero(credentials->secret, strlen(credentials->secret));
  }
  g_free(credentials->username);
  g_free(credentials->secret);
  g_free(credentials);
}

static char *docker_config_path(void) {
  const char *configured = g_getenv("DOCKER_CONFIG");
  const char *directory = configured && *configured
                              ? configured
                              : g_build_filename(g_get_home_dir(), ".docker",
                                                 NULL);
  char *path = g_build_filename(directory, "config.json", NULL);
  if (!(configured && *configured)) {
    g_free((char *)directory);
  }
  return path;
}

static gboolean read_config(const char *path, gchar **contents_out,
                            gsize *length_out, gboolean *exists_out,
                            GError **error) {
  *contents_out = NULL;
  *length_out = 0;
  *exists_out = FALSE;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) {
      return TRUE;
    }
    auth_error(error, "cannot open Docker config.json safely");
    return FALSE;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (guint64)st.st_size > DOCKER_CONFIG_MAX_BYTES || st.st_uid != geteuid()) {
    close(fd);
    auth_error(error, "Docker config.json has unsafe ownership or size");
    return FALSE;
  }
  gchar *contents = g_malloc((gsize)st.st_size + 1);
  gsize offset = 0;
  while (offset < (gsize)st.st_size) {
    ssize_t count = read(fd, contents + offset, (gsize)st.st_size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      explicit_bzero(contents, offset);
      g_free(contents);
      close(fd);
      auth_error(error, "could not read Docker config.json");
      return FALSE;
    }
    offset += (gsize)count;
  }
  close(fd);
  contents[offset] = '\0';
  *contents_out = contents;
  *length_out = offset;
  *exists_out = TRUE;
  return TRUE;
}

static gboolean json_string(JsonObject *object, const char *key,
                            const char **value_out) {
  *value_out = NULL;
  if (!object || !json_object_has_member(object, key)) {
    return TRUE;
  }
  JsonNode *node = json_object_get_member(object, key);
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    return FALSE;
  }
  *value_out = json_node_get_string(node);
  return TRUE;
}

static JsonNode *find_registry_node(JsonObject *object, const char *registry) {
  if (!object) {
    return NULL;
  }
  const char *hub_candidates[] = {"registry-1.docker.io",
                                  "https://registry-1.docker.io",
                                  "docker.io", "https://docker.io",
                                  "index.docker.io",
                                  "https://index.docker.io/v1/", NULL};
  if (g_str_equal(registry, "registry-1.docker.io") ||
      g_str_equal(registry, "docker.io") ||
      g_str_equal(registry, "index.docker.io")) {
    for (guint i = 0; hub_candidates[i]; i++) {
      JsonNode *node = json_object_get_member(object, hub_candidates[i]);
      if (node) {
        return node;
      }
    }
    return NULL;
  }
  JsonNode *node = json_object_get_member(object, registry);
  if (node) {
    return node;
  }
  char *https_registry = g_strdup_printf("https://%s", registry);
  node = json_object_get_member(object, https_registry);
  g_free(https_registry);
  return node;
}

static JsonObject *find_auth(JsonObject *auths, const char *registry) {
  JsonNode *node = find_registry_node(auths, registry);
  return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node)
                                              : NULL;
}

static char *helper_server_url(const char *registry) {
  if (g_str_equal(registry, "registry-1.docker.io") ||
      g_str_equal(registry, "docker.io") ||
      g_str_equal(registry, "index.docker.io")) {
    return g_strdup("https://index.docker.io/v1/");
  }
  return g_strdup_printf("https://%s", registry);
}

static gboolean valid_helper_suffix(const char *suffix) {
  if (!suffix || !*suffix || strlen(suffix) > 64) {
    return FALSE;
  }
  for (const char *p = suffix; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '-' || *p == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean credential_helper(const char *suffix, const char *server,
                                  QuockerRegistryCredentials **credentials_out,
                                  GError **error) {
  *credentials_out = NULL;
  if (!valid_helper_suffix(suffix)) {
    auth_error(error, "Docker credential helper name is invalid");
    return FALSE;
  }
  char *program_name = g_strdup_printf("docker-credential-%s", suffix);
  char *program = g_find_program_in_path(program_name);
  if (!program) {
    g_free(program_name);
    auth_error(error, "configured Docker credential helper was not found");
    return FALSE;
  }
  char *arguments[] = {program, (char *)"get", NULL};
  GPid child = 0;
  gint stdin_fd = -1;
  gint stdout_fd = -1;
  GError *spawn_error = NULL;
  gboolean spawned = g_spawn_async_with_pipes(
      NULL, arguments, NULL,
      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
      &child, &stdin_fd, &stdout_fd, NULL, &spawn_error);
  g_free(program_name);
  g_free(program);
  if (!spawned) {
    auth_error(error, "could not start configured Docker credential helper");
    g_clear_error(&spawn_error);
    return FALSE;
  }

  struct sigaction old_pipe_action = {0};
  struct sigaction ignore_pipe_action = {.sa_handler = SIG_IGN};
  sigemptyset(&ignore_pipe_action.sa_mask);
  sigaction(SIGPIPE, &ignore_pipe_action, &old_pipe_action);
  char *input = g_strdup_printf("%s\n", server);
  gsize input_left = strlen(input);
  const char *input_cursor = input;
  gboolean output_ok = TRUE;
  while (input_left) {
    ssize_t count = write(stdin_fd, input_cursor, input_left);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      output_ok = FALSE;
      break;
    }
    input_cursor += count;
    input_left -= (gsize)count;
  }
  explicit_bzero(input, strlen(input));
  g_free(input);
  close(stdin_fd);
  sigaction(SIGPIPE, &old_pipe_action, NULL);

  GByteArray *output = g_byte_array_sized_new(1024);
  gint64 deadline = g_get_monotonic_time() +
                    (gint64)CREDENTIAL_HELPER_TIMEOUT_MS * 1000;
  gboolean eof = FALSE;
  int child_status = 0;
  gboolean child_done = FALSE;
  while (output_ok && (!eof || !child_done)) {
    gint64 remaining_us = deadline - g_get_monotonic_time();
    if (remaining_us <= 0) {
      output_ok = FALSE;
      break;
    }
    struct pollfd descriptor = {.fd = eof ? -1 : stdout_fd,
                                .events = POLLIN | POLLHUP};
    int timeout = (int)MIN((remaining_us + 999) / 1000, 100);
    int ready = poll(&descriptor, 1, timeout);
    if (ready < 0 && errno != EINTR) {
      output_ok = FALSE;
      break;
    }
    if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
      guint8 buffer[4096];
      ssize_t count = read(stdout_fd, buffer, sizeof(buffer));
      if (count < 0 && errno != EINTR) {
        explicit_bzero(buffer, sizeof(buffer));
        output_ok = FALSE;
        break;
      }
      if (count == 0) {
        eof = TRUE;
      } else if (count > 0) {
        if ((gsize)count > CREDENTIAL_HELPER_MAX_BYTES - output->len) {
          explicit_bzero(buffer, sizeof(buffer));
          output_ok = FALSE;
          break;
        }
        g_byte_array_append(output, buffer, (guint)count);
      }
      explicit_bzero(buffer, sizeof(buffer));
    }
    if (!child_done) {
      pid_t waited = waitpid(child, &child_status, WNOHANG);
      if (waited == child) {
        child_done = TRUE;
      } else if (waited < 0 && errno != EINTR) {
        output_ok = FALSE;
        break;
      }
    }
  }
  close(stdout_fd);
  if (!child_done) {
    kill(child, SIGKILL);
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  g_spawn_close_pid(child);
  if (!output_ok) {
    explicit_bzero(output->data, output->len);
    g_byte_array_unref(output);
    auth_error(error, "Docker credential helper failed or exceeded its limits");
    return FALSE;
  }
  if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    explicit_bzero(output->data, output->len);
    g_byte_array_unref(output);
    return TRUE;
  }

  JsonParser *parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(
      parser, (const char *)output->data, output->len, NULL);
  explicit_bzero(output->data, output->len);
  g_byte_array_unref(output);
  JsonNode *root_node = parsed ? json_parser_get_root(parser) : NULL;
  JsonObject *root = root_node && JSON_NODE_HOLDS_OBJECT(root_node)
                         ? json_node_get_object(root_node)
                         : NULL;
  const char *username = NULL;
  const char *secret = NULL;
  gboolean valid = json_string(root, "Username", &username) &&
                   json_string(root, "Secret", &secret);
  if (!valid || !username || !*username || !secret) {
    g_object_unref(parser);
    auth_error(error, "Docker credential helper returned invalid credentials");
    return FALSE;
  }
  QuockerRegistryCredentials *credentials =
      g_new0(QuockerRegistryCredentials, 1);
  credentials->username = g_strdup(username);
  credentials->secret = g_strdup(secret);
  g_object_unref(parser);
  *credentials_out = credentials;
  return TRUE;
}

gboolean quocker_registry_credentials_load(
    const char *registry, QuockerRegistryCredentials **credentials_out,
    GError **error) {
  if (credentials_out) {
    *credentials_out = NULL;
  }
  if (error) {
    *error = NULL;
  }
  if (!registry || !*registry || !credentials_out) {
    auth_error(error, "registry name and credential output are required");
    return FALSE;
  }
  char *path = docker_config_path();
  gchar *contents = NULL;
  gsize length = 0;
  gboolean exists = FALSE;
  if (!read_config(path, &contents, &length, &exists, error)) {
    g_free(path);
    return FALSE;
  }
  g_free(path);
  if (!exists) {
    return TRUE;
  }
  JsonParser *parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, contents, length, error);
  explicit_bzero(contents, length);
  g_free(contents);
  if (!parsed) {
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *root_node = json_parser_get_root(parser);
  JsonObject *root = JSON_NODE_HOLDS_OBJECT(root_node)
                         ? json_node_get_object(root_node)
                         : NULL;
  JsonNode *auths_node = root ? json_object_get_member(root, "auths") : NULL;
  if (!root || (auths_node && !JSON_NODE_HOLDS_OBJECT(auths_node))) {
    g_object_unref(parser);
    auth_error(error, "Docker config.json auths must be an object");
    return FALSE;
  }
  JsonNode *helpers_node = json_object_get_member(root, "credHelpers");
  if (helpers_node && !JSON_NODE_HOLDS_OBJECT(helpers_node)) {
    g_object_unref(parser);
    auth_error(error, "Docker config.json credHelpers must be an object");
    return FALSE;
  }
  JsonNode *helper_node = find_registry_node(
      helpers_node ? json_node_get_object(helpers_node) : NULL, registry);
  const char *helper = NULL;
  if (helper_node) {
    if (!JSON_NODE_HOLDS_VALUE(helper_node) ||
        json_node_get_value_type(helper_node) != G_TYPE_STRING ||
        !json_node_get_string(helper_node)[0]) {
      g_object_unref(parser);
      auth_error(error, "Docker registry credential helper name is invalid");
      return FALSE;
    }
    helper = json_node_get_string(helper_node);
  } else {
    const char *store = NULL;
    if (!json_string(root, "credsStore", &store)) {
      g_object_unref(parser);
      auth_error(error, "Docker config.json credsStore must be a string");
      return FALSE;
    }
    helper = store && *store ? store : NULL;
  }
  if (helper) {
    char *helper_copy = g_strdup(helper);
    char *server = helper_server_url(registry);
    g_object_unref(parser);
    gboolean ok = credential_helper(helper_copy, server, credentials_out, error);
    g_free(helper_copy);
    g_free(server);
    return ok;
  }
  JsonObject *entry = find_auth(auths_node ? json_node_get_object(auths_node)
                                           : NULL,
                                registry);
  if (!entry) {
    g_object_unref(parser);
    return TRUE;
  }
  const char *username = NULL;
  const char *password = NULL;
  const char *auth = NULL;
  if (!json_string(entry, "username", &username) ||
      !json_string(entry, "password", &password) ||
      !json_string(entry, "auth", &auth)) {
    g_object_unref(parser);
    auth_error(error, "Docker registry auth entry contains a non-string field");
    return FALSE;
  }
  QuockerRegistryCredentials *credentials = g_new0(
      QuockerRegistryCredentials, 1);
  if (username && password) {
    credentials->username = g_strdup(username);
    credentials->secret = g_strdup(password);
  } else if (auth) {
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(auth, &decoded_length);
    char *separator = decoded ? memchr(decoded, ':', decoded_length) : NULL;
    if (!separator || memchr(decoded, '\0', decoded_length)) {
      g_free(decoded);
      quocker_registry_credentials_free(credentials);
      g_object_unref(parser);
      auth_error(error, "Docker registry auth entry has invalid base64 auth");
      return FALSE;
    }
    *separator = '\0';
    credentials->username = g_strdup((char *)decoded);
    credentials->secret = g_strndup(separator + 1,
                                    decoded_length - (gsize)(separator + 1 -
                                                            (char *)decoded));
    explicit_bzero(decoded, decoded_length);
    g_free(decoded);
  }
  g_object_unref(parser);
  if (!credentials->username || !*credentials->username ||
      !credentials->secret) {
    quocker_registry_credentials_free(credentials);
    auth_error(error, "Docker registry auth entry has no usable credentials");
    return FALSE;
  }
  *credentials_out = credentials;
  return TRUE;
}
