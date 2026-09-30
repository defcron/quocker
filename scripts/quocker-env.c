/*
 * Quocker Compose environment-file parsing.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-env.h"

typedef enum QuockerEnvError {
  QUOCKER_ENV_ERROR_IO,
  QUOCKER_ENV_ERROR_SYNTAX,
  QUOCKER_ENV_ERROR_INTERPOLATION,
} QuockerEnvError;

GQuark quocker_env_error_quark(void) {
  return g_quark_from_static_string("quocker-env-error");
}

gboolean quocker_env_name_valid(const char *name) {
  if (!name || !(g_ascii_isalpha(*name) || *name == '_')) {
    return FALSE;
  }
  for (const char *p = name + 1; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static char *parse_value(const char *raw, gboolean raw_format,
                         gboolean *single_quoted, const char *path,
                         guint line_number, GError **error) {
  *single_quoted = FALSE;
  if (raw_format) {
    return g_strdup(raw);
  }
  if (*raw != '\'' && *raw != '"') {
    char *value = g_strdup(raw);
    char *comment = strstr(value, " #");
    if (comment) {
      *comment = '\0';
    }
    g_strstrip(value);
    return value;
  }

  char quote = *raw;
  GString *value = g_string_new(NULL);
  const char *p = raw + 1;
  gboolean closed = FALSE;
  while (*p) {
    if (quote == '"' && *p == '\\' && p[1]) {
      p++;
      switch (*p) {
      case 'n':
        g_string_append_c(value, '\n');
        break;
      case 'r':
        g_string_append_c(value, '\r');
        break;
      case 't':
        g_string_append_c(value, '\t');
        break;
      case '\\':
      case '"':
        g_string_append_c(value, *p);
        break;
      default:
        g_string_append_c(value, '\\');
        g_string_append_c(value, *p);
        break;
      }
      p++;
      continue;
    }
    if (quote == '\'' && *p == '\\' && p[1] == '\'') {
      g_string_append_c(value, '\'');
      p += 2;
      continue;
    }
    if (*p == quote) {
      closed = TRUE;
      p++;
      break;
    }
    g_string_append_c(value, *p++);
  }
  if (!closed) {
    g_set_error(error, quocker_env_error_quark(), QUOCKER_ENV_ERROR_SYNTAX,
                "environment file %s:%u has an unterminated quoted value",
                path, line_number);
    g_string_free(value, TRUE);
    return NULL;
  }
  while (g_ascii_isspace(*p)) {
    p++;
  }
  if (*p && *p != '#') {
    g_set_error(error, quocker_env_error_quark(), QUOCKER_ENV_ERROR_SYNTAX,
                "environment file %s:%u has unexpected text after a quoted "
                "value",
                path, line_number);
    g_string_free(value, TRUE);
    return NULL;
  }
  *single_quoted = quote == '\'';
  return g_string_free(value, FALSE);
}

gboolean quocker_env_file_read(
    GHashTable *destination, const char *path, gboolean optional,
    GHashTable *lookup_environment, gboolean preserve_host_environment,
    gboolean interpolate, gboolean raw_format,
    QuockerEnvInterpolateFunc interpolate_func, GError **error) {
  if (!destination || !path || !lookup_environment ||
      (interpolate && !raw_format && !interpolate_func)) {
    g_set_error(error, quocker_env_error_quark(), QUOCKER_ENV_ERROR_SYNTAX,
                "environment file parser received invalid arguments");
    return FALSE;
  }

  gchar *contents = NULL;
  GError *io_error = NULL;
  if (!g_file_get_contents(path, &contents, NULL, &io_error)) {
    if (optional &&
        g_error_matches(io_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&io_error);
      return TRUE;
    }
    g_set_error(error, quocker_env_error_quark(), QUOCKER_ENV_ERROR_IO,
                "cannot read environment file %s: %s", path,
                io_error->message);
    g_error_free(io_error);
    return FALSE;
  }

  gchar **lines = g_strsplit(contents, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    char *line = g_strstrip(lines[i]);
    if (!*line || *line == '#') {
      continue;
    }
    if (g_str_has_prefix(line, "export ")) {
      line = g_strstrip(line + 7);
    }
    char *equals = strchr(line, '=');
    char *key = line;
    const char *raw_value = NULL;
    if (equals) {
      *equals = '\0';
      raw_value = g_strstrip(equals + 1);
    }
    key = g_strstrip(key);
    if (!quocker_env_name_valid(key)) {
      g_set_error(error, quocker_env_error_quark(), QUOCKER_ENV_ERROR_SYNTAX,
                  "environment file %s:%u has an invalid variable name", path,
                  i + 1);
      g_strfreev(lines);
      g_free(contents);
      return FALSE;
    }
    if (preserve_host_environment && g_getenv(key)) {
      continue;
    }
    if (!raw_value) {
      if (destination != lookup_environment) {
        g_hash_table_replace(destination, g_strdup(key), NULL);
        g_hash_table_remove(lookup_environment, key);
      } else {
        g_hash_table_remove(destination, key);
      }
      continue;
    }

    gboolean single_quoted = FALSE;
    char *value = parse_value(raw_value, raw_format, &single_quoted, path,
                              i + 1, error);
    if (!value) {
      g_strfreev(lines);
      g_free(contents);
      return FALSE;
    }
    if (interpolate && !raw_format && !single_quoted) {
      char *expanded = interpolate_func(value, lookup_environment);
      g_free(value);
      value = expanded;
      if (!value) {
        g_set_error(error, quocker_env_error_quark(),
                    QUOCKER_ENV_ERROR_INTERPOLATION,
                    "cannot interpolate environment file %s:%u", path,
                    i + 1);
        g_strfreev(lines);
        g_free(contents);
        return FALSE;
      }
    }
    g_hash_table_replace(destination, g_strdup(key), g_strdup(value));
    if (destination != lookup_environment) {
      g_hash_table_replace(lookup_environment, g_strdup(key), g_strdup(value));
    }
    g_free(value);
  }
  g_strfreev(lines);
  g_free(contents);
  return TRUE;
}
