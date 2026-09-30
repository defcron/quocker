#include <glib.h>
#include <json-glib/json-glib.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  char *arguments[] = {
      (char *)cli,        (char *)"-f",   argv[1], (char *)"config",
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
  JsonObject *services = json_object_get_object_member(root, "services");
  g_assert_true(json_object_has_member(services, "base"));
  g_assert_false(json_object_has_member(services, "common"));

  JsonObject *child = json_object_get_object_member(services, "child");
  g_assert_false(json_object_has_member(child, "extends"));
  JsonArray *command = json_object_get_array_member(child, "command");
  g_assert_cmpuint(json_array_get_length(command), ==, 3);
  g_assert_cmpstr(json_array_get_string_element(command, 2), ==, "child");
  JsonObject *environment = json_object_get_object_member(child, "environment");
  g_assert_cmpstr(json_object_get_string_member(environment, "BASE"), ==,
                  "yes");
  g_assert_cmpstr(json_object_get_string_member(environment, "OVERRIDE"), ==,
                  "child");
  g_assert_cmpstr(json_object_get_string_member(environment, "CHILD"), ==,
                  "yes");
  JsonArray *expose = json_object_get_array_member(child, "expose");
  g_assert_cmpuint(json_array_get_length(expose), ==, 3);
  g_assert_cmpint(json_node_get_int(json_array_get_element(expose, 0)), ==, 80);
  g_assert_cmpint(json_node_get_int(json_array_get_element(expose, 1)), ==, 81);
  g_assert_cmpint(json_node_get_int(json_array_get_element(expose, 2)), ==, 82);

  JsonObject *external =
      json_object_get_object_member(services, "external_child");
  g_assert_false(json_object_has_member(external, "extends"));
  char *compose_directory = g_path_get_dirname(argv[1]);
  char *expected_image_path = g_build_filename(
      compose_directory, "extends", "fragments", "guest.qcow2", NULL);
  char *expected_image = g_canonicalize_filename(expected_image_path, NULL);
  g_assert_cmpstr(json_object_get_string_member(external, "image"), ==,
                  expected_image);
  g_assert_false(json_object_has_member(external, "env_file"));
  JsonObject *external_environment =
      json_object_get_object_member(external, "environment");
  g_assert_cmpstr(json_object_get_string_member(external_environment, "SOURCE"),
                  ==, "external");
  g_assert_cmpstr(
      json_object_get_string_member(external_environment, "FROM_EXTENDS"), ==,
      "external");

  char *no_path_arguments[] = {
      (char *)cli, (char *)"-f", argv[1], (char *)"config",
      (char *)"--no-env-resolution", (char *)"--no-path-resolution",
      (char *)"--format", (char *)"json", NULL};
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
  services = json_object_get_object_member(root, "services");
  external = json_object_get_object_member(services, "external_child");
  g_assert_cmpstr(json_object_get_string_member(external, "image"), ==,
                  "./guest.qcow2");
  JsonArray *env_files = json_object_get_array_member(external, "env_file");
  JsonObject *external_env_file = json_array_get_object_element(env_files, 0);
  g_assert_cmpstr(json_object_get_string_member(external_env_file, "path"),
                  ==, "extends/fragments/common.env");

  g_free(expected_image);
  g_free(expected_image_path);
  g_free(compose_directory);
  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
