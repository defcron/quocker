#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 3);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  char *arguments[] = {(char *)cli,        (char *)"-f",   argv[1],
                       (char *)"-f",       argv[2],        (char *)"config",
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
  g_assert_false(json_object_has_member(root, "include"));
  JsonObject *services = json_object_get_object_member(root, "services");
  g_assert_true(json_object_has_member(services, "local"));
  g_assert_true(json_object_has_member(services, "included"));
  g_assert_true(json_object_has_member(services, "included_env"));
  g_assert_true(json_object_has_member(services, "nested"));
  g_assert_true(json_object_has_member(services, "second"));
  JsonObject *included = json_object_get_object_member(services, "included");
  g_assert_cmpstr(json_object_get_string_member(included, "image"), ==,
                  "./local-override.qcow2");
  JsonObject *included_env =
      json_object_get_object_member(services, "included_env");
  char *project_directory = g_path_get_dirname(argv[1]);
  char *expected_include_path = g_build_filename(project_directory, "fragments",
                                                 "parent-override.qcow2", NULL);
  char *expected_include_image =
      g_canonicalize_filename(expected_include_path, NULL);
  g_assert_cmpstr(json_object_get_string_member(included_env, "image"), ==,
                  expected_include_image);
  g_free(expected_include_image);
  JsonObject *included_environment =
      json_object_get_object_member(included_env, "environment");
  g_assert_cmpstr(
      json_object_get_string_member(included_environment, "INCLUDED_ONLY"), ==,
      "from-last-include-env");
  g_assert_cmpstr(
      json_object_get_string_member(included_environment, "INCLUDED_EXTRA"), ==,
      "from-include-env-list");
  JsonObject *nested = json_object_get_object_member(services, "nested");
  char *expected_nested_path =
      g_build_filename(project_directory, "fragments", "nestedbase",
                       "nested-from-dotenv.qcow2", NULL);
  char *expected_nested_image =
      g_canonicalize_filename(expected_nested_path, NULL);
  g_assert_cmpstr(json_object_get_string_member(nested, "image"), ==,
                  expected_nested_image);
  g_free(expected_include_path);
  g_free(expected_nested_image);
  g_free(expected_nested_path);
  g_free(project_directory);
  JsonObject *second = json_object_get_object_member(services, "second");
  g_assert_nonnull(strstr(json_object_get_string_member(second, "image"),
                          "second-override.qcow2"));
  JsonObject *shared = json_object_get_object_member(services, "shared");
  g_assert_cmpstr(json_object_get_string_member(shared, "image"), ==,
                  "./local-shared.qcow2");
  g_assert_nonnull(strstr(stderr_text, "included Compose resource"));
  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
