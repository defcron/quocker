/*
 * Verify that Compose schema failures identify their YAML source location.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    g_printerr("usage: %s QUOCKER CONFIG... EXPECTED-DIAGNOSTIC\n", argv[0]);
    return 2;
  }
  char option[] = "-f";
  char config[] = "config";
  char quiet[] = "--quiet";
  guint config_count = argc - 3;
  char **command = g_new0(char *, 2 * config_count + 4);
  guint command_index = 0;
  command[command_index++] = argv[1];
  for (guint i = 0; i < config_count; i++) {
    command[command_index++] = option;
    command[command_index++] = argv[2 + i];
  }
  command[command_index++] = config;
  command[command_index++] = quiet;
  char *stdout_text = NULL;
  char *stderr_text = NULL;
  int status = 0;
  GError *error = NULL;
  if (!g_spawn_sync(NULL, command, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                    &stdout_text, &stderr_text, &status, &error)) {
    g_printerr("cannot run Quocker: %s\n", error->message);
    g_clear_error(&error);
    g_free(command);
    return 1;
  }
  gboolean failed = !g_spawn_check_wait_status(status, &error);
  if (!failed || !stderr_text ||
      !g_strstr_len(stderr_text, -1, argv[argc - 1])) {
    g_printerr("Quocker did not produce expected schema diagnostic:\n%s%s\n",
               stderr_text ? stderr_text : "", error ? error->message : "");
    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    g_free(command);
    return 1;
  }
  g_clear_error(&error);
  g_free(stdout_text);
  g_free(stderr_text);
  g_free(command);
  return 0;
}
