/*
 * Compose orphan lifecycle behavior.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static gboolean run_command(const char *cli, const char *directory,
                            const char *compose, const char *command,
                            const char *first_flag, const char *second_flag,
                            gchar **stdout_text, gchar **stderr_text) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"orphan-test",
                       (char *)"-f",
                       (char *)compose,
                       (char *)command,
                       (char *)first_flag,
                       (char *)second_flag,
                       NULL};
  gint status = 0;
  GError *error = NULL;
  gboolean spawned = g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL,
                                  NULL, stdout_text, stderr_text, &status,
                                  &error);
  g_assert_true(spawned);
  g_assert_no_error(error);
  gboolean succeeded = g_spawn_check_wait_status(status, &error);
  g_clear_error(&error);
  return succeeded;
}

static void write_orphan_state(const char *path) {
  GError *error = NULL;
  g_assert_true(
      g_file_set_contents(path, "2147483647\norphan-worker.qcow2\n", -1,
                          &error));
  g_assert_no_error(error);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-orphans-XXXXXX", &error);
  g_assert_no_error(error);
  char *compose = g_build_filename(directory, "compose.yaml", NULL);
  g_assert_true(g_file_set_contents(
      compose, "services:\n  app:\n    image: ./app.qcow2\n", -1, &error));
  g_assert_no_error(error);
  char *state_directory =
      g_build_filename(directory, ".quocker", "orphan-test", NULL);
  g_assert_cmpint(g_mkdir_with_parents(state_directory, 0700), ==, 0);
  char *orphan_state =
      g_build_filename(state_directory, "orphan-worker.state", NULL);
  char *orphan_disk =
      g_build_filename(state_directory, "orphan-worker.qcow2", NULL);
  write_orphan_state(orphan_state);
  g_assert_true(g_file_set_contents(orphan_disk, "preserve", -1, &error));
  g_assert_no_error(error);

  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  g_assert_true(run_command(argv[1], directory, compose, "down", "--dry-run",
                            "--remove-orphans", &stdout_text, &stderr_text));
  g_assert_cmpstr(stdout_text, ==,
                  "Would stop service app\nWould stop service orphan-worker\n");
  g_assert_true(g_file_test(orphan_state, G_FILE_TEST_IS_REGULAR));
  g_free(stdout_text);
  g_free(stderr_text);

  stdout_text = NULL;
  stderr_text = NULL;
  g_assert_true(run_command(argv[1], directory, compose, "up", "--dry-run",
                            "--remove-orphans", &stdout_text, &stderr_text));
  const char *remove_plan = strstr(stdout_text, "Would stop service orphan-worker");
  const char *start_plan = strstr(stdout_text, "Would start service app");
  g_assert_nonnull(remove_plan);
  g_assert_nonnull(start_plan);
  g_assert_true(remove_plan < start_plan);
  g_assert_true(g_file_test(orphan_state, G_FILE_TEST_IS_REGULAR));
  g_free(stdout_text);
  g_free(stderr_text);

  stdout_text = NULL;
  stderr_text = NULL;
  g_assert_true(run_command(argv[1], directory, compose, "down",
                            "--remove-orphans", NULL, &stdout_text,
                            &stderr_text));
  g_assert_false(g_file_test(orphan_state, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(orphan_disk, G_FILE_TEST_IS_REGULAR));
  g_free(stdout_text);
  g_free(stderr_text);

  char *lock = g_build_filename(state_directory, ".lifecycle.lock", NULL);
  g_assert_cmpint(g_unlink(lock), ==, 0);
  g_assert_cmpint(g_unlink(compose), ==, 0);
  g_assert_cmpint(g_unlink(orphan_disk), ==, 0);
  g_assert_cmpint(g_rmdir(state_directory), ==, 0);
  char *quocker_directory = g_build_filename(directory, ".quocker", NULL);
  g_assert_cmpint(g_rmdir(quocker_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(quocker_directory);
  g_free(lock);
  g_free(orphan_state);
  g_free(orphan_disk);
  g_free(state_directory);
  g_free(compose);
  g_free(directory);
  return 0;
}
