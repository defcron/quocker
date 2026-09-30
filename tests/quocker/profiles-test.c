#include <glib.h>
#include <string.h>

static char *run_quocker(const char *cli, const char *file, const char *profile,
                         const char *service) {
  char *arguments[9];
  guint i = 0;
  arguments[i++] = (char *)cli;
  arguments[i++] = (char *)"-f";
  arguments[i++] = (char *)file;
  if (profile) {
    arguments[i++] = (char *)"--profile";
    arguments[i++] = (char *)profile;
  }
  arguments[i++] = (char *)"up";
  arguments[i++] = (char *)"--dry-run";
  if (service) {
    arguments[i++] = (char *)service;
  }
  arguments[i] = NULL;

  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_free(stderr_text);
  return output;
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);

  char *output = run_quocker(cli, argv[1], NULL, "tool");
  const char *base = strstr(output, "Would start service base\n");
  const char *helper = strstr(output, "Would start service tool-helper\n");
  const char *tool = strstr(output, "Would start service tool\n");
  g_assert_nonnull(base);
  g_assert_nonnull(helper);
  g_assert_nonnull(tool);
  g_assert_true(base < helper);
  g_assert_true(helper < tool);
  g_assert_null(strstr(output, "Would start service sibling\n"));
  g_free(output);

  output = run_quocker(cli, argv[1], "tools", NULL);
  g_assert_nonnull(strstr(output, "Would start service tool\n"));
  g_assert_nonnull(strstr(output, "Would start service sibling\n"));
  g_free(output);
  return 0;
}
