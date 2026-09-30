/*
 * Verify atomic YAML and JSON config output.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <string.h>

static gboolean run_config(const char *cli, const char *compose,
                           const char *format, const char *output_path,
                           guint output_syntax, gboolean expect_success,
                           gchar **stdout_text) {
  char *format_option = g_strdup_printf("--format=%s", format);
  char *output_option = g_strdup_printf("--output=%s", output_path);
  char *separate_arguments[] = {(char *)cli,     (char *)"-f",
                                (char *)compose, (char *)"config",
                                format_option,   (char *)"--output",
                                (char *)output_path, NULL};
  char *long_arguments[] = {(char *)cli,     (char *)"-f",
                            (char *)compose, (char *)"config",
                            format_option,   output_option,
                            NULL};
  char *short_arguments[] = {(char *)cli,     (char *)"-f",
                             (char *)compose, (char *)"config",
                             format_option,   (char *)"-o",
                             (char *)output_path, NULL};
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  char **arguments = output_syntax == 1 ? short_arguments
                     : output_syntax == 2 ? separate_arguments
                                          : long_arguments;
  g_assert_true(g_spawn_sync(NULL, arguments,
                             NULL, G_SPAWN_DEFAULT, NULL, NULL, stdout_text,
                             &stderr_text, &status, &error));
  g_assert_no_error(error);
  gboolean succeeded = g_spawn_check_wait_status(status, &error);
  g_clear_error(&error);
  g_free(format_option);
  g_free(output_option);
  g_free(stderr_text);
  return succeeded == expect_success;
}

int main(void) {
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-config-output-XXXXXX", &error);
  g_assert_no_error(error);
  char *compose = g_build_filename(directory, "compose.yaml", NULL);
  char *output_path = g_build_filename(directory, "resolved.yaml", NULL);
  g_assert_true(g_file_set_contents(
      compose, "services:\n  app:\n    image: alpine\n", -1, &error));
  g_assert_no_error(error);

  gchar *stdout_text = NULL;
  g_assert_true(run_config(cli, compose, "yaml", output_path, TRUE, TRUE,
                           &stdout_text));
  g_assert_cmpstr(stdout_text, ==, "");
  g_free(stdout_text);
  gchar *contents = NULL;
  g_assert_true(g_file_get_contents(output_path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(contents, "\"services\""));
  g_assert_nonnull(strstr(contents, "\"image\": \"alpine\""));
  g_free(contents);
  struct stat output_stat;
  g_assert_cmpint(g_lstat(output_path, &output_stat), ==, 0);
  g_assert_cmpuint(output_stat.st_mode & 0777, ==, 0600);

  g_assert_true(run_config(cli, compose, "json", output_path, FALSE, TRUE,
                           &stdout_text));
  g_assert_cmpstr(stdout_text, ==, "");
  g_free(stdout_text);
  JsonParser *parser = json_parser_new();
  g_assert_true(json_parser_load_from_file(parser, output_path, &error));
  g_assert_no_error(error);
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonObject *services = json_object_get_object_member(root, "services");
  JsonObject *app = json_object_get_object_member(services, "app");
  g_assert_cmpstr(json_object_get_string_member(app, "image"), ==, "alpine");
  g_object_unref(parser);

  char *target_path = g_build_filename(directory, "target.txt", NULL);
  g_assert_true(g_file_set_contents(target_path, "keep", -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_unlink(output_path), ==, 0);
  g_assert_cmpint(symlink(target_path, output_path), ==, 0);
  g_assert_true(run_config(cli, compose, "yaml", output_path, 2, TRUE,
                           &stdout_text));
  g_assert_cmpstr(stdout_text, ==, "");
  g_free(stdout_text);
  g_assert_cmpint(g_lstat(output_path, &output_stat), ==, 0);
  g_assert_true(S_ISREG(output_stat.st_mode));
  g_assert_true(g_file_get_contents(target_path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(contents, ==, "keep");
  g_free(contents);

  char *missing_directory = g_build_filename(directory, "missing", "out.yaml",
                                             NULL);
  g_assert_true(run_config(cli, compose, "yaml", missing_directory, FALSE,
                           FALSE,
                           &stdout_text));
  g_assert_cmpstr(stdout_text, ==, "");
  g_free(stdout_text);
  g_assert_false(g_file_test(missing_directory, G_FILE_TEST_EXISTS));

  g_assert_cmpint(g_unlink(output_path), ==, 0);
  g_assert_cmpint(g_unlink(target_path), ==, 0);
  g_assert_cmpint(g_unlink(compose), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(missing_directory);
  g_free(target_path);
  g_free(output_path);
  g_free(compose);
  g_free(directory);
  return 0;
}
