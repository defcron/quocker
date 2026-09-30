/*
 * Quocker guest lifecycle serial protocol.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_GUEST_STATUS_H
#define QUOCKER_GUEST_STATUS_H

#include <glib.h>

gboolean quocker_guest_status_has_ready(const char *contents, gsize length);
gboolean quocker_guest_status_parse_exit(const char *contents, gsize length,
                                         int *status_out, GError **error);

#endif
