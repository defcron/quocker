/*
 * OCI root filesystem to raw ext4 guest disk conversion.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-disk.h"

#include <dirent.h>
#include <errno.h>
#include <ext2fs/ext2fs.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#define DISK_MAX_BYTES (64ULL * 1024 * 1024 * 1024)
#define DISK_MAX_DEPTH 256
#define DISK_MAX_XATTR_BYTES (32 * 1024)

static GQuark disk_error_quark(void) {
  return g_quark_from_static_string("quocker-disk-error");
}

static void disk_error(GError **error, const char *message) {
  if (error && !*error) {
    g_set_error_literal(error, disk_error_quark(), 1, message);
  }
}

static gboolean parse_u32_attribute(const char *name, const void *value,
                                    size_t size, guint32 *number_out) {
  if (!value || !size || size > 32 || ((const char *)value)[size - 1] != '\0') {
    return FALSE;
  }
  errno = 0;
  char *end = NULL;
  guint64 number = g_ascii_strtoull(value, &end, 10);
  if (errno || !end || *end || number > G_MAXUINT32) {
    return FALSE;
  }
  *number_out = (guint32)number;
  (void)name;
  return TRUE;
}

static gboolean tree_size_at(int directory_fd, guint depth, guint64 *size_out) {
  if (depth > DISK_MAX_DEPTH) {
    return FALSE;
  }
  int scan_fd = fcntl(directory_fd, F_DUPFD_CLOEXEC, 3);
  if (scan_fd < 0) {
    return FALSE;
  }
  DIR *directory = fdopendir(scan_fd);
  if (!directory) {
    close(scan_fd);
    return FALSE;
  }
  gboolean ok = TRUE;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) {
      continue;
    }
    struct stat st;
    if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) <
        0) {
      ok = FALSE;
      break;
    }
    if (S_ISREG(st.st_mode)) {
      if (st.st_size < 0 || (guint64)st.st_size > DISK_MAX_BYTES - *size_out) {
        ok = FALSE;
        break;
      }
      *size_out += (guint64)st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
      int child_fd = openat(dirfd(directory), entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child_fd < 0 || !tree_size_at(child_fd, depth + 1, size_out)) {
        if (child_fd >= 0) {
          close(child_fd);
        }
        ok = FALSE;
        break;
      }
      close(child_fd);
    }
  }
  closedir(directory);
  return ok;
}

gboolean quocker_rootfs_ext4_estimate(const char *rootfs_path,
                                     guint64 *size_out, GError **error) {
  if (size_out) {
    *size_out = 0;
  }
  if (!rootfs_path || !size_out) {
    disk_error(error, "root filesystem and size result are required");
    return FALSE;
  }
  int root_fd =
      open(rootfs_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root_fd < 0) {
    disk_error(error, "could not open staged root filesystem safely");
    return FALSE;
  }
  guint64 payload = 0;
  gboolean ok = tree_size_at(root_fd, 0, &payload);
  close(root_fd);
  if (!ok || payload > DISK_MAX_BYTES - 256 * 1024 * 1024) {
    disk_error(error, "root filesystem size is unsafe or exceeds 64 GiB");
    return FALSE;
  }
  guint64 size =
      MAX(256 * 1024 * 1024, payload + payload / 4 + 128 * 1024 * 1024);
  size = (size + 4 * 1024 * 1024 - 1) & ~((guint64)4 * 1024 * 1024 - 1);
  if (size > DISK_MAX_BYTES) {
    disk_error(error, "estimated ext4 image exceeds the 64 GiB limit");
    return FALSE;
  }
  *size_out = size;
  return TRUE;
}

static gboolean read_host_xattrs(int parent_fd, const char *name,
                                 gchar **names_out, gsize *names_size_out) {
  char *path = g_strdup_printf("/proc/self/fd/%d/%s", parent_fd, name);
  ssize_t size = llistxattr(path, NULL, 0);
  if (size < 0 || size > DISK_MAX_XATTR_BYTES) {
    g_free(path);
    return FALSE;
  }
  gchar *names = g_malloc(size ? (gsize)size : 1);
  ssize_t actual = size ? llistxattr(path, names, size) : 0;
  g_free(path);
  if (actual != size) {
    g_free(names);
    return FALSE;
  }
  *names_out = names;
  *names_size_out = (gsize)size;
  return TRUE;
}

static gboolean copy_oci_xattrs(ext2_filsys fs, ext2_ino_t inode_number,
                                int parent_fd, const char *name,
                                gchar *attribute_names, gsize names_size) {
  struct ext2_xattr_handle *handle = NULL;
  if (ext2fs_xattrs_open(fs, inode_number, &handle) ||
      ext2fs_xattrs_read(handle)) {
    if (handle) {
      ext2fs_xattrs_close(&handle);
    }
    return FALSE;
  }
  static const char *const metadata_names[] = {"user.quocker.oci.uid",
                                               "user.quocker.oci.gid",
                                               "user.quocker.oci.mode",
                                               "user.quocker.oci.type",
                                               "user.quocker.oci.devmajor",
                                               "user.quocker.oci.devminor",
                                               NULL};
  gboolean ok = TRUE;
  for (guint i = 0; metadata_names[i] && ok; i++) {
    void *existing = NULL;
    size_t existing_size = 0;
    if (ext2fs_xattr_get(handle, metadata_names[i], &existing,
                         &existing_size) == 0) {
      ok = ext2fs_xattr_remove(handle, metadata_names[i]) == 0;
      ext2fs_free_mem(&existing);
    }
  }
  char *path = g_strdup_printf("/proc/self/fd/%d/%s", parent_fd, name);
  for (gsize offset = 0; offset < names_size && ok;) {
    const char *attribute = attribute_names + offset;
    gsize attribute_size = strlen(attribute) + 1;
    offset += attribute_size;
    if (!g_str_has_prefix(attribute, "user.quocker.oci.xattr.")) {
      continue;
    }
    void *existing = NULL;
    size_t existing_size = 0;
    if (ext2fs_xattr_get(handle, attribute, &existing, &existing_size) == 0) {
      ok = ext2fs_xattr_remove(handle, attribute) == 0;
      ext2fs_free_mem(&existing);
      if (!ok) {
        break;
      }
    }
    ssize_t value_size = lgetxattr(path, attribute, NULL, 0);
    if (value_size < 0 || value_size > DISK_MAX_XATTR_BYTES) {
      ok = FALSE;
      break;
    }
    guchar *record = g_malloc((gsize)value_size);
    if (lgetxattr(path, attribute, record, value_size) != value_size) {
      g_free(record);
      ok = FALSE;
      break;
    }
    gsize original_name_size = strnlen((char *)record, (gsize)value_size);
    if (original_name_size == (gsize)value_size || !original_name_size ||
        strchr((char *)record, '/')) {
      g_free(record);
      ok = FALSE;
      break;
    }
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, record, original_name_size);
    const char *stored_hash = attribute + strlen("user.quocker.oci.xattr.");
    gboolean hash_matches =
        g_str_equal(stored_hash, g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    if (!hash_matches) {
      g_free(record);
      ok = FALSE;
      break;
    }
    ok = ext2fs_xattr_set(handle, (char *)record,
                          record + original_name_size + 1,
                          (gsize)value_size - original_name_size - 1) == 0;
    g_free(record);
  }
  g_free(path);
  if (ok && names_size) {
    ok = ext2fs_xattrs_write(handle) == 0;
  }
  ext2fs_xattrs_close(&handle);
  return ok;
}

typedef struct DirentTypeUpdate {
  ext2_ino_t inode;
  int file_type;
  gboolean updated;
} DirentTypeUpdate;

static int update_dirent_type(ext2_ino_t directory, int entry,
                              struct ext2_dir_entry *dirent, int offset,
                              int blocksize, char *buffer, void *opaque) {
  DirentTypeUpdate *update = opaque;
  (void)directory;
  (void)entry;
  (void)offset;
  (void)blocksize;
  (void)buffer;
  if (dirent->inode != update->inode) {
    return 0;
  }
  ext2fs_dirent_set_file_type(dirent, update->file_type);
  update->updated = TRUE;
  return DIRENT_CHANGED;
}

static gboolean set_directory_entry_type(ext2_filsys fs,
                                         ext2_ino_t parent_inode,
                                         ext2_ino_t inode_number,
                                         int file_type) {
  DirentTypeUpdate update = {.inode = inode_number,
                             .file_type = file_type};
  return ext2fs_dir_iterate2(fs, parent_inode, 0, NULL, update_dirent_type,
                             &update) == 0 &&
         update.updated;
}

static gboolean apply_inode_metadata(ext2_filsys fs, ext2_ino_t parent_inode,
                                     int parent_fd, const char *name,
                                     const struct stat *st) {
  ext2_ino_t inode_number;
  if (ext2fs_lookup(fs, parent_inode, name, strlen(name), NULL,
                    &inode_number)) {
    return FALSE;
  }
  struct ext2_inode inode;
  if (ext2fs_read_inode(fs, inode_number, &inode)) {
    return FALSE;
  }
  gchar *attribute_names = NULL;
  gsize names_size = 0;
  if (!read_host_xattrs(parent_fd, name, &attribute_names, &names_size)) {
    return FALSE;
  }
  char *path = g_strdup_printf("/proc/self/fd/%d/%s", parent_fd, name);
  guint32 uid = (guint32)st->st_uid;
  guint32 gid = (guint32)st->st_gid;
  guint32 mode = (guint32)(st->st_mode & 07777);
  const char *metadata_keys[] = {"user.quocker.oci.uid", "user.quocker.oci.gid",
                                 "user.quocker.oci.mode"};
  guint32 *destinations[] = {&uid, &gid, &mode};
  gboolean ok = TRUE;
  int directory_file_type = EXT2_FT_UNKNOWN;
  for (guint i = 0; i < G_N_ELEMENTS(metadata_keys) && ok; i++) {
    ssize_t size = lgetxattr(path, metadata_keys[i], NULL, 0);
    if (size < 0 || size > 32) {
      ok = FALSE;
      break;
    }
    char value[32];
    if (lgetxattr(path, metadata_keys[i], value, size) != size ||
        !parse_u32_attribute(metadata_keys[i], value, (gsize)size,
                             destinations[i])) {
      ok = FALSE;
    }
  }
  mode_t guest_type = inode.i_mode & S_IFMT;
  guint32 device_major = 0;
  guint32 device_minor = 0;
  const char *type_key = "user.quocker.oci.type";
  ssize_t type_size = lgetxattr(path, type_key, NULL, 0);
  if (type_size >= 0) {
    char type[16];
    if (type_size == 0 || type_size >= sizeof(type) ||
        lgetxattr(path, type_key, type, type_size) != type_size ||
        type[type_size - 1] != '\0' || !S_ISREG(st->st_mode)) {
      ok = FALSE;
    } else if (g_str_equal(type, "char")) {
      guest_type = S_IFCHR;
      directory_file_type = EXT2_FT_CHRDEV;
    } else if (g_str_equal(type, "block")) {
      guest_type = S_IFBLK;
      directory_file_type = EXT2_FT_BLKDEV;
    } else if (g_str_equal(type, "fifo")) {
      guest_type = S_IFIFO;
      directory_file_type = EXT2_FT_FIFO;
    } else {
      ok = FALSE;
    }
    if (ok && (guest_type == S_IFCHR || guest_type == S_IFBLK)) {
      const char *device_keys[] = {"user.quocker.oci.devmajor",
                                   "user.quocker.oci.devminor"};
      guint32 *device_values[] = {&device_major, &device_minor};
      for (guint i = 0; i < G_N_ELEMENTS(device_keys) && ok; i++) {
        ssize_t value_size = lgetxattr(path, device_keys[i], NULL, 0);
        char value[32];
        ok = value_size > 0 && value_size <= sizeof(value) &&
             lgetxattr(path, device_keys[i], value, value_size) == value_size &&
             parse_u32_attribute(device_keys[i], value, value_size,
                                 device_values[i]);
      }
      ok = ok && device_major <= 0xfff && device_minor <= 0xfffff;
    }
  } else if (errno != ENODATA) {
    ok = FALSE;
  }
  g_free(path);
  if (ok) {
    inode.i_mode = guest_type | (mode & 07777);
    inode.i_uid = uid & 0xffff;
    inode.i_gid = gid & 0xffff;
    ext2fs_set_i_uid_high(inode, uid >> 16);
    ext2fs_set_i_gid_high(inode, gid >> 16);
    if (guest_type == S_IFCHR || guest_type == S_IFBLK ||
        guest_type == S_IFIFO) {
      memset(inode.i_block, 0, sizeof(inode.i_block));
      inode.i_size = 0;
      inode.i_blocks = 0;
      inode.i_flags &= ~EXT4_EXTENTS_FL;
    }
    if (guest_type == S_IFCHR || guest_type == S_IFBLK) {
      guint32 encoded_minor =
          (device_minor & 0xff) | ((device_minor & ~0xff) << 12);
      inode.i_block[0] = (device_major << 8) | encoded_minor;
      inode.i_block[1] = 0;
    }
    ok = ext2fs_write_inode(fs, inode_number, &inode) == 0 &&
         copy_oci_xattrs(fs, inode_number, parent_fd, name, attribute_names,
                         names_size);
    if (ok && directory_file_type != EXT2_FT_UNKNOWN) {
      ok = ext2fs_read_inode(fs, inode_number, &inode) == 0;
      if (ok && inode.i_file_acl) {
        inode.i_blocks = fs->blocksize / 512;
      }
      ok = ok && ext2fs_write_inode(fs, inode_number, &inode) == 0 &&
           set_directory_entry_type(fs, parent_inode, inode_number,
                                    directory_file_type);
    }
  }
  g_free(attribute_names);
  return ok;
}

static gboolean restore_tree(ext2_filsys fs, ext2_ino_t parent_inode,
                             int directory_fd, guint depth) {
  if (depth > DISK_MAX_DEPTH) {
    return FALSE;
  }
  int scan_fd = fcntl(directory_fd, F_DUPFD_CLOEXEC, 3);
  if (scan_fd < 0) {
    return FALSE;
  }
  DIR *directory = fdopendir(scan_fd);
  if (!directory) {
    close(scan_fd);
    return FALSE;
  }
  struct stat directory_stat;
  gboolean ok = fstat(directory_fd, &directory_stat) == 0 &&
                apply_inode_metadata(fs, parent_inode, directory_fd, ".",
                                     &directory_stat);
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) {
      continue;
    }
    struct stat st;
    if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) <
            0 ||
        !apply_inode_metadata(fs, parent_inode, dirfd(directory), entry->d_name,
                              &st)) {
      ok = FALSE;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      ext2_ino_t child_inode;
      int child_fd = openat(dirfd(directory), entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child_fd < 0 ||
          ext2fs_lookup(fs, parent_inode, entry->d_name, strlen(entry->d_name),
                        NULL, &child_inode) ||
          !restore_tree(fs, child_inode, child_fd, depth + 1)) {
        if (child_fd >= 0) {
          close(child_fd);
        }
        ok = FALSE;
        break;
      }
      close(child_fd);
    }
  }
  closedir(directory);
  return ok;
}

static gboolean create_ext4(const char *rootfs_path, const char *disk_path,
                            GError **error) {
  guint64 size = 0;
  if (!quocker_rootfs_ext4_estimate(rootfs_path, &size, error)) {
    return FALSE;
  }
  gboolean ok;
  char *temporary_path = g_strdup_printf("%s.tmp.XXXXXX", disk_path);
  int disk_fd = g_mkstemp(temporary_path);
  if (disk_fd < 0 || ftruncate(disk_fd, (off_t)size) < 0) {
    if (disk_fd >= 0) {
      close(disk_fd);
      unlink(temporary_path);
    }
    disk_error(error, "could not create raw ext4 image file");
    g_free(temporary_path);
    return FALSE;
  }
  close(disk_fd);
  char *size_text = g_strdup_printf("%" G_GUINT64_FORMAT, size / 1024);
  const char *arguments[] = {"mke2fs",  "-q", "-F",        "-t",
                             "ext4",    "-d", rootfs_path, temporary_path,
                             size_text, NULL};
  gchar **spawn_arguments = g_new0(gchar *, G_N_ELEMENTS(arguments));
  for (guint i = 0; i < G_N_ELEMENTS(arguments) - 1; i++) {
    spawn_arguments[i] = g_strdup(arguments[i]);
  }
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *spawn_error = NULL;
  gboolean spawned =
      g_spawn_sync(NULL, spawn_arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                   &stdout_text, &stderr_text, &status, &spawn_error);
  g_strfreev(spawn_arguments);
  g_free(stdout_text);
  g_free(size_text);
  if (!spawned || !g_spawn_check_wait_status(status, &spawn_error)) {
    if (error && !*error) {
      g_set_error(error, disk_error_quark(), 1, "mke2fs failed: %s",
                  stderr_text && *stderr_text ? stderr_text
                                              : spawn_error->message);
    }
    g_clear_error(&spawn_error);
    g_free(stderr_text);
    unlink(temporary_path);
    g_free(temporary_path);
    return FALSE;
  }
  g_free(stderr_text);
  ext2_filsys fs = NULL;
  errcode_t code =
      ext2fs_open(temporary_path, EXT2_FLAG_RW, 0, 0, unix_io_manager, &fs);
  int root_fd =
      open(rootfs_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  ok = code == 0 && root_fd >= 0 && restore_tree(fs, EXT2_ROOT_INO, root_fd, 0);
  if (root_fd >= 0) {
    close(root_fd);
  }
  if (fs) {
    errcode_t close_code = ext2fs_close(fs);
    ok = ok && close_code == 0;
  }
  if (ok) {
    int sync_fd = open(temporary_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ok = sync_fd >= 0 && fsync(sync_fd) == 0;
    if (sync_fd >= 0) {
      close(sync_fd);
    }
  }
  if (!ok) {
    disk_error(error, "could not replay OCI metadata into ext4 image");
    unlink(temporary_path);
    g_free(temporary_path);
    return FALSE;
  }
  if (link(temporary_path, disk_path) < 0) {
    disk_error(error, errno == EEXIST
                          ? "raw ext4 output already exists"
                          : "could not publish completed raw ext4 image");
    unlink(temporary_path);
    g_free(temporary_path);
    return FALSE;
  }
  unlink(temporary_path);
  g_free(temporary_path);
  char *basename = g_path_get_basename(disk_path);
  gboolean cache_disk = strlen(basename) == 69 &&
                        g_str_has_suffix(basename, ".ext4");
  for (guint i = 0; cache_disk && i < 64; i++) {
    cache_disk = g_ascii_isxdigit(basename[i]);
  }
  if (cache_disk) {
    char *marker = g_strconcat(disk_path, ".quocker", NULL);
    char *contents = g_strdup_printf("sha256:%.*s", 64, basename);
    if (!g_file_set_contents(marker, contents, -1, NULL)) {
      unlink(disk_path);
      disk_error(error, "could not mark completed OCI base disk");
      g_free(contents);
      g_free(marker);
      g_free(basename);
      return FALSE;
    }
    g_free(contents);
    g_free(marker);
  }
  g_free(basename);
  return TRUE;
}

gboolean quocker_rootfs_to_ext4(const char *rootfs_path, const char *disk_path,
                                GError **error) {
  if (error) {
    *error = NULL;
  }
  if (!rootfs_path || !disk_path || !g_path_is_absolute(disk_path)) {
    disk_error(error,
               "root filesystem and absolute disk output path are required");
    return FALSE;
  }
  return create_ext4(rootfs_path, disk_path, error);
}
