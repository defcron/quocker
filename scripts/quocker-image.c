/*
 * OCI image runtime defaults and Compose override handling.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-image.h"

#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <string.h>

#define IMAGE_METADATA_MAX_BYTES (16 * 1024 * 1024)
#define IMAGE_ARG_MAX_COUNT 65536
#define IMAGE_STRING_MAX_BYTES (64 * 1024)

static GQuark image_config_error_quark(void) {
  return g_quark_from_static_string("quocker-image-config-error");
}

static void image_config_error(GError **error, const char *format, ...)
    G_GNUC_PRINTF(2, 3);

static void image_config_error(GError **error, const char *format, ...) {
  if (!error || *error) {
    return;
  }
  va_list args;
  va_start(args, format);
  char *message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error_literal(error, image_config_error_quark(), 1, message);
  g_free(message);
}

static GPtrArray *string_array_new(void) {
  return g_ptr_array_new_with_free_func(g_free);
}

static GHashTable *string_map_new(void) {
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static gboolean valid_env_name(const char *name) {
  if (!name || !*name || !(g_ascii_isalpha(*name) || *name == '_')) {
    return FALSE;
  }
  for (const char *p = name + 1; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean json_string_array(JsonObject *object, const char *key,
                                  GPtrArray *array, GError **error) {
  if (!json_object_has_member(object, key)) {
    return TRUE;
  }
  JsonNode *node = json_object_get_member(object, key);
  if (json_node_is_null(node)) {
    return TRUE;
  }
  if (!JSON_NODE_HOLDS_ARRAY(node)) {
    image_config_error(error, "OCI image Config.%s must be an array", key);
    return FALSE;
  }
  JsonArray *json_array = json_node_get_array(node);
  if (json_array_get_length(json_array) > IMAGE_ARG_MAX_COUNT) {
    image_config_error(error, "OCI image Config.%s has too many entries", key);
    return FALSE;
  }
  for (guint i = 0; i < json_array_get_length(json_array); i++) {
    JsonNode *item = json_array_get_element(json_array, i);
    if (!JSON_NODE_HOLDS_VALUE(item) ||
        json_node_get_value_type(item) != G_TYPE_STRING) {
      image_config_error(error, "OCI image Config.%s entry %u must be a string",
                         key, i);
      return FALSE;
    }
    const char *value = json_node_get_string(item);
    if (!value || strlen(value) > IMAGE_STRING_MAX_BYTES) {
      image_config_error(
          error, "OCI image Config.%s entry %u exceeds the size limit", key, i);
      return FALSE;
    }
    g_ptr_array_add(array, g_strdup(value));
  }
  return TRUE;
}

static gboolean json_optional_string(JsonObject *object, const char *key,
                                     char **value_out, GError **error) {
  if (!json_object_has_member(object, key)) {
    return TRUE;
  }
  JsonNode *node = json_object_get_member(object, key);
  if (json_node_is_null(node)) {
    return TRUE;
  }
  if (!JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    image_config_error(error, "OCI image Config.%s must be a string", key);
    return FALSE;
  }
  const char *value = json_node_get_string(node);
  if (!value || strlen(value) > IMAGE_STRING_MAX_BYTES) {
    image_config_error(error, "OCI image Config.%s exceeds the size limit",
                       key);
    return FALSE;
  }
  *value_out = g_strdup(value);
  return TRUE;
}

static gboolean parse_environment(GPtrArray *entries, GHashTable *environment,
                                  GError **error) {
  for (guint i = 0; i < entries->len; i++) {
    const char *entry = g_ptr_array_index(entries, i);
    const char *equals = strchr(entry, '=');
    if (!equals) {
      image_config_error(error, "OCI image Config.Env entry %u has no '='", i);
      return FALSE;
    }
    char *name = g_strndup(entry, equals - entry);
    if (!valid_env_name(name)) {
      image_config_error(
          error, "OCI image Config.Env entry %u has an invalid name", i);
      g_free(name);
      return FALSE;
    }
    g_hash_table_replace(environment, name, g_strdup(equals + 1));
  }
  return TRUE;
}

static gboolean parse_volumes(JsonObject *object, GHashTable *volumes,
                              GError **error) {
  if (!json_object_has_member(object, "Volumes")) {
    return TRUE;
  }
  JsonNode *node = json_object_get_member(object, "Volumes");
  if (json_node_is_null(node)) {
    return TRUE;
  }
  if (!JSON_NODE_HOLDS_OBJECT(node)) {
    image_config_error(error, "OCI image Config.Volumes must be an object");
    return FALSE;
  }
  JsonObject *json_volumes = json_node_get_object(node);
  GList *members = json_object_get_members(json_volumes);
  for (GList *it = members; it; it = it->next) {
    const char *path = it->data;
    JsonNode *volume = json_object_get_member(json_volumes, path);
    if (path[0] != '/' || !JSON_NODE_HOLDS_OBJECT(volume)) {
      g_list_free(members);
      image_config_error(error, "OCI image Config.Volumes entries must be "
                                "absolute paths with object values");
      return FALSE;
    }
    g_hash_table_add(volumes, g_strdup(path));
  }
  g_list_free(members);
  return TRUE;
}

static gboolean valid_kernel_version(const char *value) {
  if (!value || !*value || strlen(value) > 64) {
    return FALSE;
  }
  gboolean component_has_digit = FALSE;
  guint component_digits = 0;
  for (const char *p = value; *p; p++) {
    if (g_ascii_isdigit(*p)) {
      component_has_digit = TRUE;
      if (++component_digits > 10) {
        return FALSE;
      }
    } else if (*p == '.' && component_has_digit && p[1]) {
      component_has_digit = FALSE;
      component_digits = 0;
    } else {
      return FALSE;
    }
  }
  return component_has_digit;
}

static gboolean valid_kernel_token(const char *value) {
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

static gboolean valid_module_release_token(const char *value) {
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

static gboolean parse_kernel_labels(JsonObject *config,
                                    QuockerImageDefaults *defaults,
                                    GError **error) {
  if (!json_object_has_member(config, "Labels")) {
    return TRUE;
  }
  JsonNode *node = json_object_get_member(config, "Labels");
  if (json_node_is_null(node)) {
    return TRUE;
  }
  if (!JSON_NODE_HOLDS_OBJECT(node)) {
    image_config_error(error, "OCI image Config.Labels must be an object");
    return FALSE;
  }
  JsonObject *labels = json_node_get_object(node);
  const char *id_label = "io.quocker.kernel.id";
  const char *minimum_label = "io.quocker.kernel.minimum";
  const char *features_label = "io.quocker.kernel.features";
  const char *modules_label = "io.quocker.kernel.module_releases";
  const char *scalar_labels[] = {id_label, minimum_label};
  char **destinations[] = {&defaults->kernel_id, &defaults->kernel_minimum};
  for (guint i = 0; i < G_N_ELEMENTS(scalar_labels); i++) {
    if (!json_object_has_member(labels, scalar_labels[i])) {
      continue;
    }
    JsonNode *value_node = json_object_get_member(labels, scalar_labels[i]);
    if (!JSON_NODE_HOLDS_VALUE(value_node) ||
        json_node_get_value_type(value_node) != G_TYPE_STRING) {
      image_config_error(error, "OCI image label %s must be a string",
                         scalar_labels[i]);
      return FALSE;
    }
    const char *value = json_node_get_string(value_node);
    gboolean valid =
        i == 0 ? valid_kernel_token(value) : valid_kernel_version(value);
    if (!valid) {
      image_config_error(error, "OCI image label %s has an invalid value",
                         scalar_labels[i]);
      return FALSE;
    }
    *destinations[i] = g_strdup(value);
  }
  if (json_object_has_member(labels, features_label)) {
    JsonNode *features_node = json_object_get_member(labels, features_label);
    if (!JSON_NODE_HOLDS_VALUE(features_node) ||
        json_node_get_value_type(features_node) != G_TYPE_STRING) {
      image_config_error(error, "OCI image label %s must be a string",
                         features_label);
      return FALSE;
    }
    const char *features = json_node_get_string(features_node);
    if (!features || strlen(features) > IMAGE_STRING_MAX_BYTES) {
      image_config_error(error, "OCI image label %s exceeds the size limit",
                         features_label);
      return FALSE;
    }
    gchar **items = g_strsplit(features, ",", -1);
    for (guint i = 0; items[i]; i++) {
      char *item = g_strstrip(items[i]);
      if (!valid_kernel_token(item)) {
        g_strfreev(items);
        image_config_error(error, "OCI image label %s has an invalid feature",
                           features_label);
        return FALSE;
      }
      gboolean duplicate = FALSE;
      for (guint j = 0; j < defaults->kernel_features->len; j++) {
        duplicate |=
            g_str_equal(item, g_ptr_array_index(defaults->kernel_features, j));
      }
      if (!duplicate) {
        g_ptr_array_add(defaults->kernel_features, g_strdup(item));
      }
    }
    g_strfreev(items);
  }
  if (json_object_has_member(labels, modules_label)) {
    JsonNode *modules_node = json_object_get_member(labels, modules_label);
    if (!JSON_NODE_HOLDS_VALUE(modules_node) ||
        json_node_get_value_type(modules_node) != G_TYPE_STRING) {
      image_config_error(error, "OCI image label %s must be a string",
                         modules_label);
      return FALSE;
    }
    const char *modules = json_node_get_string(modules_node);
    if (!modules || strlen(modules) > IMAGE_STRING_MAX_BYTES) {
      image_config_error(error, "OCI image label %s exceeds the size limit",
                         modules_label);
      return FALSE;
    }
    gchar **items = g_strsplit(modules, ",", -1);
    for (guint i = 0; items[i]; i++) {
      char *item = g_strstrip(items[i]);
      if (!valid_module_release_token(item)) {
        g_strfreev(items);
        image_config_error(error, "OCI image label %s has an invalid release",
                           modules_label);
        return FALSE;
      }
      gboolean duplicate = FALSE;
      for (guint j = 0; j < defaults->kernel_module_releases->len; j++) {
        duplicate |= g_str_equal(
            item, g_ptr_array_index(defaults->kernel_module_releases, j));
      }
      if (!duplicate) {
        g_ptr_array_add(defaults->kernel_module_releases, g_strdup(item));
      }
    }
    g_strfreev(items);
  }
  return TRUE;
}

gboolean quocker_image_defaults_parse(const char *json, gsize length,
                                      QuockerImageDefaults **defaults_out,
                                      GError **error) {
  if (defaults_out) {
    *defaults_out = NULL;
  }
  if (!json || !defaults_out || length > IMAGE_METADATA_MAX_BYTES ||
      memchr(json, '\0', length)) {
    image_config_error(
        error, "OCI image config is missing, contains NUL, or is too large");
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, length, error)) {
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    image_config_error(error, "OCI image config root must be an object");
    g_object_unref(parser);
    return FALSE;
  }
  JsonObject *root = json_node_get_object(root_node);
  JsonObject *config = NULL;
  if (json_object_has_member(root, "config")) {
    JsonNode *config_node = json_object_get_member(root, "config");
    if (!json_node_is_null(config_node) &&
        !JSON_NODE_HOLDS_OBJECT(config_node)) {
      image_config_error(error,
                         "OCI image config 'config' member must be an object");
      g_object_unref(parser);
      return FALSE;
    }
    if (JSON_NODE_HOLDS_OBJECT(config_node)) {
      config = json_node_get_object(config_node);
    }
  }
  QuockerImageDefaults *defaults = g_new0(QuockerImageDefaults, 1);
  defaults->entrypoint = string_array_new();
  defaults->command = string_array_new();
  defaults->environment = string_map_new();
  defaults->volumes =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  defaults->kernel_features = string_array_new();
  defaults->kernel_module_releases = string_array_new();
  gboolean ok = TRUE;
  if (config) {
    GPtrArray *env_entries = string_array_new();
    ok = json_string_array(config, "Entrypoint", defaults->entrypoint, error) &&
         json_string_array(config, "Cmd", defaults->command, error) &&
         json_string_array(config, "Env", env_entries, error) &&
         parse_environment(env_entries, defaults->environment, error) &&
         json_optional_string(config, "WorkingDir",
                              &defaults->working_directory, error) &&
         json_optional_string(config, "User", &defaults->user, error) &&
         parse_volumes(config, defaults->volumes, error) &&
         parse_kernel_labels(config, defaults, error);
    g_ptr_array_free(env_entries, TRUE);
  }
  g_object_unref(parser);
  if (!ok) {
    quocker_image_defaults_free(defaults);
    return FALSE;
  }
  *defaults_out = defaults;
  return TRUE;
}

void quocker_image_defaults_free(QuockerImageDefaults *defaults) {
  if (!defaults) {
    return;
  }
  g_ptr_array_free(defaults->entrypoint, TRUE);
  g_ptr_array_free(defaults->command, TRUE);
  g_hash_table_destroy(defaults->environment);
  g_hash_table_destroy(defaults->volumes);
  g_free(defaults->working_directory);
  g_free(defaults->user);
  g_free(defaults->kernel_id);
  g_free(defaults->kernel_minimum);
  g_ptr_array_free(defaults->kernel_features, TRUE);
  g_ptr_array_free(defaults->kernel_module_releases, TRUE);
  g_free(defaults);
}

static gboolean copy_override_environment(GHashTable *destination,
                                          GHashTable *overrides,
                                          GError **error) {
  if (!overrides) {
    return TRUE;
  }
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, overrides);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    if (!valid_env_name(key) ||
        (value && strlen(value) > IMAGE_STRING_MAX_BYTES)) {
      image_config_error(
          error, "Compose environment contains an invalid name or value");
      return FALSE;
    }
    if (value) {
      g_hash_table_replace(destination, g_strdup(key), g_strdup(value));
    } else {
      g_hash_table_remove(destination, key);
    }
  }
  return TRUE;
}

static void append_args(GPtrArray *destination, const GPtrArray *source) {
  if (!source) {
    return;
  }
  for (guint i = 0; i < source->len; i++) {
    g_ptr_array_add(destination,
                    g_strdup(g_ptr_array_index((GPtrArray *)source, i)));
  }
}

gboolean quocker_runtime_config_merge(
    const QuockerImageDefaults *defaults, const GPtrArray *entrypoint_override,
    const GPtrArray *command_override, GHashTable *environment_override,
    const char *working_directory_override, const char *user_override,
    QuockerRuntimeConfig **runtime_out, GError **error) {
  if (runtime_out) {
    *runtime_out = NULL;
  }
  if (!defaults || !runtime_out) {
    image_config_error(error, "image defaults and runtime result are required");
    return FALSE;
  }
  QuockerRuntimeConfig *runtime = g_new0(QuockerRuntimeConfig, 1);
  runtime->argv = string_array_new();
  runtime->environment = string_map_new();
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, defaults->environment);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_hash_table_insert(runtime->environment, g_strdup(key), g_strdup(value));
  }
  if (!copy_override_environment(runtime->environment, environment_override,
                                 error)) {
    quocker_runtime_config_free(runtime);
    return FALSE;
  }

  append_args(runtime->argv,
              entrypoint_override ? entrypoint_override : defaults->entrypoint);
  if (command_override) {
    append_args(runtime->argv, command_override);
  } else if (!entrypoint_override) {
    append_args(runtime->argv, defaults->command);
  }
  if (!runtime->argv->len) {
    image_config_error(error,
                       "image and Compose config provide no guest command");
    quocker_runtime_config_free(runtime);
    return FALSE;
  }
  runtime->working_directory =
      g_strdup(working_directory_override ? working_directory_override
                                          : defaults->working_directory);
  runtime->user = g_strdup(user_override ? user_override : defaults->user);
  *runtime_out = runtime;
  return TRUE;
}

void quocker_runtime_config_free(QuockerRuntimeConfig *runtime) {
  if (!runtime) {
    return;
  }
  g_ptr_array_free(runtime->argv, TRUE);
  g_hash_table_destroy(runtime->environment);
  g_free(runtime->working_directory);
  g_free(runtime->user);
  g_free(runtime);
}
