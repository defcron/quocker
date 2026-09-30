/*
 * OCI root filesystem to raw ext4 guest disk conversion.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_DISK_H
#define QUOCKER_DISK_H

#include <glib.h>

gboolean quocker_rootfs_to_ext4(const char *rootfs_path, const char *disk_path,
                                GError **error);
gboolean quocker_rootfs_ext4_estimate(const char *rootfs_path,
                                      guint64 *size_out, GError **error);

#endif
