/*
 * Tests for Quocker Compose environment-file parsing.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-env.h"
#include <glib/gstdio.h>
#include <unistd.h>

static char *test_interpolate(const char *text, GHashTable *environment) {
  GString *result = g_string_new(NULL);
  for (const char *p = text; *p;) {
    if (p[0] == '$' && p[1] == '{') {
      const char *end = strchr(p + 2, '}');
      if (!end) {
        g_string_free(result, TRUE);
        return NULL;
      }
      char *name = g_strndup(p + 2, end - p - 2);
      const char *value = g_hash_table_lookup(environment, name);
      g_free(name);
      if (!value) {
        g_string_free(result, TRUE);
        return NULL;
      }
      g_string_append(result, value);
      p = end + 1;
    } else {
      g_string_append_c(result, *p++);
    }
  }
  return g_string_free(result, FALSE);
}

static char *write_env_file(const char *contents) {
  char *path = NULL;
  int fd = g_file_open_tmp("quocker-env-XXXXXX", &path, NULL);
  g_assert_cmpint(fd, >=, 0);
  close(fd);
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
  return path;
}

static GHashTable *new_environment(void) {
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void test_default_format(void) {
  char *path = write_env_file(
      "BASE=world\n"
      "PLAIN=hello # comment\n"
      "DOUBLE=\"${BASE}\\nthere\"\n"
      "SINGLE='${BASE}'\n"
      "UNSET\n");
  GHashTable *values = new_environment();
  GHashTable *lookup = new_environment();
  g_hash_table_insert(lookup, g_strdup("BASE"), g_strdup("outer"));
  GError *error = NULL;
  g_assert_true(quocker_env_file_read(values, path, FALSE, lookup, FALSE,
                                      TRUE, FALSE, test_interpolate, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(g_hash_table_lookup(values, "BASE"), ==, "world");
  g_assert_cmpstr(g_hash_table_lookup(values, "PLAIN"), ==, "hello");
  g_assert_cmpstr(g_hash_table_lookup(values, "DOUBLE"), ==, "world\nthere");
  g_assert_cmpstr(g_hash_table_lookup(values, "SINGLE"), ==, "${BASE}");
  g_assert_true(g_hash_table_contains(values, "UNSET"));
  g_assert_null(g_hash_table_lookup(values, "UNSET"));
  g_assert_false(g_hash_table_contains(lookup, "UNSET"));
  g_hash_table_destroy(values);
  g_hash_table_destroy(lookup);
  g_remove(path);
  g_free(path);
}

static void test_raw_and_host_precedence(void) {
  char *path = write_env_file("QUOCKER_TEST_HOST=file\nRAW=\"$LITERAL\"\n");
  g_setenv("QUOCKER_TEST_HOST", "host", TRUE);
  GHashTable *values = new_environment();
  GHashTable *lookup = new_environment();
  GError *error = NULL;
  g_assert_true(quocker_env_file_read(values, path, FALSE, lookup, TRUE, TRUE,
                                      FALSE, test_interpolate, &error));
  g_assert_no_error(error);
  g_assert_false(g_hash_table_contains(values, "QUOCKER_TEST_HOST"));
  g_assert_cmpstr(g_hash_table_lookup(values, "RAW"), ==, "$LITERAL");
  g_hash_table_remove_all(values);
  g_assert_true(quocker_env_file_read(values, path, FALSE, lookup, FALSE,
                                      FALSE, TRUE, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(g_hash_table_lookup(values, "RAW"), ==, "\"$LITERAL\"");
  g_hash_table_destroy(values);
  g_hash_table_destroy(lookup);
  g_unsetenv("QUOCKER_TEST_HOST");
  g_remove(path);
  g_free(path);
}

static void test_optional_missing_and_invalid(void) {
  GHashTable *values = new_environment();
  GHashTable *lookup = new_environment();
  GError *error = NULL;
  g_assert_true(quocker_env_file_read(values, "/no/such/quocker-env-file",
                                      TRUE, lookup, FALSE, FALSE, FALSE, NULL,
                                      &error));
  g_assert_no_error(error);
  char *path = write_env_file("BAD-NAME=value\n");
  g_assert_false(quocker_env_file_read(values, path, FALSE, lookup, FALSE,
                                       FALSE, FALSE, NULL, &error));
  g_assert_error(error, quocker_env_error_quark(), 1);
  g_clear_error(&error);
  g_hash_table_destroy(values);
  g_hash_table_destroy(lookup);
  g_remove(path);
  g_free(path);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/env/default-format", test_default_format);
  g_test_add_func("/quocker/env/raw-host-precedence",
                  test_raw_and_host_precedence);
  g_test_add_func("/quocker/env/optional-invalid",
                  test_optional_missing_and_invalid);
  return g_test_run();
}
