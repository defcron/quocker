/*
 * OCI image runtime defaults and Compose override handling.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_IMAGE_H
#define QUOCKER_IMAGE_H

#include <glib.h>

typedef struct QuockerImageDefaults {
  GPtrArray *entrypoint;
  GPtrArray *command;
  GHashTable *environment;
  GHashTable *volumes;
  char *working_directory;
  char *user;
  char *kernel_id;
  char *kernel_minimum;
  GPtrArray *kernel_features;
  GPtrArray *kernel_module_releases;
} QuockerImageDefaults;

typedef struct QuockerGuestMount {
  char *target;
  char *volume_name;
  char *disk_path;
  gboolean read_only;
} QuockerGuestMount;

typedef struct QuockerRuntimeConfig {
  GPtrArray *argv;
  GPtrArray *mounts;
  GHashTable *environment;
  char *working_directory;
  char *user;
} QuockerRuntimeConfig;

gboolean quocker_runtime_config_add_mount(QuockerRuntimeConfig *runtime,
                                          const char *target,
                                          const char *volume_name,
                                          gboolean read_only, GError **error);

gboolean quocker_image_defaults_parse(const char *json, gsize length,
                                      QuockerImageDefaults **defaults_out,
                                      GError **error);
void quocker_image_defaults_free(QuockerImageDefaults *defaults);
gboolean quocker_runtime_config_merge(
    const QuockerImageDefaults *defaults, const GPtrArray *entrypoint_override,
    const GPtrArray *command_override, GHashTable *environment_override,
    const char *working_directory_override, const char *user_override,
    QuockerRuntimeConfig **runtime_out, GError **error);
void quocker_runtime_config_free(QuockerRuntimeConfig *runtime);

#endif
