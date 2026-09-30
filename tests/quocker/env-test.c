/*
 * Quocker Compose environment-file behavior tests.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean contains(const char *output, const char *expected) {
  if (strstr(output, expected)) {
    return TRUE;
  }
  g_printerr("expected config output to contain: %s\n", expected);
  return FALSE;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    g_printerr("usage: quocker-env-test COMPOSE BASE_ENV OVERRIDE_ENV\n");
    return 2;
  }
  const char *cli = g_getenv("QUOCKER_CLI");
  if (!cli || !*cli) {
    g_printerr("QUOCKER_CLI is required\n");
    return 2;
  }
  g_setenv("SHARED", "host", TRUE);
  g_unsetenv("BASE");
  g_unsetenv("VALUE");
  g_unsetenv("LATER");
  g_unsetenv("SINGLE");
  g_unsetenv("GONE");
  g_unsetenv("COMMENTED");
  g_unsetenv("HASHED");
  g_unsetenv("ESCAPED");
  g_unsetenv("QUOCKER_ENV_MISSING");
  char *arguments[] = {
      (char *)cli, (char *)"-f",         argv[1], (char *)"--env-file",
      argv[2],     (char *)"--env-file", argv[3], (char *)"config",
      NULL};
  char *stdout_text = NULL;
  char *stderr_text = NULL;
  int status = 0;
  GError *error = NULL;
  if (!g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                    &stdout_text, &stderr_text, &status, &error)) {
    g_printerr("cannot run Quocker CLI: %s\n", error->message);
    g_error_free(error);
    return 1;
  }
  gboolean ok = g_spawn_check_wait_status(status, &error);
  if (!ok) {
    g_printerr("Quocker config failed: %s\n%s", error->message,
               stderr_text ? stderr_text : "");
    g_clear_error(&error);
  }
  if (ok) {
    ok = contains(stdout_text, "\"image\": \"alpine:host\"") &&
         contains(stdout_text, "\"VALUE\": \"3.20-suffix\"") &&
         contains(stdout_text, "\"LATER\": \"second\"") &&
         contains(stdout_text,
                  "\"SINGLE\": \"${QUOCKER_ENV_MISSING:?single-quoted "
                  "values are literal}\"") &&
         contains(stdout_text, "\"GONE\": \"removed\"") &&
         contains(stdout_text, "\"COMMENTED\": \"visible\"") &&
         contains(stdout_text, "\"HASHED\": \"literal#hash\"") &&
         contains(stdout_text, "\"ESCAPED\": \"line\\nnext\"");
    if (!ok) {
      g_printerr("actual config output:\n%s", stdout_text ? stdout_text : "");
    }
  }
  g_free(stdout_text);
  g_free(stderr_text);
  return ok ? 0 : 1;
}
