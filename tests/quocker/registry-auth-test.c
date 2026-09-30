#include "quocker-registry-auth.h"

#include <glib.h>
#include <glib/gstdio.h>

static char *config_directory;
static const char *helper_directory;

static void write_config(const char *json) {
  char *path = g_build_filename(config_directory, "config.json", NULL);
  GError *error = NULL;
  g_assert_true(g_file_set_contents(path, json, -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(path, 0600), ==, 0);
  g_free(path);
}

static void test_docker_hub_base64_auth(void) {
  write_config("{\"auths\":{\"https://index.docker.io/v1/\":{"
               "\"auth\":\"dXNlcjpwYXNz\"}}}");
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_true(quocker_registry_credentials_load(
      "registry-1.docker.io", &credentials, &error));
  g_assert_no_error(error);
  g_assert_nonnull(credentials);
  g_assert_cmpstr(credentials->username, ==, "user");
  g_assert_cmpstr(credentials->secret, ==, "pass");
  quocker_registry_credentials_free(credentials);
}

static void test_registry_username_password(void) {
  write_config("{\"auths\":{\"private.example\":{"
               "\"username\":\"alice\",\"password\":\"s:ecret\"}}}");
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  gboolean loaded = quocker_registry_credentials_load(
      "private.example", &credentials, &error);
  g_assert_no_error(error);
  g_assert_true(loaded);
  g_assert_cmpstr(credentials->username, ==, "alice");
  g_assert_cmpstr(credentials->secret, ==, "s:ecret");
  quocker_registry_credentials_free(credentials);
}

static void test_registry_without_auth(void) {
  write_config("{\"auths\":{}}");
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_true(quocker_registry_credentials_load(
      "private.example", &credentials, &error));
  g_assert_no_error(error);
  g_assert_null(credentials);
}

static void test_invalid_auth_is_rejected(void) {
  write_config("{\"auths\":{\"private.example\":{"
               "\"auth\":\"not-base64-credentials\"}}}");
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_false(quocker_registry_credentials_load(
      "private.example", &credentials, &error));
  g_assert_null(credentials);
  g_assert_error(error, g_quark_from_static_string("quocker-registry-auth-error"),
                 1);
  g_clear_error(&error);
}

static void test_per_registry_credential_helper(void) {
  write_config("{\"auths\":{\"private.example\":{"
               "\"username\":\"inline-user\",\"password\":\"inline-secret\"}},"
               "\"credHelpers\":{\"private.example\":\"quocker-test\"}}");
  g_setenv("QUOCKER_TEST_HELPER_MODE", "success", TRUE);
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  gboolean loaded = quocker_registry_credentials_load(
      "private.example", &credentials, &error);
  g_assert_no_error(error);
  g_assert_true(loaded);
  g_assert_cmpstr(credentials->username, ==, "helper-user");
  g_assert_cmpstr(credentials->secret, ==, "helper-secret");
  quocker_registry_credentials_free(credentials);
}

static void test_global_credential_store(void) {
  write_config("{\"credsStore\":\"quocker-test\"}");
  g_setenv("QUOCKER_TEST_HELPER_MODE", "success", TRUE);
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_true(quocker_registry_credentials_load(
      "private.example", &credentials, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(credentials->username, ==, "helper-user");
  quocker_registry_credentials_free(credentials);
}

static void test_missing_helper_credentials(void) {
  write_config("{\"credHelpers\":{\"private.example\":\"quocker-test\"}}");
  g_setenv("QUOCKER_TEST_HELPER_MODE", "missing", TRUE);
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_true(quocker_registry_credentials_load(
      "private.example", &credentials, &error));
  g_assert_no_error(error);
  g_assert_null(credentials);
}

static void test_helper_output_is_bounded(void) {
  write_config("{\"credHelpers\":{\"private.example\":\"quocker-test\"}}");
  g_setenv("QUOCKER_TEST_HELPER_MODE", "large", TRUE);
  QuockerRegistryCredentials *credentials = NULL;
  GError *error = NULL;
  g_assert_false(quocker_registry_credentials_load(
      "private.example", &credentials, &error));
  g_assert_null(credentials);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  GError *error = NULL;
  config_directory = g_dir_make_tmp("quocker-docker-config-XXXXXX", &error);
  g_assert_no_error(error);
  g_setenv("DOCKER_CONFIG", config_directory, TRUE);
  helper_directory = g_getenv("QUOCKER_TEST_HELPER_DIR");
  g_assert_nonnull(helper_directory);
  g_setenv("PATH", helper_directory, TRUE);
  g_test_add_func("/quocker/registry-auth/docker-hub", test_docker_hub_base64_auth);
  g_test_add_func("/quocker/registry-auth/username-password",
                  test_registry_username_password);
  g_test_add_func("/quocker/registry-auth/no-auth", test_registry_without_auth);
  g_test_add_func("/quocker/registry-auth/invalid",
                  test_invalid_auth_is_rejected);
  g_test_add_func("/quocker/registry-auth/per-registry-helper",
                  test_per_registry_credential_helper);
  g_test_add_func("/quocker/registry-auth/global-store",
                  test_global_credential_store);
  g_test_add_func("/quocker/registry-auth/missing-helper-credentials",
                  test_missing_helper_credentials);
  g_test_add_func("/quocker/registry-auth/helper-output-bounded",
                  test_helper_output_is_bounded);
  int result = g_test_run();
  char *path = g_build_filename(config_directory, "config.json", NULL);
  g_unlink(path);
  g_free(path);
  g_rmdir(config_directory);
  g_free(config_directory);
  return result;
}
