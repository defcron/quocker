/*
 * Quocker guest lifecycle serial protocol.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-guest-status.h"

#include <string.h>

static GQuark guest_status_error_quark(void) {
  return g_quark_from_static_string("quocker-guest-status-error");
}

static gboolean line_has_prefix(const char *line, gsize length,
                                const char *prefix) {
  gsize prefix_length = strlen(prefix);
  return length >= prefix_length && memcmp(line, prefix, prefix_length) == 0;
}

static gsize line_content_length(const char *line, gsize length) {
  return length > 0 && line[length - 1] == '\r' ? length - 1 : length;
}

gboolean quocker_guest_status_has_ready(const char *contents, gsize length) {
  if (!contents || length == 0) {
    return FALSE;
  }
  const char *line = contents;
  const char *limit = contents + length;
  while (line < limit) {
    const char *end = memchr(line, '\n', limit - line);
    gsize line_length = line_content_length(
        line, end ? (gsize)(end - line) : (gsize)(limit - line));
    if (line_has_prefix(line, line_length, "QUOCKER_READY pid=")) {
      const char *pid = line + strlen("QUOCKER_READY pid=");
      const char *line_end = line + line_length;
      guint64 pid_number = 0;
      gboolean valid = pid < line_end;
      for (const char *digit = pid; valid && digit < line_end; digit++) {
        if (!g_ascii_isdigit(*digit)) {
          valid = FALSE;
          break;
        }
        guint value = (guint)(*digit - '0');
        if (pid_number > (G_MAXINT - value) / 10) {
          valid = FALSE;
          break;
        }
        pid_number = pid_number * 10 + value;
      }
      valid = valid && pid_number > 0;
      if (valid) {
        return TRUE;
      }
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  return FALSE;
}

gboolean quocker_guest_status_parse_exit(const char *contents, gsize length,
                                         int *status_out, GError **error) {
  if (!contents) {
    contents = "";
    length = 0;
  }
  const char *line = contents;
  const char *limit = contents + length;
  const char *last_marker = NULL;
  gsize last_marker_length = 0;
  while (line < limit) {
    const char *end = memchr(line, '\n', limit - line);
    gsize line_length = line_content_length(
        line, end ? (gsize)(end - line) : (gsize)(limit - line));
    if (line_has_prefix(line, line_length, "QUOCKER_EXIT ")) {
      last_marker = line;
      last_marker_length = line_length;
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  if (!last_marker) {
    g_set_error_literal(error, guest_status_error_quark(), 1,
                        "no Quocker guest completion status was recorded");
    return FALSE;
  }
  const char prefix[] = "QUOCKER_EXIT status=";
  if (!line_has_prefix(last_marker, last_marker_length, prefix)) {
    g_set_error_literal(error, guest_status_error_quark(), 2,
                        "guest did not exit normally");
    return FALSE;
  }
  const char *value = last_marker + sizeof(prefix) - 1;
  const char *end_line = last_marker + last_marker_length;
  guint status = 0;
  gboolean valid = value < end_line;
  for (const char *digit = value; valid && digit < end_line; digit++) {
    if (!g_ascii_isdigit(*digit)) {
      valid = FALSE;
      break;
    }
    guint value_digit = (guint)(*digit - '0');
    if (status > (255 - value_digit) / 10) {
      valid = FALSE;
      break;
    }
    status = status * 10 + value_digit;
  }
  if (valid && status_out) {
    *status_out = (int)status;
  }
  if (!valid) {
    g_set_error_literal(error, guest_status_error_quark(), 3,
                        "guest completion status is invalid");
  }
  return valid;
}

gboolean quocker_guest_status_parse_wait_code(const char *contents,
                                              gsize length, int *code_out,
                                              gboolean *found_out,
                                              GError **error) {
  if (found_out) {
    *found_out = FALSE;
  }
  if (!contents) {
    contents = "";
    length = 0;
  }
  const char *line = contents;
  const char *limit = contents + length;
  const char *last_marker = NULL;
  gsize last_marker_length = 0;
  while (line < limit) {
    const char *end = memchr(line, '\n', limit - line);
    gsize line_length = line_content_length(
        line, end ? (gsize)(end - line) : (gsize)(limit - line));
    if (line_has_prefix(line, line_length, "QUOCKER_EXIT ")) {
      last_marker = line;
      last_marker_length = line_length;
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  if (!last_marker) {
    return TRUE;
  }
  const char status_prefix[] = "QUOCKER_EXIT status=";
  if (line_has_prefix(last_marker, last_marker_length, status_prefix)) {
    int status = 0;
    if (!quocker_guest_status_parse_exit(last_marker, last_marker_length,
                                         &status, error)) {
      return FALSE;
    }
    if (code_out) {
      *code_out = status;
    }
    if (found_out) {
      *found_out = TRUE;
    }
    return TRUE;
  }
  const char signal_prefix[] = "QUOCKER_EXIT signal=";
  if (!line_has_prefix(last_marker, last_marker_length, signal_prefix)) {
    g_set_error_literal(error, guest_status_error_quark(), 2,
                        "guest did not exit normally");
    return FALSE;
  }
  const char *value = last_marker + sizeof(signal_prefix) - 1;
  const char *end_line = last_marker + last_marker_length;
  guint signal_number = 0;
  gboolean valid = value < end_line;
  for (const char *digit = value; valid && digit < end_line; digit++) {
    if (!g_ascii_isdigit(*digit)) {
      valid = FALSE;
      break;
    }
    guint value_digit = (guint)(*digit - '0');
    if (signal_number > (64 - value_digit) / 10) {
      valid = FALSE;
      break;
    }
    signal_number = signal_number * 10 + value_digit;
  }
  valid = valid && signal_number > 0 && signal_number <= 64;
  if (!valid) {
    g_set_error_literal(error, guest_status_error_quark(), 4,
                        "guest termination signal is invalid");
    return FALSE;
  }
  if (code_out) {
    *code_out = 128 + (int)signal_number;
  }
  if (found_out) {
    *found_out = TRUE;
  }
  return TRUE;
}
