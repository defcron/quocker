#include <glib.h>

static void assert_cli_output(const char *cli, const char *compose_file,
                              const char *option, const char *expected) {
  char *arguments[] = {(char *)cli,      (char *)"-f",   (char *)compose_file,
                       (char *)"config", (char *)option, NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(output, ==, expected);
  g_free(stderr_text);
  g_free(output);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  assert_cli_output(cli, argv[1], "--services", "alpha\napp\nzeta\n");
  assert_cli_output(cli, argv[1], "--profiles", "blue\ngreen\n");
  assert_cli_output(cli, argv[1], "--images",
                    "alpine:latest\nregistry.example/alpha:latest\n"
                    "registry.example/zeta:latest\n");
  assert_cli_output(cli, argv[1], "--volumes", "cache\ndata\n");
  assert_cli_output(cli, argv[1], "--networks", "backend\nfrontend\n");
  assert_cli_output(cli, argv[1], "--models", "llama\nvision\n");
  return 0;
}
