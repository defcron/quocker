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
  g_assert_cmpint(g_unlink(first), ==, 0);
  g_assert_cmpint(g_rmdir(volume_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(volume_directory);
  g_free(second);
  g_free(first);
  g_free(directory);
  return 0;
}
