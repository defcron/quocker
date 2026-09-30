/*
 * Tests for Quocker guest configuration and initramfs assembly.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-initrd.h"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static QuockerRuntimeConfig *new_runtime(void) {
  QuockerRuntimeConfig *runtime = g_new0(QuockerRuntimeConfig, 1);
  runtime->argv = g_ptr_array_new_with_free_func(g_free);
  runtime->environment =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_ptr_array_add(runtime->argv, g_strdup("/usr/bin/service"));
  g_ptr_array_add(runtime->argv, g_strdup("argument with spaces"));
  g_hash_table_insert(runtime->environment, g_strdup("Z_LAST"),
                      g_strdup("last"));
  g_hash_table_insert(runtime->environment, g_strdup("A_FIRST"),
                      g_strdup("first=value"));
  runtime->working_directory = g_strdup("/srv/service");
  runtime->user = g_strdup("1000:100");
  return runtime;
}

static void free_runtime(QuockerRuntimeConfig *runtime) {
  g_ptr_array_free(runtime->argv, TRUE);
  g_hash_table_destroy(runtime->environment);
  g_free(runtime->working_directory);
  g_free(runtime->user);
  g_free(runtime);
}

static const char *native_oci_architecture(void) {
  struct utsname system;
  g_assert_cmpint(uname(&system), ==, 0);
  if (g_str_equal(system.machine, "x86_64")) {
    return "amd64";
  }
  if (g_str_equal(system.machine, "aarch64")) {
    return "arm64";
  }
  g_error("initrd test does not define OCI alias for host %s", system.machine);
  return NULL;
}

static void test_runtime_config_round_trip(void) {
  GError *error = NULL;
  QuockerRuntimeConfig *runtime = new_runtime();
  GByteArray *encoded = NULL;
  g_assert_true(quocker_guest_config_encode(runtime, &encoded, &error));
  g_assert_no_error(error);
  g_assert_cmpmem(encoded->data, 4, "QCFG", 4);
  g_assert_cmpuint(encoded->data[7], ==, 1);
  g_assert_cmpuint(encoded->data[11], ==, 2);
  g_assert_cmpuint(encoded->data[15], ==, 2);

  GError *spawn_error = NULL;
  char *directory = g_dir_make_tmp("quocker-init-config-XXXXXX", &error);
  g_assert_no_error(error);
  char *path = g_build_filename(directory, "config", NULL);
  g_assert_true(g_file_set_contents(path, (const char *)encoded->data,
                                    encoded->len, &error));
  g_assert_no_error(error);
  const char *guest_init = g_getenv("QUOCKER_GUEST_INIT");
  g_assert_nonnull(guest_init);
  char *arguments[] = {(char *)guest_init, (char *)"--validate-config", path,
                       NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL,
                             NULL, &stdout_text, &stderr_text, &status,
                             &spawn_error));
  g_assert_no_error(spawn_error);
  g_assert_true(g_spawn_check_wait_status(status, &spawn_error));
  g_assert_no_error(spawn_error);
  g_assert_nonnull(strstr(stdout_text, "2 argv, 2 environment values"));
  g_free(stdout_text);
  g_free(stderr_text);
  stdout_text = NULL;
  stderr_text = NULL;

  encoded->data[7] = 2;
  g_assert_true(g_file_set_contents(path, (const char *)encoded->data,
                                    encoded->len, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL,
                             NULL, &stdout_text, &stderr_text, &status,
                             &spawn_error));
  g_assert_no_error(spawn_error);
  g_assert_false(g_spawn_check_wait_status(status, &spawn_error));
  g_clear_error(&spawn_error);

  g_free(stdout_text);
  g_free(stderr_text);
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(path);
  g_free(directory);
  g_byte_array_unref(encoded);
  free_runtime(runtime);
}

static void test_initrd_appends_guest_archive(void) {
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-initrd-test-XXXXXX", &error);
  g_assert_no_error(error);
  char *base_path = g_build_filename(directory, "base.initrd", NULL);
  char *output_path = g_build_filename(directory, "guest.initrd", NULL);
  g_assert_true(g_file_set_contents(base_path, "base-initrd", 11, &error));
  g_assert_no_error(error);
  char *base_digest =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, "base-initrd", 11);
  char *expected_digest = g_strdup_printf("sha256:%s", base_digest);
  g_free(base_digest);
  const char *guest_init = g_getenv("QUOCKER_GUEST_INIT");
  g_assert_nonnull(guest_init);
  const char *architecture = native_oci_architecture();
  QuockerRuntimeConfig *runtime = new_runtime();
  GByteArray *expected_config = NULL;
  g_assert_true(quocker_guest_config_encode(runtime, &expected_config, &error));
  g_assert_no_error(error);
  g_assert_false(quocker_initrd_build(
      base_path, expected_digest, guest_init,
      g_str_equal(architecture, "amd64") ? "arm64" : "amd64", runtime,
      output_path, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-initrd-error"), 1);
  g_clear_error(&error);
  g_assert_false(g_file_test(output_path, G_FILE_TEST_EXISTS));
  g_assert_false(quocker_initrd_build(
      base_path,
      "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      guest_init, architecture, runtime, output_path, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-initrd-error"), 1);
  g_clear_error(&error);
  g_assert_false(g_file_test(output_path, G_FILE_TEST_EXISTS));
  g_assert_true(quocker_initrd_build(base_path, expected_digest, guest_init,
                                     architecture, runtime, output_path,
                                     &error));
  g_assert_no_error(error);

  int fd = g_open(output_path, O_RDONLY | O_CLOEXEC, 0);
  g_assert_cmpint(fd, >=, 0);
  g_assert_cmpint(lseek(fd, 11, SEEK_SET), ==, 11);
  struct archive *archive = archive_read_new();
  archive_read_support_format_cpio(archive);
  g_assert_cmpint(archive_read_open_fd(archive, fd, 64 * 1024), ==, ARCHIVE_OK);
  gboolean saw_init = FALSE;
  gboolean saw_config = FALSE;
  struct archive_entry *entry;
  while (archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
    const char *name = archive_entry_pathname(entry);
    if (g_strcmp0(name, "init") == 0) {
      saw_init = TRUE;
      g_assert_cmpint(archive_entry_perm(entry), ==, 0755);
      archive_read_data_skip(archive);
    } else if (g_strcmp0(name, "quocker/config") == 0) {
      saw_config = TRUE;
      g_assert_cmpuint(archive_entry_size(entry), ==, expected_config->len);
      guint8 *data = g_malloc(expected_config->len);
      la_ssize_t read_size =
          archive_read_data(archive, data, expected_config->len);
      g_assert_cmpint(read_size, ==, expected_config->len);
      g_assert_cmpmem(data, expected_config->len, expected_config->data,
                      expected_config->len);
      g_free(data);
    } else {
      archive_read_data_skip(archive);
    }
  }
  g_assert_true(saw_init);
  g_assert_true(saw_config);
  archive_read_free(archive);
  close(fd);

  g_byte_array_unref(expected_config);
  free_runtime(runtime);
  g_free(expected_digest);
  g_assert_cmpint(g_unlink(output_path), ==, 0);
  g_assert_cmpint(g_unlink(base_path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(output_path);
  g_free(base_path);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/initrd/config", test_runtime_config_round_trip);
  g_test_add_func("/quocker/initrd/archive", test_initrd_appends_guest_archive);
  return g_test_run();
}
