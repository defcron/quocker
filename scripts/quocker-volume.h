/*
 * Project-scoped persistent VM volume disks.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_VOLUME_H
#define QUOCKER_VOLUME_H

#include <glib.h>

gboolean quocker_volume_disk_prepare(const char *project_directory,
                                     const char *logical_name,
                                     guint64 size_bytes, char **disk_path_out,
                                     GError **error);

#endif
