#include <glib.h>

static gchar *run_config(const char *cli, const char *file, gboolean compose) {
  char *direct[] = {(char *)cli, (char *)"-f", (char *)file,
                    (char *)"config", NULL};
  char *nested[] = {(char *)cli, (char *)"compose", (char *)"-f",
                    (char *)file, (char *)"config", NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, compose ? nested : direct, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, &output,
                             &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(output);
  g_assert_cmpstr(stderr_text, ==, "");
  g_free(stderr_text);
  return output;
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  gchar *direct = run_config(cli, argv[1], FALSE);
  gchar *compose = run_config(cli, argv[1], TRUE);
  g_assert_cmpstr(direct, ==, compose);
  g_free(compose);
  g_free(direct);
  return 0;
}
