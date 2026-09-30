#include "../../scripts/quocker-registry-config.h"

#include <glib.h>
#include <glib/gstdio.h>

static char *config_path;

static void write_config(const char *contents) {
  GError *error = NULL;
  g_assert_true(g_file_set_contents(config_path, contents, -1, &error));
  g_assert_no_error(error);
}

static void test_docker_hub_aliases(void) {
  write_config("{\"mirrors\":{\"docker.io\":\"https://cache.example/oci\"}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("registry-1.docker.io", NULL,
                                                  &error);
  g_assert_no_error(error);
  g_assert_cmpstr(mirror, ==, "https://cache.example/oci");
  g_free(mirror);
}

static void test_registry_specific_mapping(void) {
  write_config("{\"mirrors\":{\"docker.io\":\"https://hub-cache.example\","
               "\"registry.example:5443\":\"https://private-cache.example/base\"}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("registry.example:5443", NULL,
                                                  &error);
  g_assert_no_error(error);
  g_assert_cmpstr(mirror, ==, "https://private-cache.example/base");
  g_free(mirror);
  mirror = quocker_registry_mirror_resolve("other.example", NULL, &error);
  g_assert_no_error(error);
  g_assert_null(mirror);
}

static void test_global_override_precedence(void) {
  write_config("{\"mirrors\":{\"registry.example\":"
               "\"https://configured.example\"}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve(
      "registry.example", "https://override.example", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(mirror, ==, "https://override.example");
  g_free(mirror);
}

static void test_insecure_mirror_rejected(void) {
  write_config("{\"mirrors\":{\"registry.example\":"
               "\"http://mirror.example\"}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("registry.example", NULL,
                                                  &error);
  g_assert_null(mirror);
  g_assert_error(error, g_quark_from_static_string(
                            "quocker-registry-config-error"), 6);
  g_clear_error(&error);
}

static void test_invalid_config_rejected(void) {
  write_config("{\"registries\":{}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("registry.example", NULL,
                                                  &error);
  g_assert_null(mirror);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

static void test_missing_config_is_optional(void) {
  g_assert_cmpint(g_unlink(config_path), ==, 0);
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("registry.example", NULL,
                                                  &error);
  g_assert_no_error(error);
  g_assert_null(mirror);
}

static void test_conflicting_docker_hub_aliases_rejected(void) {
  write_config("{\"mirrors\":{\"docker.io\":\"https://one.example\","
               "\"registry-1.docker.io\":\"https://two.example\"}}");
  GError *error = NULL;
  char *mirror = quocker_registry_mirror_resolve("docker.io", NULL, &error);
  g_assert_null(mirror);
  g_assert_error(error, g_quark_from_static_string(
                            "quocker-registry-config-error"), 8);
  g_clear_error(&error);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-registry-config-XXXXXX", &error);
  g_assert_no_error(error);
  config_path = g_build_filename(directory, "registries.json", NULL);
  g_setenv("QUOCKER_REGISTRY_CONFIG", config_path, TRUE);
  g_test_add_func("/quocker/registry-config/docker-hub-aliases",
                  test_docker_hub_aliases);
  g_test_add_func("/quocker/registry-config/host-mapping",
                  test_registry_specific_mapping);
  g_test_add_func("/quocker/registry-config/global-precedence",
                  test_global_override_precedence);
  g_test_add_func("/quocker/registry-config/insecure-url",
                  test_insecure_mirror_rejected);
  g_test_add_func("/quocker/registry-config/invalid-config",
                  test_invalid_config_rejected);
  g_test_add_func("/quocker/registry-config/missing-config",
                  test_missing_config_is_optional);
  g_test_add_func("/quocker/registry-config/conflicting-aliases",
                  test_conflicting_docker_hub_aliases_rejected);
  int result = g_test_run();
  g_unlink(config_path);
  g_free(config_path);
  g_rmdir(directory);
  g_free(directory);
  return result;
}
