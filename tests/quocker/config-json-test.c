#include <glib.h>
#include <json-glib/json-glib.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
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
  JsonObject *services = json_object_get_object_member(root, "services");
  JsonObject *app = json_object_get_object_member(services, "app");
  g_assert_cmpstr(json_object_get_string_member(app, "image"), ==,
                  "alpine:latest");
  JsonNode *cpus = json_object_get_member(app, "cpus");
  g_assert_true(JSON_NODE_HOLDS_VALUE(cpus));
  g_assert_cmpint(json_node_get_int(cpus), ==, 2);
  g_assert_true(json_object_get_boolean_member(app, "privileged"));
  g_assert_cmpstr(json_object_get_string_member(app, "mem_limit"), ==, "1G");
  JsonArray *command = json_object_get_array_member(app, "command");
  g_assert_cmpstr(json_array_get_string_element(command, 1), ==, "true");

  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
