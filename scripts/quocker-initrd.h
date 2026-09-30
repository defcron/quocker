/*
 * Quocker guest initramfs construction.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_INITRD_H
#define QUOCKER_INITRD_H

#include "quocker-image.h"

#include <glib.h>

gboolean quocker_guest_config_encode(const QuockerRuntimeConfig *runtime,
                                     GByteArray **config_out, GError **error);
gboolean quocker_initrd_build(const char *base_initrd_path,
                              const char *base_initrd_digest,
                              const char *guest_init_path,
                              const char *guest_architecture,
                              const QuockerRuntimeConfig *runtime,
                              const char *output_path, GError **error);

#endif
