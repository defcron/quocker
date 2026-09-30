#include <glib.h>
#include <json-glib/json-glib.h>

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
  JsonObject *services = json_object_get_object_member(root, "services");
  JsonObject *app = json_object_get_object_member(services, "app");

  JsonArray *expose = json_object_get_array_member(app, "expose");
  g_assert_cmpuint(json_array_get_length(expose), ==, 0);
  JsonObject *environment = json_object_get_object_member(app, "environment");
  g_assert_cmpuint(json_object_get_size(environment), ==, 0);

  JsonArray *ports = json_object_get_array_member(app, "ports");
  g_assert_cmpuint(json_array_get_length(ports), ==, 3);
  JsonObject *first_port =
      json_node_get_object(json_array_get_element(ports, 0));
  g_assert_cmpint(
      json_node_get_int(json_object_get_member(first_port, "target")), ==, 80);
  g_assert_cmpstr(json_object_get_string_member(first_port, "published"), ==,
                  "8080");
  g_assert_cmpstr(json_array_get_string_element(ports, 1), ==, "9090:90");
  g_assert_cmpstr(json_array_get_string_element(ports, 2), ==, "8081:81");

  JsonArray *volumes = json_object_get_array_member(app, "volumes");
  g_assert_cmpuint(json_array_get_length(volumes), ==, 2);
  g_assert_cmpstr(json_array_get_string_element(volumes, 0), ==,
                  "new-cache:/data");
  g_assert_cmpstr(json_array_get_string_element(volumes, 1), ==, "logs:/logs");

  JsonArray *secrets = json_object_get_array_member(app, "secrets");
  g_assert_cmpuint(json_array_get_length(secrets), ==, 1);
  g_assert_cmpstr(
      json_object_get_string_member(
          json_node_get_object(json_array_get_element(secrets, 0)), "source"),
      ==, "new-secret");
  JsonArray *configs = json_object_get_array_member(app, "configs");
  g_assert_cmpuint(json_array_get_length(configs), ==, 1);
  g_assert_cmpstr(
      json_object_get_string_member(
          json_node_get_object(json_array_get_element(configs, 0)), "source"),
      ==, "new-config");

  JsonArray *command = json_object_get_array_member(app, "command");
  g_assert_cmpuint(json_array_get_length(command), ==, 2);
  g_assert_cmpstr(json_array_get_string_element(command, 1), ==, "override");
  JsonObject *healthcheck = json_object_get_object_member(app, "healthcheck");
  JsonArray *test = json_object_get_array_member(healthcheck, "test");
  g_assert_cmpuint(json_array_get_length(test), ==, 2);
  g_assert_cmpstr(json_array_get_string_element(test, 1), ==, "false");

  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
