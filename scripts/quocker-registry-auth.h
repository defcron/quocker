/*
 * Docker CLI config authentication reader.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_REGISTRY_AUTH_H
#define QUOCKER_REGISTRY_AUTH_H

#include <glib.h>

typedef struct QuockerRegistryCredentials {
  char *username;
  char *secret;
} QuockerRegistryCredentials;

gboolean quocker_registry_credentials_load(
    const char *registry, QuockerRegistryCredentials **credentials_out,
    GError **error);
void quocker_registry_credentials_free(QuockerRegistryCredentials *credentials);

#endif
