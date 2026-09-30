/*
 * Quocker Compose environment-file parsing.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_ENV_H
#define QUOCKER_ENV_H

#include <glib.h>

typedef char *(*QuockerEnvInterpolateFunc)(const char *text,
                                           GHashTable *environment);

GQuark quocker_env_error_quark(void);

gboolean quocker_env_name_valid(const char *name);
gboolean quocker_env_file_read(
    GHashTable *destination, const char *path, gboolean optional,
    GHashTable *lookup_environment, gboolean preserve_host_environment,
    gboolean interpolate, gboolean raw_format,
    QuockerEnvInterpolateFunc interpolate_func, GError **error);

#endif
