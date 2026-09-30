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
  *disk_path_out = disk_path;
  g_free(volume_directory);
  return TRUE;
}
