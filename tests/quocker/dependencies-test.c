#include <glib.h>
#include <string.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  char *arguments[] = {(char *)cli, (char *)"-f", argv[1], (char *)"up",
                       (char *)"--dry-run", (char *)"app", NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  gboolean spawned = g_spawn_sync(NULL, arguments, NULL,
                                  G_SPAWN_DEFAULT, NULL, NULL,
                                  &stdout_text, &stderr_text, &status, &error);
  if (!spawned) {
    g_error("could not execute quocker: %s", error->message);
  }
  g_assert_true(g_spawn_check_wait_status(status, &error));
  const char *database = strstr(stdout_text, "Would start service database\n");
  const char *app =
      strstr(stdout_text, "Would start service app (QEMU user network:");
  g_assert_nonnull(database);
  g_assert_nonnull(app);
  g_assert_true(database < app);
  g_assert_nonnull(strstr(stdout_text, "hostfwd=tcp:127.0.0.1:8080-:80"));
  g_assert_nonnull(strstr(stdout_text, "hostfwd=udp::9000-:80"));
  g_assert_nonnull(strstr(stdout_text, "hostfwd=udp::9001-:81"));
  g_assert_nonnull(strstr(stdout_text,
                          "hostfwd=udp:127.0.0.1:5353-:53"));
  g_free(stdout_text);
  g_free(stderr_text);
  stdout_text = NULL;
  stderr_text = NULL;
  status = 0;
  char *down_arguments[] = {(char *)cli, (char *)"-f", argv[1],
                            (char *)"down", (char *)"--dry-run",
                            (char *)"app", NULL};
  spawned = g_spawn_sync(NULL, down_arguments, NULL, G_SPAWN_DEFAULT, NULL,
                         NULL, &stdout_text, &stderr_text, &status, &error);
  if (!spawned) {
    g_error("could not execute quocker down: %s", error->message);
  }
  g_assert_true(g_spawn_check_wait_status(status, &error));
  app = strstr(stdout_text, "Would stop service app\n");
  database = strstr(stdout_text, "Would stop service database\n");
  g_assert_nonnull(app);
  g_assert_nonnull(database);
  g_assert_true(app < database);
  g_free(stdout_text);
  g_free(stderr_text);
  g_clear_error(&error);
  return 0;
}
