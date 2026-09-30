#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  g_unsetenv("QUOCKER_CONFIG_INTERPOLATION_MISSING");
  g_unsetenv("QUOCKER_CONFIG_UNRESOLVED");
  g_setenv("QUOCKER_VARIABLE_REQUIRED", "provided", TRUE);
  char *arguments[] = {(char *)cli, (char *)"-f", argv[1], (char *)"config",
                       (char *)"--format", (char *)"json", NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);

  JsonParser *parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, output, -1, &error));
  g_assert_no_error(error);
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  char *resource_directory = g_path_get_dirname(argv[1]);
  char *config_file =
      g_build_filename(resource_directory, "config-json-config.txt", NULL);
  char *expected_config_file = g_canonicalize_filename(config_file, NULL);
  char *secret_file = g_build_filename(resource_directory, "..",
                                       "config-json-secret.txt", NULL);
  char *expected_secret_file = g_canonicalize_filename(secret_file, NULL);
  JsonObject *configs = json_object_get_object_member(root, "configs");
  JsonObject *secrets = json_object_get_object_member(root, "secrets");
  g_assert_cmpstr(json_object_get_string_member(
                      json_object_get_object_member(configs, "app-config"),
                      "file"),
                  ==, expected_config_file);
  g_assert_cmpstr(json_object_get_string_member(
                      json_object_get_object_member(secrets, "app-secret"),
                      "file"),
                  ==, expected_secret_file);
  g_free(expected_secret_file);
  g_free(secret_file);
  g_free(expected_config_file);
  g_free(config_file);
  g_free(resource_directory);
  JsonObject *services = json_object_get_object_member(root, "services");
  JsonObject *app = json_object_get_object_member(services, "app");
  JsonObject *build = json_object_get_object_member(app, "build");
  char *build_directory = g_path_get_dirname(argv[1]);
  char *build_context =
      g_build_filename(build_directory, "config-json-build", NULL);
  char *expected_build_context = g_canonicalize_filename(build_context, NULL);
  g_assert_cmpstr(json_object_get_string_member(build, "context"), ==,
                  expected_build_context);
  g_assert_cmpstr(json_object_get_string_member(build, "dockerfile"), ==,
                  "Dockerfile");
  JsonObject *list_service =
      json_object_get_object_member(services, "list-context");
  JsonObject *list_build =
      json_object_get_object_member(list_service, "build");
  JsonObject *additional_contexts =
      json_object_get_object_member(list_build, "additional_contexts");
  char *additional_directory = g_path_get_dirname(argv[1]);
  char *additional_context =
      g_build_filename(additional_directory, "config-json-extra", NULL);
  char *expected_additional_context =
      g_canonicalize_filename(additional_context, NULL);
  g_assert_cmpstr(json_object_get_string_member(additional_contexts, "extra"),
                  ==, expected_additional_context);
  g_free(expected_additional_context);
  g_free(additional_context);
  g_free(additional_directory);
  g_free(expected_build_context);
  g_free(build_context);
  g_free(build_directory);
  g_assert_cmpstr(json_object_get_string_member(app, "image"), ==,
                  "alpine:latest");
  JsonNode *cpus = json_object_get_member(app, "cpus");
  g_assert_true(JSON_NODE_HOLDS_VALUE(cpus));
  g_assert_cmpint(json_node_get_int(cpus), ==, 2);
  g_assert_true(json_object_get_boolean_member(app, "privileged"));
  g_assert_cmpstr(json_object_get_string_member(app, "mem_limit"), ==, "1G");
  JsonArray *command = json_object_get_array_member(app, "command");
  g_assert_cmpstr(json_array_get_string_element(command, 1), ==, "true");
  g_assert_cmpstr(json_array_get_string_element(command, 2), ==,
                  "$${LITERAL}");
  g_assert_false(json_object_has_member(app, "env_file"));
  JsonObject *environment = json_object_get_object_member(app, "environment");
  g_assert_cmpstr(json_object_get_string_member(environment, "BASE_ONLY"), ==,
                  "base");
  g_assert_cmpstr(json_object_get_string_member(environment, "OVERRIDE_ONLY"),
                  ==, "override");
  g_assert_cmpstr(json_object_get_string_member(environment, "SHARED"), ==,
                  "from-service");
  g_assert_cmpstr(json_object_get_string_member(environment, "OVERRIDE_ME"),
                  ==, "");
  g_assert_true(JSON_NODE_HOLDS_NULL(
      json_object_get_member(environment, "QUOCKER_CONFIG_UNRESOLVED")));
  g_assert_cmpstr(json_object_get_string_member(environment, "ESCAPED_ENV"),
                  ==, "$${LITERAL_ENV}");
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_TYPED_INT"),
                  ==, "7");
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_TYPED_BOOL"),
                  ==, "true");
  JsonArray *mounts = json_object_get_array_member(app, "volumes");
  char *fixture_directory = g_path_get_dirname(argv[1]);
  char *short_source_path =
      g_build_filename(fixture_directory, "relative-volume", NULL);
  char *expected_short_source =
      g_canonicalize_filename(short_source_path, NULL);
  char *expected_short_mount = g_strdup_printf("%s:/data:ro",
                                               expected_short_source);
  g_assert_cmpstr(json_array_get_string_element(mounts, 0), ==,
                  expected_short_mount);
  JsonObject *long_mount = json_array_get_object_element(mounts, 1);
  char *long_source_path =
      g_build_filename(fixture_directory, "..", "long-volume", NULL);
  char *expected_long_source = g_canonicalize_filename(long_source_path, NULL);
  g_assert_cmpstr(json_object_get_string_member(long_mount, "source"), ==,
                  expected_long_source);
  g_free(expected_long_source);
  g_free(long_source_path);
  g_free(expected_short_mount);
  g_free(expected_short_source);
  g_free(short_source_path);
  g_free(fixture_directory);
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_DEFAULT_SET"),
                  ==, "present");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_DEFAULT_MISSING"),
                  ==, "fallback");
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_DEFAULT_EMPTY"),
                  ==, "fallback");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_DEFAULT_SET_NO_COLON"),
                  ==, "present");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_DEFAULT_MISSING_NO_COLON"),
                  ==, "fallback");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_DEFAULT_EMPTY_NO_COLON"),
                  ==, "");
  g_assert_cmpstr(json_object_get_string_member(environment, "QUOCKER_PLUS_SET"),
                  ==, "yes");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_PLUS_MISSING"),
                  ==, "");
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_PLUS_EMPTY"),
                  ==, "yes");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_COLON_PLUS_SET"),
                  ==, "yes");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_COLON_PLUS_MISSING"),
                  ==, "");
  g_assert_cmpstr(json_object_get_string_member(
                      environment, "QUOCKER_COLON_PLUS_EMPTY"),
                  ==, "");

  char *no_resolution_arguments[] = {
      (char *)cli, (char *)"-f", argv[1], (char *)"config",
      (char *)"--no-env-resolution", (char *)"--format", (char *)"json",
      NULL};
  g_free(output);
  g_free(stderr_text);
  output = NULL;
  stderr_text = NULL;
  status = 0;
  g_assert_true(g_spawn_sync(NULL, no_resolution_arguments, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, &output,
                             &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_true(json_parser_load_from_data(parser, output, -1, &error));
  g_assert_no_error(error);
  root = json_node_get_object(json_parser_get_root(parser));
  services = json_object_get_object_member(root, "services");
  app = json_object_get_object_member(services, "app");
  g_assert_true(json_object_has_member(app, "env_file"));
  g_assert_false(json_object_has_member(
      json_object_get_object_member(app, "environment"), "BASE_ONLY"));
  g_assert_cmpstr(json_object_get_string_member(
                      json_object_get_object_member(app, "environment"),
                      "QUOCKER_TYPED_INT"),
                  ==, "7");
  JsonArray *env_files = json_object_get_array_member(app, "env_file");
  g_assert_cmpuint(json_array_get_length(env_files), ==, 2);
  JsonObject *first_env_file =
      json_array_get_object_element(env_files, 0);
  fixture_directory = g_path_get_dirname(argv[1]);
  char *fixture_env_path =
      g_build_filename(fixture_directory, "config-json-base.env", NULL);
  char *expected_env_path = g_canonicalize_filename(fixture_env_path, NULL);
  g_assert_cmpstr(json_object_get_string_member(first_env_file, "path"), ==,
                  expected_env_path);
  g_free(expected_env_path);
  g_free(fixture_env_path);
  g_free(fixture_directory);

  char *no_interpolate_arguments[] = {
      (char *)cli, (char *)"-f", argv[1], (char *)"config",
      (char *)"--no-interpolate", (char *)"--format", (char *)"json",
      NULL};
  g_free(output);
  g_free(stderr_text);
  output = NULL;
  stderr_text = NULL;
  status = 0;
  g_assert_true(g_spawn_sync(NULL, no_interpolate_arguments, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, &output,
                             &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_true(json_parser_load_from_data(parser, output, -1, &error));
  g_assert_no_error(error);
  root = json_node_get_object(json_parser_get_root(parser));
  services = json_object_get_object_member(root, "services");
  app = json_object_get_object_member(services, "app");
  environment = json_object_get_object_member(app, "environment");
  g_assert_cmpstr(json_object_get_string_member(environment,
                                                "QUOCKER_DEFAULT_SET"),
                  ==, "${QUOCKER_CONFIG_INTERPOLATION_SET:-fallback}");
  g_assert_false(json_object_has_member(environment, "BASE_ONLY"));
  g_assert_true(json_object_has_member(app, "env_file"));

  char *no_path_arguments[] = {
      (char *)cli, (char *)"-f", argv[1], (char *)"config",
      (char *)"--no-path-resolution", (char *)"--format", (char *)"json",
      NULL};
  g_free(output);
  g_free(stderr_text);
  output = NULL;
  stderr_text = NULL;
  status = 0;
  g_assert_true(g_spawn_sync(NULL, no_path_arguments, NULL, G_SPAWN_DEFAULT,
                             NULL, NULL, &output, &stderr_text, &status,
                             &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_true(json_parser_load_from_data(parser, output, -1, &error));
  g_assert_no_error(error);
  root = json_node_get_object(json_parser_get_root(parser));
  configs = json_object_get_object_member(root, "configs");
  secrets = json_object_get_object_member(root, "secrets");
  g_assert_cmpstr(json_object_get_string_member(
                      json_object_get_object_member(configs, "app-config"),
                      "file"),
                  ==, "./config-json-config.txt");
  g_assert_cmpstr(json_object_get_string_member(
                      json_object_get_object_member(secrets, "app-secret"),
                      "file"),
                  ==, "../config-json-secret.txt");
  services = json_object_get_object_member(root, "services");
  app = json_object_get_object_member(services, "app");
  build = json_object_get_object_member(app, "build");
  g_assert_cmpstr(json_object_get_string_member(build, "context"), ==,
                  "./config-json-build");
  g_assert_cmpstr(json_object_get_string_member(build, "dockerfile"), ==,
                  "Dockerfile");
  list_service = json_object_get_object_member(services, "list-context");
  list_build = json_object_get_object_member(list_service, "build");
  additional_contexts =
      json_object_get_object_member(list_build, "additional_contexts");
  g_assert_cmpstr(json_object_get_string_member(additional_contexts, "extra"),
                  ==, "./config-json-extra");
  mounts = json_object_get_array_member(app, "volumes");
  g_assert_cmpstr(json_array_get_string_element(mounts, 0), ==,
                  "./relative-volume:/data:ro");
  long_mount = json_array_get_object_element(mounts, 1);
  g_assert_cmpstr(json_object_get_string_member(long_mount, "source"), ==,
                  "../long-volume");

  char *variables_arguments[] = {
      (char *)cli, (char *)"-f", argv[1], (char *)"config",
      (char *)"--variables", NULL};
  g_unsetenv("QUOCKER_VARIABLE_REQUIRED");
  g_free(output);
  g_free(stderr_text);
  output = NULL;
  stderr_text = NULL;
  status = 0;
  g_assert_true(g_spawn_sync(NULL, variables_arguments, NULL, G_SPAWN_DEFAULT,
                             NULL, NULL, &output, &stderr_text, &status,
                             &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(output, "NAME"));
  g_assert_nonnull(strstr(output, "REQUIRED"));
  g_assert_nonnull(strstr(output,
                          "QUOCKER_CONFIG_INTERPOLATION_SET"));
  g_assert_nonnull(strstr(output, "QUOCKER_VARIABLE_REQUIRED"));
  g_assert_nonnull(strstr(output, "true"));
  g_assert_nonnull(strstr(output, "fallback"));
  g_assert_nonnull(strstr(output, "yes"));

  char *fixture_directory_for_variables = g_path_get_dirname(argv[1]);
  char *variables_fixture =
      g_build_filename(fixture_directory_for_variables,
                       "config-variables.yaml", NULL);
  char *included_variables_arguments[] = {
      (char *)cli, (char *)"-f", variables_fixture, (char *)"config",
      (char *)"--variables", NULL};
  g_free(output);
  g_free(stderr_text);
  output = NULL;
  stderr_text = NULL;
  status = 0;
  g_assert_true(g_spawn_sync(NULL, included_variables_arguments, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, &output,
                             &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(output, "QUOCKER_IMAGE_TAG"));
  g_assert_nonnull(strstr(output, "QUOCKER_INCLUDE_TAG"));
  g_assert_nonnull(strstr(output, "QUOCKER_BASE_LABEL"));
  g_free(variables_fixture);
  g_free(fixture_directory_for_variables);

  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
