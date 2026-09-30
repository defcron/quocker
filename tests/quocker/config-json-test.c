#include <glib.h>
#include <json-glib/json-glib.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  g_unsetenv("QUOCKER_CONFIG_INTERPOLATION_MISSING");
  g_unsetenv("QUOCKER_CONFIG_UNRESOLVED");
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

  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
