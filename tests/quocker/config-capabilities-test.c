/*
 * Verify the service-field capability report.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    g_printerr("usage: %s QUOCKER CONFIG\n", argv[0]);
    return 2;
  }
  char file_option[] = "-f";
  char config[] = "config";
  char capabilities[] = "--capabilities";
  char *command[] = {argv[1], file_option, argv[2], config, capabilities,
                     NULL};
  char *stdout_text = NULL;
  char *stderr_text = NULL;
  int status = 0;
  GError *error = NULL;
  if (!g_spawn_sync(NULL, command, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                    &stdout_text, &stderr_text, &status, &error)) {
    g_printerr("cannot run Quocker: %s\n", error->message);
    g_clear_error(&error);
    return 1;
  }
  gboolean ok = g_spawn_check_wait_status(status, &error);
  static const char *const expected[] = {
      "local\timage\tVM image source",
      "local\tmem_limit\tQEMU setting",
      "local\tports\tpartially supported; see Quocker field documentation",
      "local\thostname\tunsupported",
      "local\tlabels\tpreserved-only; no runtime behavior implemented",
      "guest\tcommand\tOCI guest workload setting",
      "guest\tworking_dir\tOCI guest workload setting",
      "guest\thealthcheck\tunsupported",
      "RESOURCE\tNAME\tSTATUS",
      "configs\tapp-config\tunsupported; not provisioned into guests",
      "networks\tfrontend\tpartially supported QEMU user networking; "
      "named networks unsupported",
      "secrets\tapp-secret\tunsupported; not provisioned into guests",
      "volumes\tdata\tpartially supported VM disks; bind and tmpfs mounts "
      "unsupported",
      NULL,
  };
  ok = ok && stdout_text;
  for (const char *const *item = expected; ok && *item; item++) {
    ok = g_strstr_len(stdout_text, -1, *item) != NULL;
  }
  if (!ok) {
    g_printerr("unexpected Quocker capability report:\n%s%s\n",
               stdout_text ? stdout_text : "",
               stderr_text ? stderr_text : "");
    if (error) {
      g_printerr("%s\n", error->message);
    }
  }
  g_clear_error(&error);
  g_free(stdout_text);
  g_free(stderr_text);
  return ok ? 0 : 1;
}
