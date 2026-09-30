/*
 * Verify Compose-style tail selection for saved VM serial logs.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static gchar *run_logs(const char *cli, const char *directory,
                       const char *compose, const char *tail_option,
                       const char *tail_value, gboolean expect_success) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"logs-tail-test",
                       (char *)"-f",
                       (char *)compose,
                       (char *)"logs",
                       (char *)tail_option,
                       (char *)tail_value,
                       (char *)"app",
                       NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  gboolean success = g_spawn_check_wait_status(status, &error);
  g_clear_error(&error);
  g_assert_cmpint(success, ==, expect_success);
  g_free(stderr_text);
  return output;
}

static void test_follow_format(const char *cli, const char *directory,
                               const char *compose, const char *log,
                               gboolean no_prefix) {
  GError *error = NULL;
  g_assert_true(g_file_set_contents(log, "", 0, &error));
  g_assert_no_error(error);
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"logs-tail-test",
                       (char *)"-f",
                       (char *)compose,
                       (char *)"logs",
                       (char *)"--follow",
                       no_prefix ? (char *)"--no-log-prefix" : NULL,
                       NULL};
  GPid child = 0;
  gint stdout_fd = -1;
  gint stderr_fd = -1;
  g_assert_true(g_spawn_async_with_pipes(
      NULL, arguments, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child,
      NULL, &stdout_fd, &stderr_fd, &error));
  g_assert_no_error(error);
  g_usleep(500000);
  g_assert_true(g_file_set_contents(log, "streamed\n", -1, &error));
  g_assert_no_error(error);
  g_usleep(500000);
  g_assert_cmpint(kill(child, SIGINT), ==, 0);
  int status = 0;
  g_assert_cmpint(waitpid(child, &status, 0), ==, child);
  g_spawn_close_pid(child);
  g_assert_true(WIFEXITED(status));
  g_assert_cmpint(WEXITSTATUS(status), ==, 0);

  GString *output = g_string_new(NULL);
  char buffer[256];
  ssize_t bytes;
  while ((bytes = read(stdout_fd, buffer, sizeof(buffer))) > 0) {
    g_string_append_len(output, buffer, bytes);
  }
  close(stdout_fd);
  close(stderr_fd);
  g_assert_cmpstr(output->str, ==,
                  no_prefix ? "streamed\n" : "app | streamed\n");
  g_string_free(output, TRUE);
}

int main(void) {
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-logs-tail-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(directory);
  char *compose = g_build_filename(directory, "compose.yaml", NULL);
  g_assert_true(g_file_set_contents(
      compose, "services:\n  app:\n    image: ./disk.qcow2\n", -1, &error));
  g_assert_no_error(error);
  char *state = g_build_filename(directory, ".quocker", "logs-tail-test", NULL);
  g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
  char *log = g_build_filename(state, "app.log", NULL);
  g_assert_true(g_file_set_contents(log, "one\ntwo\n\nthree\nfour\n", -1,
                                    &error));
  g_assert_no_error(error);

  gchar *output = run_logs(cli, directory, compose, "--tail", "2", TRUE);
  g_assert_cmpstr(output, ==, "app | three\napp | four\n");
  g_free(output);
  output = run_logs(cli, directory, compose, "-n", "all", TRUE);
  g_assert_cmpstr(output, ==,
                  "app | one\napp | two\napp | three\napp | four\n");
  g_free(output);
  output = run_logs(cli, directory, compose, "--no-log-prefix", NULL, TRUE);
  g_assert_cmpstr(output, ==, "one\ntwo\nthree\nfour\n");
  g_free(output);
  output = run_logs(cli, directory, compose, "--tail=0", NULL, TRUE);
  g_assert_cmpstr(output, ==, "");
  g_free(output);
  output = run_logs(cli, directory, compose, "--tail", "-1", FALSE);
  g_free(output);
  test_follow_format(cli, directory, compose, log, FALSE);
  test_follow_format(cli, directory, compose, log, TRUE);

  g_assert_cmpint(g_unlink(log), ==, 0);
  g_assert_cmpint(g_rmdir(state), ==, 0);
  char *project_state = g_build_filename(directory, ".quocker", NULL);
  g_assert_cmpint(g_rmdir(project_state), ==, 0);
  g_assert_cmpint(g_unlink(compose), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(project_state);
  g_free(log);
  g_free(state);
  g_free(compose);
  g_free(directory);
  return 0;
}
