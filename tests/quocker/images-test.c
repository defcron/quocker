/*
 * Verify the VM-aware Compose images command.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 2);
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-images-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(directory);
  char *command[] = {(char *)cli,
                     (char *)"--project-directory",
                     directory,
                     (char *)"--project-name",
                     (char *)"images-test",
                     (char *)"--profile",
                     (char *)"test",
                     (char *)"-f",
                     argv[1],
                     (char *)"images",
                     NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  g_assert_true(g_spawn_sync(NULL, command, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(output, ==,
                  "SERVICE\tIMAGE\tVM_STATE\n"
                  "app\t./guest.qcow2\tnot-created\n");
  g_assert_cmpstr(stderr_text, ==, "");
  char *state = g_build_filename(directory, ".quocker", NULL);
  g_assert_false(g_file_test(state, G_FILE_TEST_EXISTS));
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(state);
  g_free(stderr_text);
  g_free(output);
  g_free(directory);
  return 0;
}
