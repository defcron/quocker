/*
 * Quocker OCI registry configuration.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_REGISTRY_CONFIG_H
#define QUOCKER_REGISTRY_CONFIG_H

#include <glib.h>

char *quocker_registry_mirror_resolve(const char *registry,
                                      const char *global_override,
                                      GError **error);
gboolean quocker_registry_mirror_url_valid(const char *url);

#endif
