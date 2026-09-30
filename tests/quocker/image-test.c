/*
 * Tests for OCI image defaults and Compose runtime overrides.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-image.h"

static const char image_config_json[] =
    "{\"architecture\":\"amd64\",\"os\":\"linux\","
    "\"config\":{\"Entrypoint\":[\"/usr/bin/server\"],"
    "\"Cmd\":[\"--listen\",\"0.0.0.0\"],"
    "\"Env\":[\"PATH=/usr/bin\",\"MODE=release\",\"EMPTY=\"],"
    "\"WorkingDir\":\"/srv/app\",\"User\":\"1000:1000\","
    "\"Volumes\":{\"/data\":{},\"/cache\":{}},"
    "\"Labels\":{\"io.quocker.kernel.minimum\":\"6.6.1\","
    "\"io.quocker.kernel.features\":\"virtio_blk, virtio_net,virtio_blk\","
    "\"io.quocker.kernel.id\":\"linux-amd64-generic\","
    "\"io.quocker.kernel.module_releases\":\"6.8.1-quocker,"
    "6.8.1-quocker+debug\"}}}";

static GPtrArray *args(const char *const *values, guint count) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < count; i++) {
    g_ptr_array_add(result, g_strdup(values[i]));
  }
  return result;
}

static void test_image_defaults_parse_and_inherit(void) {
  QuockerImageDefaults *defaults = NULL;
  GError *error = NULL;
  g_assert_true(quocker_image_defaults_parse(
      image_config_json, strlen(image_config_json), &defaults, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(defaults->entrypoint->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(defaults->entrypoint, 0), ==,
                  "/usr/bin/server");
  g_assert_cmpuint(defaults->command->len, ==, 2);
  g_assert_cmpstr(g_hash_table_lookup(defaults->environment, "MODE"), ==,
                  "release");
  g_assert_cmpstr(g_hash_table_lookup(defaults->environment, "EMPTY"), ==, "");
  g_assert_cmpstr(defaults->working_directory, ==, "/srv/app");
  g_assert_cmpstr(defaults->user, ==, "1000:1000");
  g_assert_true(g_hash_table_contains(defaults->volumes, "/data"));
  g_assert_true(g_hash_table_contains(defaults->volumes, "/cache"));
  g_assert_cmpstr(defaults->kernel_id, ==, "linux-amd64-generic");
  g_assert_cmpstr(defaults->kernel_minimum, ==, "6.6.1");
  g_assert_cmpuint(defaults->kernel_features->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(defaults->kernel_features, 0), ==,
                  "virtio_blk");
  g_assert_cmpstr(g_ptr_array_index(defaults->kernel_features, 1), ==,
                  "virtio_net");
  g_assert_cmpuint(defaults->kernel_module_releases->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(defaults->kernel_module_releases, 0), ==,
                  "6.8.1-quocker");

  QuockerRuntimeConfig *runtime = NULL;
  g_assert_true(quocker_runtime_config_merge(defaults, NULL, NULL, NULL, NULL,
                                             NULL, &runtime, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(runtime->argv->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 0), ==, "/usr/bin/server");
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 1), ==, "--listen");
  g_assert_cmpstr(runtime->working_directory, ==, "/srv/app");
  g_assert_cmpstr(runtime->user, ==, "1000:1000");

  quocker_runtime_config_free(runtime);
  quocker_image_defaults_free(defaults);
}

static void test_compose_entrypoint_clears_image_command(void) {
  QuockerImageDefaults *defaults = NULL;
  GError *error = NULL;
  g_assert_true(quocker_image_defaults_parse(
      image_config_json, strlen(image_config_json), &defaults, &error));
  const char *entrypoint_values[] = {"/compose/entrypoint"};
  GPtrArray *entrypoint =
      args(entrypoint_values, G_N_ELEMENTS(entrypoint_values));
  QuockerRuntimeConfig *runtime = NULL;
  g_assert_true(quocker_runtime_config_merge(defaults, entrypoint, NULL, NULL,
                                             NULL, NULL, &runtime, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(runtime->argv->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 0), ==,
                  "/compose/entrypoint");
  quocker_runtime_config_free(runtime);
  g_ptr_array_free(entrypoint, TRUE);
  quocker_image_defaults_free(defaults);
}

static void test_command_and_environment_overrides(void) {
  QuockerImageDefaults *defaults = NULL;
  GError *error = NULL;
  g_assert_true(quocker_image_defaults_parse(
      image_config_json, strlen(image_config_json), &defaults, &error));
  const char *command_values[] = {"--port", "9000"};
  GPtrArray *command = args(command_values, G_N_ELEMENTS(command_values));
  GHashTable *environment =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert(environment, g_strdup("MODE"), g_strdup("debug"));
  g_hash_table_insert(environment, g_strdup("EXTRA"), g_strdup("enabled"));
  g_hash_table_insert(environment, g_strdup("PATH"), NULL);
  QuockerRuntimeConfig *runtime = NULL;
  g_assert_true(quocker_runtime_config_merge(defaults, NULL, command,
                                             environment, "/workspace", "app",
                                             &runtime, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(runtime->argv->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 0), ==, "/usr/bin/server");
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 1), ==, "--port");
  g_assert_cmpstr(g_ptr_array_index(runtime->argv, 2), ==, "9000");
  g_assert_cmpstr(g_hash_table_lookup(runtime->environment, "MODE"), ==,
                  "debug");
  g_assert_cmpstr(g_hash_table_lookup(runtime->environment, "EXTRA"), ==,
                  "enabled");
  g_assert_false(g_hash_table_contains(runtime->environment, "PATH"));
  g_assert_cmpstr(runtime->working_directory, ==, "/workspace");
  g_assert_cmpstr(runtime->user, ==, "app");

  quocker_runtime_config_free(runtime);
  g_hash_table_destroy(environment);
  g_ptr_array_free(command, TRUE);
  quocker_image_defaults_free(defaults);
}

static void test_invalid_image_runtime_fields_are_rejected(void) {
  const char *invalid[] = {
      "{\"config\":{\"Entrypoint\":\"/bin/app\"}}",
      "{\"config\":{\"Env\":[\"NO_EQUALS\"]}}",
      "{\"config\":{\"Env\":[\"BAD-NAME=value\"]}}",
      "{\"config\":{\"Volumes\":[\"/data\"]}}",
      "{\"config\":{\"Labels\":{\"io.quocker.kernel.minimum\":\"latest\"}}}",
      "{\"config\":{\"Labels\":{\"io.quocker.kernel.features\":\"virtio_net,,"
      "x\"}}}",
      "{\"config\":{\"Labels\":{\"io.quocker.kernel.module_releases\":\"6.8,"
      "bad/release\"}}}",
      "{\"config\":\"invalid\"}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    QuockerImageDefaults *defaults = NULL;
    GError *error = NULL;
    g_assert_false(quocker_image_defaults_parse(invalid[i], strlen(invalid[i]),
                                                &defaults, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_null(defaults);
  }
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/image/inherit",
                  test_image_defaults_parse_and_inherit);
  g_test_add_func("/quocker/image/entrypoint-precedence",
                  test_compose_entrypoint_clears_image_command);
  g_test_add_func("/quocker/image/command-env-precedence",
                  test_command_and_environment_overrides);
  g_test_add_func("/quocker/image/invalid-fields",
                  test_invalid_image_runtime_fields_are_rejected);
  return g_test_run();
}
