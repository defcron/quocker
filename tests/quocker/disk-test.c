/*
 * Tests for materialized OCI rootfs to ext4 conversion.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-disk.h"

#include <ext2fs/ext2fs.h>
#include <glib/gstdio.h>
#include <sys/xattr.h>

static void set_metadata(const char *path, const char *name,
                         const char *value) {
  g_assert_cmpint(lsetxattr(path, name, value, strlen(value) + 1, 0), ==, 0);
}

static void assert_ext4_clean(const char *disk) {
  char *arguments[] = {(char *)"e2fsck", (char *)"-fn", (char *)disk, NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL,
                             NULL, &stdout_text, &stderr_text, &status,
                             &error));
  g_assert_no_error(error);
  if (!g_spawn_check_wait_status(status, &error)) {
    g_test_message("e2fsck stdout: %s", stdout_text ? stdout_text : "");
    g_test_message("e2fsck stderr: %s", stderr_text ? stderr_text : "");
    g_assert_no_error(error);
  }
  g_free(stdout_text);
  g_free(stderr_text);
}

static void test_ext4_conversion_replays_guest_metadata(void) {
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-disk-test-XXXXXX", &error);
  g_assert_no_error(error);
  char *rootfs = g_build_filename(directory, "rootfs", NULL);
  char *bin = g_build_filename(rootfs, "bin", NULL);
  char *app = g_build_filename(bin, "app", NULL);
  char *disk = g_build_filename(directory, "guest.ext4", NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(bin, 0700), ==, 0);
  g_assert_true(g_file_set_contents(app, "guest executable", -1, NULL));

  set_metadata(rootfs, "user.quocker.oci.uid", "1234");
  set_metadata(rootfs, "user.quocker.oci.gid", "2345");
  set_metadata(rootfs, "user.quocker.oci.mode", "493");
  set_metadata(bin, "user.quocker.oci.uid", "3000");
  set_metadata(bin, "user.quocker.oci.gid", "4000");
  set_metadata(bin, "user.quocker.oci.mode", "488");
  set_metadata(app, "user.quocker.oci.uid", "42");
  set_metadata(app, "user.quocker.oci.gid", "84");
  set_metadata(app, "user.quocker.oci.mode", "457");
  const char attribute_record[] = "user.example\0kept in guest image";
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)"user.example", 12);
  char *attribute = g_strdup_printf("user.quocker.oci.xattr.%s",
                                    g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  g_assert_cmpint(
      lsetxattr(app, attribute, attribute_record, sizeof(attribute_record), 0),
      ==, 0);

  guint64 estimated_size = 0;
  g_assert_true(quocker_rootfs_ext4_estimate(rootfs, &estimated_size, &error));
  g_assert_no_error(error);
  g_assert_true(quocker_rootfs_to_ext4(rootfs, disk, &error));
  g_assert_no_error(error);
  struct stat disk_stat;
  g_assert_cmpint(stat(disk, &disk_stat), ==, 0);
  g_assert_cmpuint((guint64)disk_stat.st_size, ==, estimated_size);
  ext2_filsys fs = NULL;
  g_assert_cmpint(ext2fs_open(disk, 0, 0, 0, unix_io_manager, &fs), ==, 0);
  ext2_ino_t inode_number = 0;
  struct ext2_inode inode;
  g_assert_cmpint(ext2fs_read_inode(fs, EXT2_ROOT_INO, &inode), ==, 0);
  g_assert_cmpuint(inode_uid(inode), ==, 1234);
  g_assert_cmpuint(inode_gid(inode), ==, 2345);
  g_assert_cmpuint(inode.i_mode & 07777, ==, 0755);
  g_assert_cmpint(
      ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/bin", &inode_number), ==,
      0);
  g_assert_cmpint(ext2fs_read_inode(fs, inode_number, &inode), ==, 0);
  g_assert_cmpuint(inode_uid(inode), ==, 3000);
  g_assert_cmpuint(inode_gid(inode), ==, 4000);
  g_assert_cmpuint(inode.i_mode & 07777, ==, 0750);
  g_assert_cmpint(
      ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/bin/app", &inode_number),
      ==, 0);
  g_assert_cmpint(ext2fs_read_inode(fs, inode_number, &inode), ==, 0);
  g_assert_cmpuint(inode_uid(inode), ==, 42);
  g_assert_cmpuint(inode_gid(inode), ==, 84);
  g_assert_cmpuint(inode.i_mode & 07777, ==, 0711);
  struct ext2_xattr_handle *xattrs = NULL;
  g_assert_cmpint(ext2fs_xattrs_open(fs, inode_number, &xattrs), ==, 0);
  g_assert_cmpint(ext2fs_xattrs_read(xattrs), ==, 0);
  void *xattr_value = NULL;
  size_t xattr_size = 0;
  g_assert_cmpint(
      ext2fs_xattr_get(xattrs, "user.example", &xattr_value, &xattr_size), ==,
      0);
  g_assert_cmpuint(xattr_size, ==, strlen("kept in guest image") + 1);
  g_assert_cmpmem(xattr_value, xattr_size, "kept in guest image",
                  strlen("kept in guest image") + 1);
  ext2fs_free_mem(&xattr_value);
  ext2fs_xattrs_close(&xattrs);
  g_assert_cmpint(ext2fs_close(fs), ==, 0);
  assert_ext4_clean(disk);

  g_assert_cmpint(g_unlink(disk), ==, 0);
  g_assert_cmpint(g_unlink(app), ==, 0);
  g_assert_cmpint(g_rmdir(bin), ==, 0);
  g_assert_cmpint(g_rmdir(rootfs), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(attribute);
  g_free(disk);
  g_free(app);
  g_free(bin);
  g_free(rootfs);
  g_free(directory);
}

static void test_special_nodes_are_materialized_only_in_guest_image(void) {
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-disk-special-XXXXXX", &error);
  g_assert_no_error(error);
  char *rootfs = g_build_filename(directory, "rootfs", NULL);
  char *placeholder = g_build_filename(rootfs, "device", NULL);
  char *fifo_placeholder = g_build_filename(rootfs, "pipe", NULL);
  char *disk = g_build_filename(directory, "guest.ext4", NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_true(g_file_set_contents(placeholder, "", 0, NULL));
  g_assert_true(g_file_set_contents(fifo_placeholder, "", 0, NULL));
  set_metadata(rootfs, "user.quocker.oci.uid", "0");
  set_metadata(rootfs, "user.quocker.oci.gid", "0");
  set_metadata(rootfs, "user.quocker.oci.mode", "493");
  set_metadata(placeholder, "user.quocker.oci.uid", "0");
  set_metadata(placeholder, "user.quocker.oci.gid", "0");
  set_metadata(placeholder, "user.quocker.oci.mode", "438");
  set_metadata(placeholder, "user.quocker.oci.type", "char");
  set_metadata(placeholder, "user.quocker.oci.devmajor", "1");
  set_metadata(placeholder, "user.quocker.oci.devminor", "3");
  set_metadata(fifo_placeholder, "user.quocker.oci.uid", "100");
  set_metadata(fifo_placeholder, "user.quocker.oci.gid", "200");
  set_metadata(fifo_placeholder, "user.quocker.oci.mode", "384");
  set_metadata(fifo_placeholder, "user.quocker.oci.type", "fifo");
  g_assert_true(quocker_rootfs_to_ext4(rootfs, disk, &error));
  g_assert_no_error(error);
  ext2_filsys fs = NULL;
  g_assert_cmpint(ext2fs_open(disk, 0, 0, 0, unix_io_manager, &fs), ==, 0);
  ext2_ino_t inode_number = 0;
  struct ext2_inode inode;
  g_assert_cmpint(
      ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/device", &inode_number),
      ==, 0);
  g_assert_cmpint(ext2fs_read_inode(fs, inode_number, &inode), ==, 0);
  g_assert_true(S_ISCHR(inode.i_mode));
  g_assert_cmpuint(inode.i_mode & 07777, ==, 0666);
  g_assert_cmpuint(inode_uid(inode), ==, 0);
  g_assert_cmpuint(inode_gid(inode), ==, 0);
  g_assert_cmphex(inode.i_block[0], ==, 0x103);
  g_assert_cmpint(
      ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/pipe", &inode_number),
      ==, 0);
  g_assert_cmpint(ext2fs_read_inode(fs, inode_number, &inode), ==, 0);
  g_assert_true(S_ISFIFO(inode.i_mode));
  g_assert_cmpuint(inode_uid(inode), ==, 100);
  g_assert_cmpuint(inode_gid(inode), ==, 200);
  g_assert_cmpint(ext2fs_close(fs), ==, 0);
  assert_ext4_clean(disk);

  struct stat st;
  g_assert_cmpint(lstat(placeholder, &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));
  g_assert_cmpint(lstat(fifo_placeholder, &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));

  g_assert_cmpint(g_unlink(disk), ==, 0);
  g_assert_cmpint(g_unlink(placeholder), ==, 0);
  g_assert_cmpint(g_unlink(fifo_placeholder), ==, 0);
  g_assert_cmpint(g_rmdir(rootfs), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(disk);
  g_free(fifo_placeholder);
  g_free(placeholder);
  g_free(rootfs);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/disk/ext4-metadata",
                  test_ext4_conversion_replays_guest_metadata);
  g_test_add_func("/quocker/disk/special-files",
                  test_special_nodes_are_materialized_only_in_guest_image);
  return g_test_run();
}
