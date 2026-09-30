#include <glib.h>
#include <json-glib/json-glib.h>

static void assert_project_name(const char *cli, const char *file,
                                const char *explicit_name,
                                const char *environment_name,
                                const char *expected) {
  char *arguments[10];
  guint argc = 0;
  arguments[argc++] = (char *)cli;
  arguments[argc++] = (char *)"-f";
  arguments[argc++] = (char *)file;
  if (explicit_name) {
    arguments[argc++] = (char *)"-p";
    arguments[argc++] = (char *)explicit_name;
  }
  arguments[argc++] = (char *)"config";
  arguments[argc++] = (char *)"--format";
  arguments[argc++] = (char *)"json";
  arguments[argc] = NULL;

  gchar **environment = g_get_environ();
  environment = g_environ_unsetenv(environment, "COMPOSE_PROJECT_NAME");
  environment =
      g_environ_setenv(environment, "COMPOSE_DISABLE_ENV_FILE", "1", TRUE);
  if (environment_name) {
    environment = g_environ_setenv(environment, "COMPOSE_PROJECT_NAME",
                                   environment_name, TRUE);
  }
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, environment, G_SPAWN_DEFAULT,
                             NULL, NULL, &stdout_text, &stderr_text, &status,
                             &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);

  JsonParser *parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, stdout_text, -1, &error));
  g_assert_no_error(error);
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  g_assert_cmpstr(json_object_get_string_member(root, "name"), ==, expected);
  JsonObject *services = json_object_get_object_member(root, "services");
  JsonObject *app = json_object_get_object_member(services, "app");
  JsonObject *labels = json_object_get_object_member(app, "labels");
  if (json_object_has_member(labels, "resolved_project")) {
    g_assert_cmpstr(json_object_get_string_member(labels, "resolved_project"),
                    ==, expected);
  }

  g_object_unref(parser);
  g_strfreev(environment);
  g_free(stderr_text);
  g_free(stdout_text);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 3);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  assert_project_name(cli, argv[1], NULL, NULL, "myproject");
  assert_project_name(cli, argv[1], "command-line", NULL, "command-line");
  assert_project_name(cli, argv[1], NULL, "Env.Project", "envproject");
  assert_project_name(cli, argv[2], NULL, NULL, "projectname");
  return 0;
}
