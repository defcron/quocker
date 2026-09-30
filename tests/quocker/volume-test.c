#include "../../scripts/quocker-volume.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-volume-test-XXXXXX", &error);
  g_assert_no_error(error);
  char *first = NULL;
  g_assert_true(quocker_volume_disk_prepare(directory, "project:data",
                                            64 * 1024 * 1024, &first, &error));
  g_assert_no_error(error);
  struct stat st;
  g_assert_cmpint(g_lstat(first, &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));
  g_assert_cmpint(st.st_size, ==, 64 * 1024 * 1024);
  GPtrArray *volumes = NULL;
  g_assert_true(quocker_volume_list(directory, &volumes, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(volumes->len, ==, 1);
  QuockerVolumeInfo *info = g_ptr_array_index(volumes, 0);
  g_assert_cmpstr(info->logical_name, ==, "project:data");
  g_assert_cmpuint(info->size_bytes, ==, 64 * 1024 * 1024);
  g_ptr_array_free(volumes, TRUE);
  char *second = NULL;
  g_assert_true(quocker_volume_disk_prepare(
      directory, "project:data", 128 * 1024 * 1024, &second, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(first, ==, second);
  g_assert_cmpint(g_lstat(second, &st), ==, 0);
  g_assert_cmpint(st.st_size, ==, 64 * 1024 * 1024);

  char *invalid = NULL;
  g_assert_false(
      quocker_volume_disk_prepare(directory, "bad", 1024, &invalid, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_clear_error(&error);

  char *volume_directory = g_build_filename(directory, "volumes", NULL);
  char *metadata_path = g_strconcat(first, ".name", NULL);
  g_assert_cmpint(g_unlink(metadata_path), ==, 0);
  g_assert_cmpint(g_unlink(first), ==, 0);
  g_assert_cmpint(g_rmdir(volume_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(volume_directory);
  g_free(second);
  g_free(metadata_path);
  g_free(first);
  g_free(directory);

  const char *cli = g_getenv("QUOCKER_CLI");
  if (cli && *cli) {
    char *project = g_dir_make_tmp("quocker-volume-cli-XXXXXX", &error);
    g_assert_no_error(error);
    char *compose = g_build_filename(project, "compose.yaml", NULL);
    g_assert_true(g_file_set_contents(
        compose, "services:\n  app:\n    image: ./ignored.qcow2\n", -1,
        &error));
    g_assert_no_error(error);
    char *quocker_state =
        g_build_filename(project, ".quocker", "project", NULL);
    g_assert_cmpint(g_mkdir_with_parents(quocker_state, 0700), ==, 0);
    char *disk = NULL;
    g_assert_true(quocker_volume_disk_prepare(quocker_state, "project:data",
                                              64 * 1024 * 1024, &disk, &error));
    g_assert_no_error(error);
    char *arguments[] = {(char *)cli,       (char *)"--project-directory",
                         project,           (char *)"--project-name",
                         (char *)"project", (char *)"-f",
                         compose,           (char *)"volume",
                         (char *)"ls",      NULL};
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint status = 0;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL,
                               NULL, &stdout_text, &stderr_text, &status,
                               &error));
    g_assert_no_error(error);
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(stdout_text, "project:data"));
    g_assert_nonnull(strstr(stdout_text, "64"));
    g_free(stdout_text);
    g_free(stderr_text);
    char *metadata = g_strconcat(disk, ".name", NULL);
    char *volume_dir = g_path_get_dirname(disk);
    g_assert_cmpint(g_unlink(metadata), ==, 0);
    g_assert_cmpint(g_unlink(disk), ==, 0);
    g_assert_cmpint(g_rmdir(volume_dir), ==, 0);
    g_assert_cmpint(g_rmdir(quocker_state), ==, 0);
    char *metadata_root = g_build_filename(project, ".quocker", NULL);
    g_assert_cmpint(g_rmdir(metadata_root), ==, 0);
    g_assert_cmpint(g_unlink(compose), ==, 0);
    g_assert_cmpint(g_rmdir(project), ==, 0);
    g_free(metadata_root);
    g_free(volume_dir);
    g_free(metadata);
    g_free(disk);
    g_free(quocker_state);
    g_free(compose);
    g_free(project);
  }
  return 0;
}
