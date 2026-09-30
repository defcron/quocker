/*
 * Quocker's OCI layer materializer. Images are data for VM root filesystems;
 * this code never invokes a container runtime.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-rootfs.h"
#include "quocker-oci.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define ROOTFS_MAX_BYTES (64ULL * 1024 * 1024 * 1024)
#define ROOTFS_MAX_ENTRIES 2000000ULL
#define ROOTFS_MAX_PATH 4096
#define ROOTFS_MAX_XATTRS 64
#define ROOTFS_MAX_XATTR_BYTES (32 * 1024)

static void rootfs_error(const char *message);
static gboolean set_ownership_metadata(int fd, la_int64_t uid, la_int64_t gid,
                                       mode_t mode);
static gboolean store_extended_attributes(int fd, int parent_fd,
                                          const char *leaf,
                                          struct archive_entry *entry,
                                          const char *cache_directory);

static gboolean rootfs_provenance_build(const char *manifest_digest,
                                        GPtrArray *layer_paths,
                                        char **contents_out) {
  GString *contents = g_string_new("quocker-rootfs-provenance-v1\n");
  g_string_append_printf(contents, "manifest=%s\n", manifest_digest);
  for (guint i = 0; i < layer_paths->len; i++) {
    const char *path = g_ptr_array_index(layer_paths, i);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
      if (fd >= 0) {
        close(fd);
      }
      g_string_free(contents, TRUE);
      rootfs_error("cannot inspect OCI layer while recording provenance");
      return FALSE;
    }
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    char buffer[64 * 1024];
    gboolean ok = TRUE;
    for (;;) {
      ssize_t count = read(fd, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        ok = FALSE;
      } else if (count == 0) {
        break;
      } else {
        g_checksum_update(checksum, (const guchar *)buffer, (gsize)count);
      }
      if (!ok) {
        break;
      }
    }
    close(fd);
    if (!ok) {
      g_checksum_free(checksum);
      g_string_free(contents, TRUE);
      rootfs_error("cannot read OCI layer while recording provenance");
      return FALSE;
    }
    g_string_append_printf(contents, "layer[%u]=sha256:%s\n", i,
                           g_checksum_get_string(checksum));
    g_checksum_free(checksum);
  }
  *contents_out = g_string_free(contents, FALSE);
  return TRUE;
}

static gboolean set_xattr_at(int parent_fd, const char *name,
                             const char *attribute, const void *value,
                             size_t value_size) {
  char *path = g_strdup_printf("/proc/self/fd/%d/%s", parent_fd, name);
  gboolean ok = lsetxattr(path, attribute, value, value_size, 0) == 0;
  g_free(path);
  return ok;
}

static gboolean store_extended_attributes(int fd, int parent_fd,
                                          const char *leaf,
                                          struct archive_entry *entry,
                                          const char *cache_directory) {
  int count = archive_entry_xattr_reset(entry);
  if (count <= 0) {
    return TRUE;
  }
  if (count > ROOTFS_MAX_XATTRS) {
    rootfs_error("archive entry exceeds the extended-attribute count limit");
    return FALSE;
  }
  guint64 total = 0;
  for (int i = 0; i < count; i++) {
    const char *name = NULL;
    const void *value = NULL;
    size_t value_size = 0;
    if (archive_entry_xattr_next(entry, &name, &value, &value_size) != 0 ||
        !name || !*name || strchr(name, '/') || strlen(name) > 1024 ||
        value_size > ROOTFS_MAX_XATTR_BYTES ||
        G_MAXUINT64 - total < strlen(name) + 1 + value_size) {
      rootfs_error("archive entry contains an invalid extended attribute");
      return FALSE;
    }
    total += strlen(name) + 1 + value_size;
    if (total > ROOTFS_MAX_XATTR_BYTES) {
      rootfs_error("archive entry exceeds the extended-attribute size limit");
      return FALSE;
    }
  }
  if (!quocker_oci_cache_has_room(cache_directory, total)) {
    return FALSE;
  }
  archive_entry_xattr_reset(entry);
  for (int i = 0; i < count; i++) {
    const char *name = NULL;
    const void *value = NULL;
    size_t value_size = 0;
    if (archive_entry_xattr_next(entry, &name, &value, &value_size) != 0) {
      return FALSE;
    }
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)name, strlen(name));
    char *attribute = g_strdup_printf("user.quocker.oci.xattr.%s",
                                      g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    gsize name_size = strlen(name) + 1;
    gsize record_size = name_size + value_size;
    guchar *record = g_malloc(record_size);
    memcpy(record, name, name_size);
    if (value_size) {
      memcpy(record + name_size, value, value_size);
    }
    gboolean ok =
        fd >= 0 ? fsetxattr(fd, attribute, record, record_size, 0) == 0
                : set_xattr_at(parent_fd, leaf, attribute, record, record_size);
    g_free(record);
    g_free(attribute);
    if (!ok) {
      rootfs_error("host filesystem cannot preserve OCI extended attributes");
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean set_ownership_metadata_at(int parent_fd, const char *leaf,
                                          la_int64_t uid, la_int64_t gid,
                                          mode_t mode) {
  char uid_text[32];
  char gid_text[32];
  char mode_text[32];
  g_snprintf(uid_text, sizeof(uid_text), "%" G_GINT64_FORMAT, (gint64)uid);
  g_snprintf(gid_text, sizeof(gid_text), "%" G_GINT64_FORMAT, (gint64)gid);
  g_snprintf(mode_text, sizeof(mode_text), "%u", (unsigned)(mode & 07777));
  char *path = g_strdup_printf("/proc/self/fd/%d/%s", parent_fd, leaf);
  gboolean ok = lsetxattr(path, "user.quocker.oci.uid", uid_text,
                          strlen(uid_text) + 1, 0) == 0 &&
                lsetxattr(path, "user.quocker.oci.gid", gid_text,
                          strlen(gid_text) + 1, 0) == 0 &&
                lsetxattr(path, "user.quocker.oci.mode", mode_text,
                          strlen(mode_text) + 1, 0) == 0;
  g_free(path);
  return ok;
}

static void rootfs_error(const char *message) {
  g_printerr("quocker: OCI rootfs: %s\n",
             message ? message : "archive reader rejected the layer");
}

static char *normalize_archive_path(const char *path) {
  if (!path || !*path || path[0] == '/' || strlen(path) > ROOTFS_MAX_PATH) {
    return NULL;
  }
  gchar **parts = g_strsplit(path, "/", -1);
  GString *normalized = g_string_new(NULL);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_str_equal(parts[i], ".")) {
      continue;
    }
    if (g_str_equal(parts[i], "..")) {
      g_string_free(normalized, TRUE);
      g_strfreev(parts);
      return NULL;
    }
    if (normalized->len) {
      g_string_append_c(normalized, '/');
    }
    g_string_append(normalized, parts[i]);
  }
  g_strfreev(parts);
  if (!normalized->len) {
    g_string_free(normalized, TRUE);
    return NULL;
  }
  return g_string_free(normalized, FALSE);
}

static gboolean split_parent(const char *path, char **parent_out,
                             char **name_out) {
  char *copy = g_strdup(path);
  char *slash = strrchr(copy, '/');
  if (slash) {
    *slash = '\0';
    *parent_out = g_strdup(copy);
    *name_out = g_strdup(slash + 1);
  } else {
    *parent_out = g_strdup("");
    *name_out = g_strdup(copy);
  }
  g_free(copy);
  return **name_out != '\0';
}

static int open_directory_at(int root_fd, const char *path, gboolean create) {
  if (!path || !*path) {
    return dup(root_fd);
  }
  char *normalized = normalize_archive_path(path);
  if (!normalized) {
    errno = EINVAL;
    return -1;
  }
  int current = dup(root_fd);
  gchar **parts = g_strsplit(normalized, "/", -1);
  for (guint i = 0; current >= 0 && parts[i]; i++) {
    int next = openat(current, parts[i],
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0 && errno == ENOENT && create) {
      gboolean created = mkdirat(current, parts[i], 0700) == 0;
      if (!created && errno != EEXIST) {
        close(current);
        current = -1;
        break;
      }
      next = openat(current, parts[i],
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next >= 0 && created && !set_ownership_metadata(next, 0, 0, 0755)) {
        close(next);
        next = -1;
      }
    }
    close(current);
    current = next;
  }
  g_strfreev(parts);
  g_free(normalized);
  return current;
}

static int open_parent_at(int root_fd, const char *path, gboolean create,
                          char **leaf_out) {
  char *parent = NULL;
  char *leaf = NULL;
  if (!split_parent(path, &parent, &leaf)) {
    g_free(parent);
    g_free(leaf);
    errno = EINVAL;
    return -1;
  }
  int fd = open_directory_at(root_fd, parent, create);
  g_free(parent);
  if (fd < 0) {
    g_free(leaf);
    return -1;
  }
  *leaf_out = leaf;
  return fd;
}

static gboolean remove_entry_at_depth(int parent_fd, const char *name,
                                      guint64 *removed_bytes, guint depth) {
  if (depth > 256) {
    errno = ELOOP;
    return FALSE;
  }
  struct stat st;
  if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
    return errno == ENOENT;
  }
  if (S_ISDIR(st.st_mode)) {
    int child_fd = openat(parent_fd, name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child_fd < 0) {
      return FALSE;
    }
    DIR *directory = fdopendir(dup(child_fd));
    if (!directory) {
      close(child_fd);
      return FALSE;
    }
    struct dirent *entry;
    gboolean ok = TRUE;
    while ((entry = readdir(directory))) {
      if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) {
        continue;
      }
      if (!remove_entry_at_depth(child_fd, entry->d_name, removed_bytes,
                                 depth + 1)) {
        ok = FALSE;
        break;
      }
    }
    closedir(directory);
    close(child_fd);
    if (!ok) {
      return FALSE;
    }
    return unlinkat(parent_fd, name, AT_REMOVEDIR) == 0 || errno == ENOENT;
  }
  if (removed_bytes && S_ISREG(st.st_mode)) {
    *removed_bytes += st.st_size;
  }
  return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;
}

static gboolean remove_entry_at_count(int parent_fd, const char *name,
                                      guint64 *removed_bytes) {
  return remove_entry_at_depth(parent_fd, name, removed_bytes, 0);
}

static gboolean remove_entry_at(int parent_fd, const char *name) {
  return remove_entry_at_count(parent_fd, name, NULL);
}

static gboolean clear_directory_at(int directory_fd) {
  DIR *directory = fdopendir(dup(directory_fd));
  if (!directory) {
    return FALSE;
  }
  struct dirent *entry;
  gboolean ok = TRUE;
  while ((entry = readdir(directory))) {
    if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) {
      continue;
    }
    if (!remove_entry_at(directory_fd, entry->d_name)) {
      ok = FALSE;
      break;
    }
  }
  closedir(directory);
  return ok;
}

static gboolean apply_whiteout(int root_fd, const char *marker) {
  char *parent = NULL;
  char *name = NULL;
  if (!split_parent(marker, &parent, &name)) {
    g_free(parent);
    g_free(name);
    return FALSE;
  }
  int directory_fd = open_directory_at(root_fd, parent, FALSE);
  if (directory_fd < 0) {
    gboolean missing = errno == ENOENT;
    g_free(parent);
    g_free(name);
    return missing;
  }
  gboolean ok = TRUE;
  if (g_str_equal(name, ".wh..wh..opq")) {
    ok = clear_directory_at(directory_fd);
  } else if (g_str_has_prefix(name, ".wh.")) {
    const char *target_name = name + 4;
    if (!*target_name || strchr(target_name, '/')) {
      ok = FALSE;
    } else {
      ok = remove_entry_at(directory_fd, target_name);
    }
  } else {
    ok = FALSE;
  }
  close(directory_fd);
  g_free(parent);
  g_free(name);
  return ok;
}

static gboolean scan_layer(const char *path, GPtrArray *whiteouts,
                           guint64 *entry_count, guint64 *declared_bytes) {
  struct archive *archive = archive_read_new();
  archive_read_support_filter_all(archive);
  archive_read_support_format_tar(archive);
  if (archive_read_open_filename(archive, path, 64 * 1024) != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    archive_read_free(archive);
    return FALSE;
  }
  struct archive_entry *entry;
  gboolean ok = TRUE;
  int result;
  while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char *raw_name = archive_entry_pathname(entry);
    if (archive_entry_filetype(entry) == AE_IFDIR && raw_name &&
        (g_str_equal(raw_name, ".") || g_str_equal(raw_name, "./"))) {
      archive_read_data_skip(archive);
      continue;
    }
    if (++*entry_count > ROOTFS_MAX_ENTRIES) {
      rootfs_error("layer set exceeds the entry-count limit");
      ok = FALSE;
      break;
    }
    if (archive_entry_uid(entry) < 0 || archive_entry_gid(entry) < 0) {
      rootfs_error("archive contains a negative UID or GID");
      ok = FALSE;
      break;
    }
    char *name = normalize_archive_path(archive_entry_pathname(entry));
    if (!name) {
      rootfs_error("archive contains an absolute, empty, or traversing path");
      ok = FALSE;
      break;
    }
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (g_str_has_prefix(base, ".wh.")) {
      g_ptr_array_add(whiteouts, name);
    } else {
      g_free(name);
    }
    if (archive_entry_filetype(entry) == AE_IFREG &&
        !archive_entry_hardlink(entry)) {
      la_int64_t size = archive_entry_size(entry);
      if (size < 0 || (guint64)size > ROOTFS_MAX_BYTES - *declared_bytes) {
        rootfs_error("layer set exceeds the expanded-size limit");
        ok = FALSE;
        break;
      }
      *declared_bytes += size;
    }
    archive_read_data_skip(archive);
  }
  if (result != ARCHIVE_EOF && result != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    ok = FALSE;
  }
  if (archive_read_close(archive) != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    ok = FALSE;
  }
  archive_read_free(archive);
  return ok;
}

static gboolean set_ownership_metadata(int fd, la_int64_t uid, la_int64_t gid,
                                       mode_t mode) {
  char uid_text[32];
  char gid_text[32];
  char mode_text[32];
  g_snprintf(uid_text, sizeof(uid_text), "%" G_GINT64_FORMAT, (gint64)uid);
  g_snprintf(gid_text, sizeof(gid_text), "%" G_GINT64_FORMAT, (gint64)gid);
  g_snprintf(mode_text, sizeof(mode_text), "%u", (unsigned)(mode & 07777));
  if (fsetxattr(fd, "user.quocker.oci.uid", uid_text, strlen(uid_text) + 1, 0) <
          0 ||
      fsetxattr(fd, "user.quocker.oci.gid", gid_text, strlen(gid_text) + 1, 0) <
          0 ||
      fsetxattr(fd, "user.quocker.oci.mode", mode_text, strlen(mode_text) + 1,
                0) < 0) {
    rootfs_error(
        "host filesystem does not support storing OCI mode/ownership metadata");
    return FALSE;
  }
  return TRUE;
}

static gboolean write_regular(struct archive *archive, int parent_fd,
                              const char *name, struct archive_entry *entry,
                              guint64 *expanded_bytes,
                              const char *cache_directory) {
  la_int64_t expected_size = archive_entry_size(entry);
  if (expected_size < 0 ||
      (guint64)expected_size > ROOTFS_MAX_BYTES - *expanded_bytes) {
    rootfs_error("expanded image exceeds the 64 GiB limit");
    return FALSE;
  }
  if (!quocker_oci_cache_has_room(cache_directory, expected_size)) {
    return FALSE;
  }
  int fd = openat(parent_fd, name,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return FALSE;
  }
  gboolean ok = TRUE;
  char buffer[64 * 1024];
  la_ssize_t count;
  guint64 written_total = 0;
  while ((count = archive_read_data(archive, buffer, sizeof(buffer))) > 0) {
    gsize offset = 0;
    while (offset < (gsize)count) {
      ssize_t written = write(fd, buffer + offset, count - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        ok = FALSE;
        break;
      }
      offset += written;
      written_total += written;
    }
    if (!ok) {
      break;
    }
  }
  if (count < 0 || written_total != (guint64)expected_size) {
    ok = FALSE;
  }
  if (ok && fchmod(fd, 0600) < 0) {
    ok = FALSE;
  }
  if (ok && !set_ownership_metadata(fd, archive_entry_uid(entry),
                                    archive_entry_gid(entry),
                                    archive_entry_perm(entry))) {
    ok = FALSE;
  }
  if (ok && !store_extended_attributes(fd, -1, NULL, entry, cache_directory)) {
    ok = FALSE;
  }
  if (ok && fsync(fd) < 0) {
    ok = FALSE;
  }
  close(fd);
  if (!ok) {
    unlinkat(parent_fd, name, 0);
    if (count < 0) {
      rootfs_error(archive_error_string(archive));
    }
    return FALSE;
  }
  *expanded_bytes += written_total;
  return TRUE;
}

static gboolean write_special_placeholder(int parent_fd, const char *name,
                                          struct archive_entry *entry,
                                          const char *cache_directory) {
  mode_t type = archive_entry_filetype(entry);
  const char *type_name = type == AE_IFCHR   ? "char"
                          : type == AE_IFBLK ? "block"
                          : type == AE_IFIFO ? "fifo"
                                             : NULL;
  if (!type_name) {
    return FALSE;
  }
  if (!quocker_oci_cache_has_room(cache_directory, 256)) {
    return FALSE;
  }
  int fd = openat(parent_fd, name,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return FALSE;
  }
  char major_text[32];
  char minor_text[32];
  g_snprintf(major_text, sizeof(major_text), "%" G_GUINT64_FORMAT,
             (guint64)archive_entry_rdevmajor(entry));
  g_snprintf(minor_text, sizeof(minor_text), "%" G_GUINT64_FORMAT,
             (guint64)archive_entry_rdevminor(entry));
  gboolean ok = set_ownership_metadata(fd, archive_entry_uid(entry),
                                       archive_entry_gid(entry),
                                       archive_entry_perm(entry)) &&
                fsetxattr(fd, "user.quocker.oci.type", type_name,
                          strlen(type_name) + 1, 0) == 0;
  if (ok && (type == AE_IFCHR || type == AE_IFBLK)) {
    ok = fsetxattr(fd, "user.quocker.oci.devmajor", major_text,
                   strlen(major_text) + 1, 0) == 0 &&
         fsetxattr(fd, "user.quocker.oci.devminor", minor_text,
                   strlen(minor_text) + 1, 0) == 0;
  }
  if (ok) {
    ok = store_extended_attributes(fd, -1, NULL, entry, cache_directory);
  }
  if (ok) {
    ok = fsync(fd) == 0;
  }
  close(fd);
  if (!ok) {
    unlinkat(parent_fd, name, 0);
    rootfs_error("could not safely retain OCI special-file metadata");
  }
  return ok;
}

static gboolean extract_layer(const char *path, int root_fd,
                              guint64 *expanded_bytes, guint64 *entry_count,
                              guint64 *declared_bytes,
                              const char *cache_directory) {
  GPtrArray *whiteouts = g_ptr_array_new_with_free_func(g_free);
  gboolean ok = scan_layer(path, whiteouts, entry_count, declared_bytes);
  for (guint i = 0; ok && i < whiteouts->len; i++) {
    ok = apply_whiteout(root_fd, g_ptr_array_index(whiteouts, i));
    if (!ok) {
      rootfs_error("could not safely apply OCI whiteout");
    }
  }
  g_ptr_array_free(whiteouts, TRUE);
  if (!ok) {
    return FALSE;
  }
  struct archive *archive = archive_read_new();
  archive_read_support_filter_all(archive);
  archive_read_support_format_tar(archive);
  if (archive_read_open_filename(archive, path, 64 * 1024) != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    archive_read_free(archive);
    return FALSE;
  }
  struct archive_entry *entry;
  int result;
  while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char *raw_name = archive_entry_pathname(entry);
    if (archive_entry_filetype(entry) == AE_IFDIR && raw_name &&
        (g_str_equal(raw_name, ".") || g_str_equal(raw_name, "./"))) {
      archive_read_data_skip(archive);
      continue;
    }
    char *path_name = normalize_archive_path(raw_name);
    if (!path_name) {
      rootfs_error("archive contains an unsafe path");
      ok = FALSE;
      break;
    }
    const char *base = strrchr(path_name, '/');
    base = base ? base + 1 : path_name;
    if (g_str_has_prefix(base, ".wh.")) {
      g_free(path_name);
      archive_read_data_skip(archive);
      continue;
    }
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
      char *leaf = NULL;
      int parent_fd = open_parent_at(root_fd, path_name, TRUE, &leaf);
      if (parent_fd < 0) {
        ok = FALSE;
      } else {
        struct stat st;
        if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            !S_ISDIR(st.st_mode)) {
          ok = remove_entry_at(parent_fd, leaf);
        }
        if (ok && mkdirat(parent_fd, leaf, 0700) < 0 && errno != EEXIST) {
          ok = FALSE;
        }
        int dir_fd =
            ok ? openat(parent_fd, leaf,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
               : -1;
        if (dir_fd < 0) {
          ok = FALSE;
        } else {
          ok = set_ownership_metadata(dir_fd, archive_entry_uid(entry),
                                      archive_entry_gid(entry),
                                      archive_entry_perm(entry));
          if (ok) {
            ok = store_extended_attributes(dir_fd, -1, NULL, entry,
                                           cache_directory);
          }
          close(dir_fd);
        }
        close(parent_fd);
        g_free(leaf);
      }
      archive_read_data_skip(archive);
    } else if (archive_entry_hardlink(entry)) {
      char *target = normalize_archive_path(archive_entry_hardlink(entry));
      char *source_leaf = NULL;
      char *dest_leaf = NULL;
      int source_fd =
          target ? open_parent_at(root_fd, target, FALSE, &source_leaf) : -1;
      int dest_fd = open_parent_at(root_fd, path_name, TRUE, &dest_leaf);
      struct stat source_stat;
      if (source_fd < 0 || dest_fd < 0 ||
          fstatat(source_fd, source_leaf, &source_stat, AT_SYMLINK_NOFOLLOW) <
              0 ||
          !S_ISREG(source_stat.st_mode)) {
        ok = FALSE;
      } else {
        ok = remove_entry_at(dest_fd, dest_leaf) &&
             linkat(source_fd, source_leaf, dest_fd, dest_leaf, 0) == 0;
        if (ok) {
          int linked_fd =
              openat(dest_fd, dest_leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
          ok = linked_fd >= 0 &&
               set_ownership_metadata(linked_fd, archive_entry_uid(entry),
                                      archive_entry_gid(entry),
                                      archive_entry_perm(entry)) &&
               store_extended_attributes(linked_fd, -1, NULL, entry,
                                         cache_directory);
          if (linked_fd >= 0) {
            close(linked_fd);
          }
        }
      }
      if (source_fd >= 0)
        close(source_fd);
      if (dest_fd >= 0)
        close(dest_fd);
      g_free(source_leaf);
      g_free(dest_leaf);
      g_free(target);
      archive_read_data_skip(archive);
    } else if (type == AE_IFREG) {
      char *leaf = NULL;
      int parent_fd = open_parent_at(root_fd, path_name, TRUE, &leaf);
      if (parent_fd < 0) {
        ok = FALSE;
      } else {
        ok = remove_entry_at(parent_fd, leaf);
        if (ok) {
          ok = write_regular(archive, parent_fd, leaf, entry, expanded_bytes,
                             cache_directory);
        }
        close(parent_fd);
        g_free(leaf);
      }
      if (!ok) {
        archive_read_data_skip(archive);
      }
    } else if (type == AE_IFLNK) {
      const char *target = archive_entry_symlink(entry);
      char *leaf = NULL;
      int parent_fd = open_parent_at(root_fd, path_name, TRUE, &leaf);
      if (!target || parent_fd < 0) {
        ok = FALSE;
      } else {
        ok = remove_entry_at(parent_fd, leaf) &&
             symlinkat(target, parent_fd, leaf) == 0;
        if (ok) {
          ok = set_ownership_metadata_at(
                   parent_fd, leaf, archive_entry_uid(entry),
                   archive_entry_gid(entry), archive_entry_perm(entry)) &&
               store_extended_attributes(-1, parent_fd, leaf, entry,
                                         cache_directory);
        }
        close(parent_fd);
        g_free(leaf);
      }
      archive_read_data_skip(archive);
    } else if (type == 0) {
      rootfs_error("archive entry has an unsupported file type");
      ok = FALSE;
      archive_read_data_skip(archive);
    } else if (type == AE_IFCHR || type == AE_IFBLK || type == AE_IFIFO) {
      char *leaf = NULL;
      int parent_fd = open_parent_at(root_fd, path_name, TRUE, &leaf);
      if (parent_fd < 0) {
        ok = FALSE;
      } else {
        ok = remove_entry_at(parent_fd, leaf) &&
             write_special_placeholder(parent_fd, leaf, entry, cache_directory);
        close(parent_fd);
        g_free(leaf);
      }
      archive_read_data_skip(archive);
    } else {
      rootfs_error("socket and unknown archive entry types are rejected");
      ok = FALSE;
      archive_read_data_skip(archive);
    }
    g_free(path_name);
    if (!ok) {
      break;
    }
  }
  if (result != ARCHIVE_EOF && result != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    ok = FALSE;
  }
  if (archive_read_close(archive) != ARCHIVE_OK) {
    rootfs_error(archive_error_string(archive));
    ok = FALSE;
  }
  archive_read_free(archive);
  return ok;
}

static gboolean remove_rootfs_directory(const char *parent, const char *name) {
  int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent_fd < 0) {
    return FALSE;
  }
  gboolean ok = remove_entry_at(parent_fd, name);
  close(parent_fd);
  return ok;
}

gboolean quocker_rootfs_materialize(const char *manifest_digest,
                                    GPtrArray *layer_paths,
                                    const char *cache_directory,
                                    char **rootfs_path_out) {
  if (!manifest_digest || !g_str_has_prefix(manifest_digest, "sha256:") ||
      strlen(manifest_digest) != 71) {
    rootfs_error("invalid selected manifest digest");
    return FALSE;
  }
  for (guint i = 7; i < 71; i++) {
    if (!g_ascii_isxdigit(manifest_digest[i])) {
      rootfs_error("invalid selected manifest digest");
      return FALSE;
    }
  }
  if (!layer_paths) {
    rootfs_error("missing OCI layer list");
    return FALSE;
  }
  char *rootfs_parent = g_build_filename(cache_directory, "rootfs", NULL);
  struct stat parent_stat;
  if (g_mkdir_with_parents(rootfs_parent, 0700) < 0 ||
      lstat(rootfs_parent, &parent_stat) < 0 || !S_ISDIR(parent_stat.st_mode) ||
      parent_stat.st_uid != geteuid() ||
      (parent_stat.st_mode & (S_IWGRP | S_IWOTH))) {
    rootfs_error("cannot create rootfs cache directory");
    g_free(rootfs_parent);
    return FALSE;
  }
  const char *hex = manifest_digest + strlen("sha256:");
  char *final_path = g_build_filename(rootfs_parent, hex, NULL);
  char *marker_path = g_strdup_printf("%s.complete", final_path);
  char *provenance_path = g_strdup_printf("%s.provenance", final_path);
  char *provenance = NULL;
  if (!rootfs_provenance_build(manifest_digest, layer_paths, &provenance)) {
    g_free(provenance_path);
    g_free(marker_path);
    g_free(final_path);
    g_free(rootfs_parent);
    return FALSE;
  }
  char *marker = NULL;
  char *saved_provenance = NULL;
  if (g_file_get_contents(marker_path, &marker, NULL, NULL) &&
      g_strcmp0(marker, manifest_digest) == 0 &&
      g_file_get_contents(provenance_path, &saved_provenance, NULL, NULL) &&
      g_strcmp0(saved_provenance, provenance) == 0 &&
      g_file_test(final_path, G_FILE_TEST_IS_DIR)) {
    if (rootfs_path_out)
      *rootfs_path_out = g_strdup(final_path);
    g_free(marker);
    g_free(saved_provenance);
    g_free(provenance);
    g_free(provenance_path);
    g_free(marker_path);
    g_free(final_path);
    g_free(rootfs_parent);
    return TRUE;
  }
  g_free(marker);
  g_free(saved_provenance);
  g_unlink(marker_path);
  g_unlink(provenance_path);
  if (g_file_test(final_path, G_FILE_TEST_EXISTS) &&
      !remove_rootfs_directory(rootfs_parent, hex)) {
    rootfs_error("cannot remove incomplete previous rootfs");
    g_free(marker_path);
    g_free(provenance);
    g_free(provenance_path);
    g_free(final_path);
    g_free(rootfs_parent);
    return FALSE;
  }
  char *template = g_build_filename(rootfs_parent, ".staging-XXXXXX", NULL);
  char *staging = g_mkdtemp(template);
  if (!staging) {
    rootfs_error("cannot create private rootfs staging directory");
    g_free(template);
    g_free(marker_path);
    g_free(provenance);
    g_free(provenance_path);
    g_free(final_path);
    g_free(rootfs_parent);
    return FALSE;
  }
  int root_fd = open(staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  gboolean ok = root_fd >= 0;
  if (ok) {
    ok = set_ownership_metadata(root_fd, 0, 0, 0755);
  }
  guint64 expanded_bytes = 0;
  guint64 entry_count = 0;
  guint64 declared_bytes = 0;
  for (guint i = 0; ok && i < layer_paths->len; i++) {
    ok = extract_layer(g_ptr_array_index(layer_paths, i), root_fd,
                       &expanded_bytes, &entry_count, &declared_bytes,
                       cache_directory);
  }
  if (root_fd >= 0)
    close(root_fd);
  if (ok && g_rename(staging, final_path) < 0) {
    ok = FALSE;
  }
  if (!ok) {
    remove_rootfs_directory(rootfs_parent, strrchr(staging, '/') + 1);
    rootfs_error(
        "rootfs materialization failed; incomplete staging was removed");
  } else {
    guint64 metadata_size = strlen(manifest_digest) + strlen(provenance);
    if (!quocker_oci_cache_has_room(cache_directory, metadata_size)) {
      remove_rootfs_directory(rootfs_parent, hex);
      ok = FALSE;
      rootfs_error("OCI cache limit exceeded while writing rootfs marker");
    } else if (!g_file_set_contents(provenance_path, provenance, -1, NULL) ||
               !g_file_set_contents(marker_path, manifest_digest, -1, NULL)) {
      g_unlink(provenance_path);
      g_unlink(marker_path);
      remove_rootfs_directory(rootfs_parent, hex);
      ok = FALSE;
      rootfs_error("could not write rootfs provenance/completion metadata");
    } else if (rootfs_path_out) {
      *rootfs_path_out = g_strdup(final_path);
    }
  }
  g_free(template);
  g_free(provenance);
  g_free(provenance_path);
  g_free(marker_path);
  g_free(final_path);
  g_free(rootfs_parent);
  return ok;
}

gboolean quocker_rootfs_cache_prune_except(const char *cache_directory,
                                           GHashTable *keep_files,
                                           guint64 *removed_bytes_out) {
  if (removed_bytes_out) {
    *removed_bytes_out = 0;
  }
  int cache_fd =
      open(cache_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (cache_fd < 0) {
    return errno == ENOENT;
  }
  struct stat st;
  if (fstatat(cache_fd, "rootfs", &st, AT_SYMLINK_NOFOLLOW) < 0) {
    gboolean missing = errno == ENOENT;
    close(cache_fd);
    return missing;
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & (S_IWGRP | S_IWOTH))) {
    rootfs_error("refusing to prune an unsafe rootfs cache directory");
    close(cache_fd);
    return FALSE;
  }
  int rootfs_fd = openat(cache_fd, "rootfs",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  DIR *rootfs = rootfs_fd >= 0 ? fdopendir(dup(rootfs_fd)) : NULL;
  gboolean ok = rootfs != NULL;
  guint64 bytes = 0;
  struct dirent *entry;
  while (ok && (entry = readdir(rootfs))) {
    const char *name = entry->d_name;
    if (g_str_equal(name, ".") || g_str_equal(name, "..")) {
      continue;
    }
    if (g_str_has_suffix(name, ".ext4.quocker")) {
      char *disk_name = g_strndup(name, strlen(name) - strlen(".quocker"));
      struct stat disk_stat;
      gboolean disk_exists =
          fstatat(rootfs_fd, disk_name, &disk_stat, AT_SYMLINK_NOFOLLOW) == 0;
      gboolean keep_disk = keep_files &&
                           g_hash_table_contains(keep_files, disk_name);
      if (!disk_exists && errno != ENOENT) {
        ok = FALSE;
      } else if (!disk_exists && !keep_disk) {
        if (fstatat(rootfs_fd, name, &disk_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(disk_stat.st_mode)) {
          bytes += disk_stat.st_size;
        }
        ok = unlinkat(rootfs_fd, name, 0) == 0 || errno == ENOENT;
      }
      g_free(disk_name);
      continue;
    }
    if (keep_files && g_hash_table_contains(keep_files, name)) {
      continue;
    }
    if (g_str_has_suffix(name, ".ext4")) {
      char *marker_name = g_strconcat(name, ".quocker", NULL);
      int marker_fd = openat(rootfs_fd, marker_name,
                             O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      if (marker_fd >= 0) {
        struct stat marker_stat;
        char marker[96] = {0};
        ssize_t count = read(marker_fd, marker, sizeof(marker) - 1);
        gboolean marker_valid =
            fstat(marker_fd, &marker_stat) == 0 &&
            S_ISREG(marker_stat.st_mode) && marker_stat.st_uid == geteuid() &&
            count >= 0 && (gsize)count < sizeof(marker) - 1;
        close(marker_fd);
        char expected[80];
        if (strlen(name) == 69 && g_str_has_suffix(name, ".ext4")) {
          g_snprintf(expected, sizeof(expected), "sha256:%.*s", 64, name);
          marker_valid = marker_valid && g_str_equal(marker, expected);
          for (guint i = 0; marker_valid && i < 64; i++) {
            marker_valid = g_ascii_isxdigit(name[i]);
          }
        } else {
          marker_valid = FALSE;
        }
        if (!marker_valid) {
          rootfs_error("refusing to prune an invalid managed-disk marker");
          ok = FALSE;
        } else {
          struct stat disk_stat;
          if (fstatat(rootfs_fd, name, &disk_stat, AT_SYMLINK_NOFOLLOW) < 0 ||
              !S_ISREG(disk_stat.st_mode)) {
            rootfs_error("refusing to prune an unsafe cached VM disk");
            ok = FALSE;
          } else {
            bytes += disk_stat.st_size;
            ok = unlinkat(rootfs_fd, name, 0) == 0 || errno == ENOENT;
            if (ok) {
              bytes += marker_stat.st_size;
              ok = unlinkat(rootfs_fd, marker_name, 0) == 0 || errno == ENOENT;
            }
          }
        }
      } else if (errno != ENOENT) {
        rootfs_error("could not inspect cached VM disk marker");
        ok = FALSE;
      }
      g_free(marker_name);
      continue;
    }
    if (!remove_entry_at_count(rootfs_fd, name, &bytes)) {
      ok = FALSE;
    }
  }
  if (rootfs) {
    closedir(rootfs);
  }
  if (rootfs_fd >= 0) {
    close(rootfs_fd);
  }
  if (ok && (!keep_files || g_hash_table_size(keep_files) == 0)) {
    unlinkat(cache_fd, "rootfs", AT_REMOVEDIR);
  }
  close(cache_fd);
  if (!ok) {
    rootfs_error("could not safely remove cached root filesystems");
    return FALSE;
  }
  if (removed_bytes_out) {
    *removed_bytes_out = bytes;
  }
  return TRUE;
}

gboolean quocker_rootfs_cache_prune(const char *cache_directory,
                                    guint64 *removed_bytes_out) {
  return quocker_rootfs_cache_prune_except(cache_directory, NULL,
                                           removed_bytes_out);
}

void quocker_distro_info_clear(QuockerDistroInfo *info) {
  if (!info) {
    return;
  }
  g_free(info->id);
  g_free(info->id_like);
  g_free(info->version_id);
  if (info->kernel_module_releases) {
    g_ptr_array_free(info->kernel_module_releases, TRUE);
  }
  memset(info, 0, sizeof(*info));
}

static gboolean distro_token_valid(const char *token, gboolean allow_space) {
  if (!token || !*token || strlen(token) > 128) {
    return FALSE;
  }
  for (const char *p = token; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-' ||
          *p == '+' || (allow_space && g_ascii_isspace(*p)))) {
      return FALSE;
    }
  }
  return TRUE;
}

static int open_modules_directory(int root_fd, const char *prefix) {
  int library_fd = -1;
  if (g_str_equal(prefix, "lib")) {
    struct stat st;
    if (fstatat(root_fd, "lib", &st, AT_SYMLINK_NOFOLLOW) < 0) {
      return -1;
    }
    if (S_ISLNK(st.st_mode)) {
      char target[PATH_MAX];
      ssize_t length = readlinkat(root_fd, "lib", target, sizeof(target) - 1);
      if (length < 0 || (size_t)length >= sizeof(target) - 1) {
        return -1;
      }
      target[length] = '\0';
      if (!g_str_equal(target, "usr/lib") && !g_str_equal(target, "/usr/lib")) {
        return -1;
      }
      int usr_fd = openat(root_fd, "usr",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (usr_fd < 0) {
        return -1;
      }
      library_fd = openat(usr_fd, "lib",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(usr_fd);
    } else if (S_ISDIR(st.st_mode)) {
      library_fd = openat(root_fd, "lib",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
  } else {
    int usr_fd =
        openat(root_fd, "usr", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (usr_fd >= 0) {
      library_fd = openat(usr_fd, "lib",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(usr_fd);
    }
  }
  if (library_fd < 0) {
    return -1;
  }
  int modules_fd = openat(library_fd, "modules",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(library_fd);
  return modules_fd;
}

static gboolean collect_module_releases(int root_fd, GPtrArray *releases) {
  const char *prefixes[] = {"lib", "usr/lib"};
  for (guint p = 0; p < G_N_ELEMENTS(prefixes); p++) {
    int modules_fd = open_modules_directory(root_fd, prefixes[p]);
    if (modules_fd < 0) {
      continue;
    }
    int scan_fd = fcntl(modules_fd, F_DUPFD_CLOEXEC, 3);
    close(modules_fd);
    if (scan_fd < 0) {
      continue;
    }
    DIR *directory = fdopendir(scan_fd);
    if (!directory) {
      close(scan_fd);
      continue;
    }
    struct dirent *entry;
    while ((entry = readdir(directory))) {
      if (releases->len >= 256) {
        break;
      }
      if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..") ||
          !distro_token_valid(entry->d_name, FALSE)) {
        continue;
      }
      struct stat st;
      if (fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) <
              0 ||
          !S_ISDIR(st.st_mode)) {
        continue;
      }
      gboolean duplicate = FALSE;
      for (guint i = 0; i < releases->len; i++) {
        duplicate |= g_str_equal(entry->d_name, g_ptr_array_index(releases, i));
      }
      if (!duplicate) {
        g_ptr_array_add(releases, g_strdup(entry->d_name));
      }
    }
    closedir(directory);
  }
  return releases->len > 0;
}

static char *os_release_value(const char *raw) {
  char *value = g_strdup(raw);
  g_strstrip(value);
  gsize length = strlen(value);
  if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                      (value[0] == '\'' && value[length - 1] == '\''))) {
    GError *error = NULL;
    char *unquoted = g_shell_unquote(value, &error);
    g_free(value);
    if (error) {
      g_clear_error(&error);
      g_free(unquoted);
      return NULL;
    }
    return unquoted;
  }
  if (strchr(value, '\n') || strchr(value, '\r')) {
    g_free(value);
    return NULL;
  }
  return value;
}

static gboolean read_os_release_file(int root_fd, char **contents_out,
                                     gsize *length_out) {
  int etc_fd =
      openat(root_fd, "etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (etc_fd < 0) {
    return FALSE;
  }
  int fd = openat(etc_fd, "os-release", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ELOOP) {
    char target[PATH_MAX];
    ssize_t size = readlinkat(etc_fd, "os-release", target, sizeof(target) - 1);
    if (size > 0) {
      target[size] = '\0';
      if (g_str_equal(target, "../usr/lib/os-release") ||
          g_str_equal(target, "/usr/lib/os-release")) {
        int usr_fd = openat(root_fd, "usr",
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int lib_fd =
            usr_fd >= 0
                ? openat(usr_fd, "lib",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                : -1;
        fd = lib_fd >= 0 ? openat(lib_fd, "os-release",
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
                         : -1;
        if (lib_fd >= 0) {
          close(lib_fd);
        }
        if (usr_fd >= 0) {
          close(usr_fd);
        }
      }
    }
  }
  close(etc_fd);
  if (fd < 0) {
    return FALSE;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      st.st_size > 64 * 1024) {
    close(fd);
    return FALSE;
  }
  gsize size = (gsize)st.st_size;
  char *contents = g_malloc(size + 1);
  gsize offset = 0;
  while (offset < size) {
    ssize_t count = read(fd, contents + offset, size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      g_free(contents);
      close(fd);
      return FALSE;
    }
    offset += count;
  }
  close(fd);
  if (memchr(contents, '\0', size)) {
    g_free(contents);
    return FALSE;
  }
  contents[size] = '\0';
  *contents_out = contents;
  *length_out = size;
  return TRUE;
}

gboolean quocker_rootfs_detect_distro(const char *rootfs_path,
                                      QuockerDistroInfo *info) {
  if (!rootfs_path || !info) {
    return FALSE;
  }
  memset(info, 0, sizeof(*info));
  int root_fd =
      open(rootfs_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root_fd < 0) {
    return FALSE;
  }
  char *contents = NULL;
  gsize length = 0;
  gboolean ok = read_os_release_file(root_fd, &contents, &length);
  info->kernel_module_releases = g_ptr_array_new_with_free_func(g_free);
  gchar **lines = ok ? g_strsplit(contents, "\n", -1) : NULL;
  for (guint i = 0; lines && lines[i]; i++) {
    char *line = g_strstrip(lines[i]);
    if (!*line || *line == '#') {
      continue;
    }
    char *equals = strchr(line, '=');
    if (!equals) {
      continue;
    }
    *equals = '\0';
    char *key = g_strstrip(line);
    char *raw = g_strstrip(equals + 1);
    if (!g_str_equal(key, "ID") && !g_str_equal(key, "ID_LIKE") &&
        !g_str_equal(key, "VERSION_ID")) {
      continue;
    }
    char *value = os_release_value(raw);
    if (!value) {
      continue;
    }
    if (g_str_equal(key, "ID")) {
      if (distro_token_valid(value, FALSE)) {
        g_free(info->id);
        info->id = g_ascii_strdown(value, -1);
      }
    } else if (g_str_equal(key, "ID_LIKE")) {
      gchar **tokens = g_strsplit_set(value, " \t", -1);
      GString *normalized = g_string_new(NULL);
      for (guint j = 0; tokens[j]; j++) {
        if (!*tokens[j]) {
          continue;
        }
        if (!distro_token_valid(tokens[j], FALSE)) {
          g_string_free(normalized, TRUE);
          normalized = NULL;
          break;
        }
        if (normalized->len) {
          g_string_append_c(normalized, ' ');
        }
        g_string_append(normalized, tokens[j]);
      }
      g_strfreev(tokens);
      if (normalized) {
        g_free(info->id_like);
        info->id_like = g_ascii_strdown(g_string_free(normalized, FALSE), -1);
      }
    } else if (distro_token_valid(value, FALSE)) {
      g_free(info->version_id);
      info->version_id = g_strdup(value);
    }
    g_free(value);
  }
  if (lines) {
    g_strfreev(lines);
  }
  g_free(contents);
  collect_module_releases(root_fd, info->kernel_module_releases);
  close(root_fd);
  if (!info->id && !info->kernel_module_releases->len) {
    quocker_distro_info_clear(info);
    return FALSE;
  }
  (void)length;
  return TRUE;
}
