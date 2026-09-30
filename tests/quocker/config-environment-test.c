#include <glib.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 3);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  char *arguments[] = {(char *)cli,
                       (char *)"-f",
                       argv[1],
                       (char *)"--env-file",
                       argv[2],
                       (char *)"config",
                       (char *)"--environment",
                       NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(
      g_strstr_len(output, -1, "QUOCKER_TEST_FILE_ONLY=from-file\n"));
  g_assert_nonnull(
      g_strstr_len(output, -1, "QUOCKER_TEST_OVERRIDE=from-process\n"));
  g_assert_null(g_strstr_len(output, -1, "QUOCKER_TEST_OVERRIDE=from-file\n"));
  g_free(stderr_text);
  g_free(output);
  return 0;
}
