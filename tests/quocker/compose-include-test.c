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
  g_assert_true(json_object_has_member(services, "nested"));
  g_assert_true(json_object_has_member(services, "second"));
  JsonObject *included = json_object_get_object_member(services, "included");
  g_assert_cmpstr(json_object_get_string_member(included, "image"), ==,
                  "./local-override.qcow2");
  JsonObject *shared = json_object_get_object_member(services, "shared");
  g_assert_cmpstr(json_object_get_string_member(shared, "image"), ==,
                  "./local-shared.qcow2");
  g_assert_nonnull(strstr(stderr_text, "included Compose resource"));
  g_object_unref(parser);
  g_free(stderr_text);
  g_free(output);
  return 0;
}
