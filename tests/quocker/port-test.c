#include <glib.h>
#include <string.h>

static void check_port(const char *cli, const char *compose,
                       const char *private_port, const char *expected,
                       gboolean should_succeed) {
  char *arguments[] = {(char *)cli, (char *)"-f", (char *)compose,
                       (char *)"port", (char *)"app", (char *)private_port,
                       NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  gboolean succeeded = g_spawn_check_wait_status(status, &error);
  if (should_succeed) {
    g_assert_true(succeeded);
    g_assert_no_error(error);
    g_assert_cmpstr(output, ==, expected);
  } else {
    g_assert_false(succeeded);
    g_assert_nonnull(strstr(stderr_text, expected));
    g_clear_error(&error);
  }
  g_free(output);
  g_free(stderr_text);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 3);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  check_port(cli, argv[1], "80", "127.0.0.1:8080\n", TRUE);
  check_port(cli, argv[1], "9001/udp", "0.0.0.0:9001\n", TRUE);
  check_port(cli, argv[1], "444/tcp", "0.0.0.0:8443\n", TRUE);
  check_port(cli, argv[1], "443", "has no published tcp port 443", FALSE);
  check_port(cli, argv[2], "80", "must be running", FALSE);
  char *dry_run_arguments[] = {(char *)cli, (char *)"-f", (char *)argv[2],
                              (char *)"up", (char *)"--dry-run", NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, dry_run_arguments, NULL, G_SPAWN_DEFAULT,
                             NULL, NULL, &output, &stderr_text, &status,
                             &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(output, "hostfwd=tcp::0-:80"));
  g_free(output);
  g_free(stderr_text);
  return 0;
}
