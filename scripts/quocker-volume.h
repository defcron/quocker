/*
 * Project-scoped persistent VM volume disks.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_VOLUME_H
#define QUOCKER_VOLUME_H

#include <glib.h>

typedef struct QuockerVolumeInfo {
  char *logical_name;
  char *disk_path;
  guint64 size_bytes;
  guint64 allocated_bytes;
} QuockerVolumeInfo;

void quocker_volume_info_free(QuockerVolumeInfo *info);

gboolean quocker_volume_disk_prepare(const char *project_directory,
                                     const char *logical_name,
                                     guint64 size_bytes, char **disk_path_out,
                                     GError **error);

gboolean quocker_volume_list(const char *project_directory,
                             GPtrArray **volumes_out, GError **error);

gboolean quocker_volume_remove(const char *project_directory,
                               const char *logical_name, GError **error);

gboolean quocker_volume_project_usage(const char *project_directory,
                                      guint64 *virtual_bytes,
                                      guint64 *allocated_bytes,
                                      guint64 *quota_bytes, GError **error);

#endif
