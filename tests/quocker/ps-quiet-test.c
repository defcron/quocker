/*
 * Verify quiet Compose-compatible output from the VM-aware ps command.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

static void assert_quiet_ps(const char *cli, const char *compose,
                            const char *option) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)g_path_get_dirname(compose),
                       (char *)"--project-name",
                       (char *)"ps-quiet-test",
                       (char *)"--profile",
                       (char *)"test",
                       (char *)"-f",
                       (char *)compose,
                       (char *)"ps",
                       (char *)option,
                       NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &stdout_text, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(stdout_text, ==, "ps-quiet-test-app\n");
  g_assert_cmpstr(stderr_text, ==, "");
  g_free(stdout_text);
  g_free(stderr_text);
  g_free(arguments[2]);
}

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  assert_quiet_ps(cli, argv[1], "-q");
  assert_quiet_ps(cli, argv[1], "--quiet");
  return 0;
}
