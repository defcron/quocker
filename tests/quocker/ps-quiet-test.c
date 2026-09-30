/*
 * Verify quiet Compose-compatible output from the VM-aware ps command.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void assert_quiet_ps(const char *cli, const char *compose,
                            const char *directory, const char *option,
                            const char *all_option, const char *filter_option,
                            const char *filter_value,
                            const char *orphans_option,
                            const char *expected) {
  char *arguments[18];
  guint i = 0;
  arguments[i++] = (char *)cli;
  arguments[i++] = (char *)"--project-directory";
  arguments[i++] = (char *)directory;
  arguments[i++] = (char *)"--project-name";
  arguments[i++] = (char *)"ps-quiet-test";
  arguments[i++] = (char *)"-f";
  arguments[i++] = (char *)compose;
  arguments[i++] = (char *)"ps";
  if (all_option) {
    arguments[i++] = (char *)all_option;
  }
  if (filter_option) {
    arguments[i++] = (char *)filter_option;
    arguments[i++] = (char *)filter_value;
  }
  if (orphans_option) {
    arguments[i++] = (char *)orphans_option;
  }
  arguments[i++] = (char *)option;
  arguments[i] = NULL;
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &stdout_text, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(stdout_text, ==, expected);
  g_assert_cmpstr(stderr_text, ==, "");
  g_free(stdout_text);
  g_free(stderr_text);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-ps-quiet-XXXXXX", &error);
  g_assert_no_error(error);
  assert_quiet_ps(cli, argv[1], directory, "-q", NULL, NULL, NULL, NULL, "");
  assert_quiet_ps(cli, argv[1], directory, "--quiet", NULL, NULL, NULL, NULL,
                  "");
  assert_quiet_ps(cli, argv[1], directory, "-q", "-a", NULL, NULL, NULL, "");
  assert_quiet_ps(cli, argv[1], directory, "--quiet", "--all", NULL, NULL,
                  NULL, "");

  char *state_directory =
      g_build_filename(directory, ".quocker", "ps-quiet-test", NULL);
  g_assert_cmpint(g_mkdir_with_parents(state_directory, 0700), ==, 0);
  char *state = g_build_filename(state_directory, "app.state", NULL);
  char *disk = g_build_filename(state_directory, "app.qcow2", NULL);
  char *orphan_state = g_build_filename(state_directory, "ghost.state", NULL);
  char *orphan_disk = g_build_filename(state_directory, "ghost.qcow2", NULL);
  g_assert_true(g_file_set_contents(state,
                                   "2147483647\nps-quiet-test-app\n"
                                   "app.qcow2\n",
                                   -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(state, 0600), ==, 0);
  g_assert_true(g_file_set_contents(disk, "", 0, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_set_contents(orphan_state,
                                   "2147483647\nps-quiet-test-ghost\n"
                                   "ghost.qcow2\n",
                                   -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(orphan_state, 0600), ==, 0);
  g_assert_true(g_file_set_contents(orphan_disk, "", 0, &error));
  g_assert_no_error(error);
  assert_quiet_ps(cli, argv[1], directory, "--quiet", "--all", NULL, NULL,
                  NULL, "ps-quiet-test-app\nps-quiet-test-ghost\n");
  assert_quiet_ps(cli, argv[1], directory, "--quiet", NULL, "--status",
                  "exited", NULL,
                  "ps-quiet-test-app\nps-quiet-test-ghost\n");
  assert_quiet_ps(cli, argv[1], directory, "-q", NULL, "--filter",
                  "status=exited", NULL,
                  "ps-quiet-test-app\nps-quiet-test-ghost\n");
  assert_quiet_ps(cli, argv[1], directory, "-q", "--all", "--status",
                  "running", NULL, "");
  assert_quiet_ps(cli, argv[1], directory, "-q", NULL, NULL, NULL, NULL, "");
  assert_quiet_ps(cli, argv[1], directory, "--services", "--all", NULL,
                  NULL, NULL, "app\nghost\n");
  assert_quiet_ps(cli, argv[1], directory, "--services", "--all", NULL,
                  NULL, "--orphans=false", "app\n");
  char *invalid_arguments[] = {(char *)cli,
                               (char *)"--project-directory",
                               directory,
                               (char *)"-f",
                               argv[1],
                               (char *)"ps",
                               (char *)"--status",
                               (char *)"paused",
                               NULL};
  gchar *invalid_stdout = NULL;
  gchar *invalid_stderr = NULL;
  gint invalid_status = 0;
  g_assert_true(g_spawn_sync(NULL, invalid_arguments, NULL, G_SPAWN_DEFAULT,
                             NULL, NULL, &invalid_stdout, &invalid_stderr,
                             &invalid_status, &error));
  g_assert_no_error(error);
  g_assert_false(g_spawn_check_wait_status(invalid_status, &error));
  g_assert_error(error, G_SPAWN_EXIT_ERROR, 2);
  g_clear_error(&error);
  g_assert_nonnull(strstr(invalid_stderr, "only 'running' and 'exited'"));
  g_free(invalid_stdout);
  g_free(invalid_stderr);
  g_assert_cmpint(g_unlink(orphan_state), ==, 0);
  g_assert_cmpint(g_unlink(orphan_disk), ==, 0);
  g_assert_cmpint(g_unlink(state), ==, 0);
  g_assert_cmpint(g_unlink(disk), ==, 0);
  g_assert_cmpint(g_rmdir(state_directory), ==, 0);
  char *quocker_directory = g_path_get_dirname(state_directory);
  g_assert_cmpint(g_rmdir(quocker_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(quocker_directory);
  g_free(disk);
  g_free(state);
  g_free(state_directory);
  g_free(orphan_disk);
  g_free(orphan_state);
  g_free(directory);
  return 0;
}
