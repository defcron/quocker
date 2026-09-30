/*
 * Quocker OCI registry support.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_OCI_H
#define QUOCKER_OCI_H

#include <glib.h>

#include "quocker-image.h"

typedef struct QuockerOciImage {
  char *rootfs_path;
  char *os;
  char *architecture;
  char *manifest_digest;
  char *config_digest;
  QuockerImageDefaults *defaults;
} QuockerOciImage;

gboolean quocker_oci_pull(const char *reference, const char *platform,
                          const char *cache_directory, const char *mirror_url,
                          QuockerOciImage **image_out);
void quocker_oci_image_free(QuockerOciImage *image);
gboolean quocker_oci_cache_prune(const char *cache_directory);
gboolean quocker_oci_cache_reference(const char *cache_directory,
                                    const char *state_directory,
                                    const char *service,
                                    const char *overlay_path,
                                    const char *base_disk_path);
/* The caller must hold a cache lock obtained with quocker_oci_cache_lock(). */
gboolean quocker_oci_cache_reference_locked(const char *cache_directory,
                                            const char *state_directory,
                                            const char *service,
                                            const char *overlay_path,
                                            const char *base_disk_path);
gboolean quocker_oci_cache_unreference(const char *cache_directory,
                                       const char *state_directory,
                                       const char *service);
gboolean quocker_oci_cache_has_room(const char *cache_directory,
                                    guint64 incoming_bytes);
int quocker_oci_cache_lock(const char *cache_directory);
void quocker_oci_cache_unlock(int lock_fd);

#endif
