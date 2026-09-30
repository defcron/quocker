/*
 * Quocker OCI root filesystem materialization.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_ROOTFS_H
#define QUOCKER_ROOTFS_H

#include <glib.h>

typedef struct QuockerDistroInfo {
  char *id;
  char *id_like;
  char *version_id;
  GPtrArray *kernel_module_releases;
} QuockerDistroInfo;

gboolean quocker_rootfs_materialize(const char *manifest_digest,
                                    GPtrArray *layer_paths,
                                    const char *cache_directory,
                                    char **rootfs_path_out);
gboolean quocker_rootfs_cache_prune(const char *cache_directory,
                                    guint64 *removed_bytes_out);
gboolean quocker_rootfs_cache_prune_except(const char *cache_directory,
                                          GHashTable *keep_files,
                                          guint64 *removed_bytes_out);
gboolean quocker_rootfs_detect_distro(const char *rootfs_path,
                                      QuockerDistroInfo *info);
void quocker_distro_info_clear(QuockerDistroInfo *info);

#endif
