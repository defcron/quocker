#include <glib.h>
#include <string.h>

static gchar *run_version(const char *cli, const char *argument) {
  char *argv[] = {(char *)cli, (char *)"version", NULL, NULL};
  if (argument && *argument) {
    argv[2] = (char *)argument;
  }
  gchar *output = NULL;
  gchar *error_output = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &error_output, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_true(!error_output || !*error_output);
  g_free(error_output);
  return output;
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  gchar *output = run_version(cli, NULL);
  g_assert_true(g_str_has_prefix(output, "quocker version 0.1 (QEMU "));
  g_assert_true(g_str_has_suffix(output, ")\n"));
  g_free(output);
  output = run_version(cli, "--short");
  g_assert_cmpstr(output, ==, "0.1\n");
  g_free(output);
  char *version_argv[] = {(char *)cli, (char *)"--version", NULL};
  gchar *error_output = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, version_argv, NULL, G_SPAWN_DEFAULT, NULL,
                             NULL, &output, &error_output, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_true(g_str_has_prefix(output, "quocker version 0.1 (QEMU "));
  g_assert_true(!error_output || !*error_output);
  g_free(error_output);
  g_free(output);
  return 0;
}
