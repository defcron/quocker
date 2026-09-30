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
  gboolean spawned = g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL,
                                  NULL, &stdout_text, &stderr_text, &status,
                                  &error);
  if (!spawned) {
    g_error("could not execute quocker: %s", error->message);
  }
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_nonnull(strstr(stdout_text, "Would start service app"));
  g_assert_null(strstr(stdout_text, "Would start service optional-service"));
  g_assert_nonnull(strstr(stderr_text, "optional dependency"));
  g_assert_nonnull(strstr(stderr_text, "inactive profile; skipping it"));

  g_free(stdout_text);
  g_free(stderr_text);
  g_clear_error(&error);
  return 0;
}
