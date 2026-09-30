/*
 * Quocker OCI registry configuration.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-registry-config.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define REGISTRY_CONFIG_MAX_BYTES (1024 * 1024)

static GQuark registry_config_error_quark(void) {
  return g_quark_from_static_string("quocker-registry-config-error");
}

static char *canonical_registry(const char *registry) {
  if (!registry || !*registry || strlen(registry) > 255) {
    return NULL;
  }
  char *canonical = g_ascii_strdown(registry, -1);
  if (g_str_equal(canonical, "docker.io") ||
      g_str_equal(canonical, "index.docker.io")) {
    g_free(canonical);
    canonical = g_strdup("registry-1.docker.io");
  }
  for (const char *p = canonical; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '-' || *p == ':')) {
      g_free(canonical);
      return NULL;
    }
  }
  return canonical;
}

gboolean quocker_registry_mirror_url_valid(const char *url) {
  if (!url || strlen(url) > 4096) {
    return FALSE;
  }
  GError *error = NULL;
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
  gboolean valid = uri &&
                   g_strcmp0(g_uri_get_scheme(uri), "https") == 0 &&
                   g_uri_get_host(uri) && *g_uri_get_host(uri) &&
                   !g_uri_get_userinfo(uri) && !g_uri_get_query(uri) &&
                   !g_uri_get_fragment(uri);
  g_clear_error(&error);
  if (uri) {
    g_uri_unref(uri);
  }
  return valid;
}

static char *registry_config_path(void) {
  const char *configured = g_getenv("QUOCKER_REGISTRY_CONFIG");
  if (configured && *configured) {
    return g_strdup(configured);
  }
  return g_build_filename(g_get_user_config_dir(), "quocker", "registries.json",
                          NULL);
}

char *quocker_registry_mirror_resolve(const char *registry,
                                      const char *global_override,
                                      GError **error) {
  if (global_override && *global_override) {
    if (!quocker_registry_mirror_url_valid(global_override)) {
      g_set_error_literal(error, registry_config_error_quark(), 1,
                          "registry mirror URL must be HTTPS without userinfo, "
                          "query, or fragment");
      return NULL;
    }
    return g_strdup(global_override);
  }

  char *key = canonical_registry(registry);
  if (!key) {
    g_set_error_literal(error, registry_config_error_quark(), 2,
                        "invalid registry hostname");
    return NULL;
  }
  char *path = registry_config_path();
  gchar *contents = NULL;
  gsize length = 0;
  GError *read_error = NULL;
  if (!g_file_get_contents(path, &contents, &length, &read_error)) {
    if (!g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_set_error(error, registry_config_error_quark(), 7,
                  "cannot read registry configuration: %s",
                  read_error->message);
      g_clear_error(&read_error);
      g_free(path);
      g_free(key);
      return NULL;
    }
    g_clear_error(&read_error);
    g_free(path);
    g_free(key);
    return NULL;
  }
  g_free(path);
  if (length > REGISTRY_CONFIG_MAX_BYTES) {
    g_set_error_literal(error, registry_config_error_quark(), 3,
                        "registry configuration exceeds the 1 MiB limit");
    g_free(contents);
    g_free(key);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;
  gboolean parsed = json_parser_load_from_data(parser, contents, length,
                                                &parse_error);
  g_free(contents);
  if (!parsed) {
    g_set_error(error, registry_config_error_quark(), 4,
                "cannot parse registry configuration: %s",
                parse_error->message);
    g_clear_error(&parse_error);
    g_object_unref(parser);
    g_free(key);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *object = JSON_NODE_HOLDS_OBJECT(root)
                           ? json_node_get_object(root)
                           : NULL;
  JsonNode *mirrors_node = object && json_object_has_member(object, "mirrors")
                               ? json_object_get_member(object, "mirrors")
                               : NULL;
  JsonObject *mirrors = mirrors_node && JSON_NODE_HOLDS_OBJECT(mirrors_node)
                            ? json_node_get_object(mirrors_node)
                            : NULL;
  if (!mirrors) {
    g_set_error_literal(error, registry_config_error_quark(), 5,
                        "registry configuration must contain a mirrors object");
    g_object_unref(parser);
    g_free(key);
    return NULL;
  }

  char *result = NULL;
  GList *members = json_object_get_members(mirrors);
  for (GList *it = members; it; it = it->next) {
    const char *host = it->data;
    char *canonical = canonical_registry(host);
    JsonNode *value = json_object_get_member(mirrors, host);
    const char *url = value && JSON_NODE_HOLDS_VALUE(value) &&
                              json_node_get_value_type(value) == G_TYPE_STRING
                          ? json_node_get_string(value)
                          : NULL;
    gboolean valid = canonical && url && quocker_registry_mirror_url_valid(url);
    if (!valid) {
      g_set_error(error, registry_config_error_quark(), 6,
                  "invalid mirror mapping for registry '%s'", host);
      g_free(canonical);
      break;
    }
    if (g_str_equal(canonical, key)) {
      if (result && !g_str_equal(result, url)) {
        g_set_error_literal(error, registry_config_error_quark(), 8,
                            "registry mirror aliases have conflicting URLs");
        g_free(canonical);
        break;
      }
      g_free(result);
      result = g_strdup(url);
    }
    g_free(canonical);
  }
  g_list_free(members);
  g_object_unref(parser);
  g_free(key);
  if (error && *error) {
    g_clear_pointer(&result, g_free);
  }
  return result;
}
