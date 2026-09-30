/*
 * Project-scoped persistent VM volume disks.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-volume.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static GQuark volume_error_quark(void) {
  return g_quark_from_static_string("quocker-volume-error");
}

static void volume_error(GError **error, const char *message) {
  if (error && !*error) {
    g_set_error_literal(error, volume_error_quark(), 1, message);
  }
}

static char *volume_metadata_read(const char *disk_path);

static gboolean volume_disk_valid(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return FALSE;
  }
  struct stat st;
  unsigned char magic[2];
  gboolean valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                   st.st_size >= 64 * 1024 &&
                   st.st_size <= (off_t)(1ULL << 40) &&
                   pread(fd, magic, sizeof(magic), 1080) == sizeof(magic) &&
                   magic[0] == 0x53 && magic[1] == 0xef;
  close(fd);
  return valid;
}

void quocker_volume_info_free(QuockerVolumeInfo *info) {
  if (!info) {
    return;
  }
  g_free(info->logical_name);
  g_free(info->disk_path);
  g_free(info);
}

static gboolean volume_metadata_write(const char *disk_path,
                                      const char *logical_name) {
  char *escaped = g_strescape(logical_name, NULL);
  char *contents = g_strconcat(escaped, "\n", NULL);
  char *metadata_path = g_strconcat(disk_path, ".name", NULL);
  int fd = open(metadata_path,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0 && errno == EEXIST) {
    char *existing_name = volume_metadata_read(disk_path);
    gboolean matches = g_strcmp0(existing_name, logical_name) == 0;
    g_free(existing_name);
    g_free(metadata_path);
    g_free(contents);
    g_free(escaped);
    return matches;
  }
  gsize offset = 0;
  gsize content_length = strlen(contents);
  gboolean created = fd >= 0;
  gboolean ok = created;
  while (ok && offset < content_length) {
    ssize_t written = write(fd, contents + offset, content_length - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ok = FALSE;
    } else {
      offset += (gsize)written;
    }
  }
  ok = ok && fsync(fd) == 0;
  if (fd >= 0) {
    close(fd);
  }
  if (!ok && created) {
    g_unlink(metadata_path);
  }
  g_free(metadata_path);
  g_free(contents);
  g_free(escaped);
  return ok;
}

static char *volume_metadata_read(const char *disk_path) {
  char *metadata_path = g_strconcat(disk_path, ".name", NULL);
  int fd = open(metadata_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  g_free(metadata_path);
  if (fd < 0) {
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > 64 * 1024) {
    close(fd);
    return NULL;
  }
  char *contents = g_malloc((gsize)st.st_size + 1);
  ssize_t count = read(fd, contents, (gsize)st.st_size);
  close(fd);
  if (count != st.st_size || contents[count - 1] != '\n') {
    g_free(contents);
    return NULL;
  }
  contents[count - 1] = '\0';
  char *name = g_strcompress(contents);
  g_free(contents);
  return name;
}

static gint volume_info_compare(gconstpointer left, gconstpointer right) {
  const QuockerVolumeInfo *a = *(QuockerVolumeInfo *const *)left;
  const QuockerVolumeInfo *b = *(QuockerVolumeInfo *const *)right;
  return g_strcmp0(a->logical_name, b->logical_name);
}

gboolean quocker_volume_list(const char *project_directory,
                             GPtrArray **volumes_out, GError **error) {
  if (error) {
    *error = NULL;
  }
  if (volumes_out) {
    *volumes_out = NULL;
  }
  if (!project_directory || !g_path_is_absolute(project_directory) ||
      !volumes_out) {
    volume_error(error, "project volume directory is invalid");
    return FALSE;
  }
  GPtrArray *volumes =
      g_ptr_array_new_with_free_func((GDestroyNotify)quocker_volume_info_free);
  char *volume_directory = g_build_filename(project_directory, "volumes", NULL);
  struct stat directory_stat;
  if (g_lstat(volume_directory, &directory_stat) < 0) {
    if (errno == ENOENT) {
      *volumes_out = volumes;
      g_free(volume_directory);
      return TRUE;
    }
    volume_error(error, "could not inspect the project volume directory");
    g_ptr_array_free(volumes, TRUE);
    g_free(volume_directory);
    return FALSE;
  }
  if (!S_ISDIR(directory_stat.st_mode) || (directory_stat.st_mode & 0077) ||
      S_ISLNK(directory_stat.st_mode)) {
    volume_error(error, "project volume directory is unsafe");
    g_ptr_array_free(volumes, TRUE);
    g_free(volume_directory);
    return FALSE;
  }
  GDir *directory = g_dir_open(volume_directory, 0, NULL);
  if (!directory) {
    volume_error(error, "could not read the project volume directory");
    g_ptr_array_free(volumes, TRUE);
    g_free(volume_directory);
    return FALSE;
  }
  const char *entry;
  while ((entry = g_dir_read_name(directory))) {
    if (!g_str_has_suffix(entry, ".ext4")) {
      continue;
    }
    char *disk_path = g_build_filename(volume_directory, entry, NULL);
    struct stat disk_stat;
    if (g_lstat(disk_path, &disk_stat) < 0 || !S_ISREG(disk_stat.st_mode) ||
        !volume_disk_valid(disk_path)) {
      volume_error(error, "project volume directory contains an invalid disk");
      g_free(disk_path);
      g_dir_close(directory);
      g_ptr_array_free(volumes, TRUE);
      g_free(volume_directory);
      return FALSE;
    }
    QuockerVolumeInfo *info = g_new0(QuockerVolumeInfo, 1);
    info->logical_name = volume_metadata_read(disk_path);
    if (!info->logical_name) {
      info->logical_name = g_strdup(entry);
    }
    info->disk_path = disk_path;
    info->size_bytes = (guint64)disk_stat.st_size;
    g_ptr_array_add(volumes, info);
  }
  g_dir_close(directory);
  g_free(volume_directory);
  g_ptr_array_sort(volumes, volume_info_compare);
  *volumes_out = volumes;
  return TRUE;
}

gboolean quocker_volume_disk_prepare(const char *project_directory,
                                     const char *logical_name,
                                     guint64 size_bytes, char **disk_path_out,
                                     GError **error) {
  if (error) {
    *error = NULL;
  }
  if (disk_path_out) {
    *disk_path_out = NULL;
  }
  if (!project_directory || !g_path_is_absolute(project_directory) ||
      !logical_name || !*logical_name || !disk_path_out ||
      size_bytes < 64 * 1024 * 1024 || size_bytes > (1ULL << 40) ||
      size_bytes > G_MAXINT64) {
    volume_error(error, "volume directory, name, or bounded size is invalid");
    return FALSE;
  }
  char *volume_directory = g_build_filename(project_directory, "volumes", NULL);
  if (g_mkdir_with_parents(volume_directory, 0700) < 0) {
    volume_error(error, "could not create the project volume directory");
    g_free(volume_directory);
    return FALSE;
  }
  struct stat directory_stat;
  if (g_lstat(volume_directory, &directory_stat) < 0 ||
      !S_ISDIR(directory_stat.st_mode) ||
      (directory_stat.st_mode & 0077) != 0) {
    volume_error(error, "project volume directory is unsafe");
    g_free(volume_directory);
    return FALSE;
  }
  char *digest =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, logical_name, -1);
  char *disk_path = g_strdup_printf("%s/%s.ext4", volume_directory, digest);
  g_free(digest);
  if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
    if (!volume_disk_valid(disk_path)) {
      volume_error(error, "existing project volume is not a safe ext4 disk");
      g_free(disk_path);
      g_free(volume_directory);
      return FALSE;
    }
    if (!volume_metadata_write(disk_path, logical_name)) {
      volume_error(error, "could not record the volume's logical name");
      g_free(disk_path);
      g_free(volume_directory);
      return FALSE;
    }
    *disk_path_out = disk_path;
    g_free(volume_directory);
    return TRUE;
  }
  char *temporary_path = g_strdup_printf("%s/.volume.XXXXXX", volume_directory);
  int fd = g_mkstemp(temporary_path);
  if (fd < 0 || fchmod(fd, 0600) < 0 || ftruncate(fd, (off_t)size_bytes) < 0) {
    if (fd >= 0) {
      close(fd);
    }
    volume_error(error, "could not allocate a bounded sparse volume disk");
    g_free(temporary_path);
    g_free(disk_path);
    g_free(volume_directory);
    return FALSE;
  }
  close(fd);
  const char *mkfs = g_getenv("QUOCKER_MKE2FS");
  if (!mkfs || !*mkfs) {
    mkfs = "mke2fs";
  }
  const char *arguments[] = {mkfs, "-q", "-t",           "ext4", "-F",
                             "-m", "0",  temporary_path, NULL};
  char *stdout_text = NULL;
  char *stderr_text = NULL;
  gint status = 0;
  GError *spawn_error = NULL;
  gboolean spawned =
      g_spawn_sync(NULL, (char **)arguments, NULL, G_SPAWN_SEARCH_PATH, NULL,
                   NULL, &stdout_text, &stderr_text, &status, &spawn_error);
  gboolean formatted = spawned && g_spawn_check_wait_status(status, NULL) &&
                       volume_disk_valid(temporary_path);
  g_free(stdout_text);
  g_free(stderr_text);
  g_clear_error(&spawn_error);
  if (!formatted) {
    volume_error(error, "mke2fs could not create a valid ext4 volume");
    g_unlink(temporary_path);
    g_free(temporary_path);
    g_free(disk_path);
    g_free(volume_directory);
    return FALSE;
  }
  if (link(temporary_path, disk_path) < 0 && errno != EEXIST) {
    volume_error(error, "could not publish the initialized volume disk");
    g_unlink(temporary_path);
    g_free(temporary_path);
    g_free(disk_path);
    g_free(volume_directory);
    return FALSE;
  }
  g_unlink(temporary_path);
  g_free(temporary_path);
  if (!volume_disk_valid(disk_path)) {
    volume_error(error, "published volume disk failed ext4 validation");
    g_free(disk_path);
    g_free(volume_directory);
    return FALSE;
  }
  if (!volume_metadata_write(disk_path, logical_name)) {
    volume_error(error, "could not record the volume's logical name");
    g_free(disk_path);
    g_free(volume_directory);
    return FALSE;
  }
  *disk_path_out = disk_path;
  g_free(volume_directory);
  return TRUE;
}
