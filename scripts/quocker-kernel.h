/*
 * Quocker guest kernel catalog selection.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_KERNEL_H
#define QUOCKER_KERNEL_H

#include "quocker-rootfs.h"

typedef struct QuockerKernel {
  char *id;
  char *os;
  char *architecture;
  char *variant;
  char *version;
  GPtrArray *features;
  GPtrArray *module_releases;
  char *kernel_path;
  char *kernel_url;
  guint64 kernel_size;
  char *kernel_digest;
  char *initrd_path;
  char *initrd_url;
  guint64 initrd_size;
  char *initrd_digest;
  char *reason;
  gboolean module_release_evidence;
  gboolean module_release_match;
  gboolean assets_verified;
  GPtrArray *observed_module_releases;
} QuockerKernel;

gboolean quocker_kernel_select(const char *catalog_path,
                               const char *public_key_path,
                               const char *platform,
                               const QuockerDistroInfo *distro,
                               const char *requested_id,
                               QuockerKernel **kernel_out, GError **error);
gboolean quocker_kernel_select_for_image(
    const char *catalog_path, const char *public_key_path, const char *platform,
    const QuockerDistroInfo *distro, const char *requested_id,
    const char *minimum_version, const GPtrArray *required_features,
    const GPtrArray *required_module_releases,
    QuockerKernel **kernel_out, GError **error);
void quocker_kernel_free(QuockerKernel *kernel);
gboolean quocker_kernel_verify_assets(const QuockerKernel *kernel,
                                      GError **error);
gboolean quocker_kernel_fetch_assets(QuockerKernel *kernel,
                                     GError **error);
gboolean quocker_kernel_catalog_update(const char *url,
                                      const char *catalog_path,
                                      const char *public_key_path,
                                      GError **error);
gboolean quocker_kernel_catalog_install_signed(
    const gchar *catalog, gsize catalog_size, const gchar *signature,
    gsize signature_size, const char *catalog_path,
    const char *public_key_path, GError **error);

#endif
