/*
 * Quocker - a Compose-style lifecycle interface for QEMU virtual machines.
 *
 * Copyright (C) 2026 The Quocker Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 */

#include "quocker-disk.h"
#include "quocker-compose-schema.h"
#include "quocker-env.h"
#include "quocker-initrd.h"
#include "quocker-kernel.h"
#include "quocker-oci.h"
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml.h>

typedef enum NodeKind {
  NODE_SCALAR,
  NODE_SEQUENCE,
  NODE_MAPPING,
} NodeKind;

typedef struct YNode YNode;
typedef struct YPair YPair;

struct YPair {
  YNode *key;
  YNode *value;
};

struct YNode {
  NodeKind kind;
  char *tag;
  char *scalar;
  yaml_scalar_style_t scalar_style;
  GPtrArray *items;
};

typedef struct Options {
  GPtrArray *files;
  GPtrArray *env_files;
  GPtrArray *profiles;
  char *project_name;
  char *project_directory;
  char *command;
  char *config_format;
  char *config_list_mode;
  char *port_spec;
  gboolean detach;
  gboolean dry_run;
  int signal_number;
  gboolean remove_volumes;
  gboolean follow;
  gboolean quiet;
  GPtrArray *services;
} Options;

static const char *compose_names[] = {
    "compose.yaml",
    "compose.yml",
    "docker-compose.yaml",
    "docker-compose.yml",
    "quocker-compose.yaml",
    "quocker-compose.yml",
    NULL,
};

static const char *TAG_STR = "tag:yaml.org,2002:str";
static const char *TAG_MAP = "tag:yaml.org,2002:map";
static const char *TAG_SEQ = "tag:yaml.org,2002:seq";
static const char *TAG_NULL = "tag:yaml.org,2002:null";

static char *user_kernel_catalog_path(void) {
  return g_build_filename(g_get_user_cache_dir(), "quocker", "kernels.json",
                          NULL);
}

static const char *default_kernel_catalog_path(void) {
  static char *user_catalog;
  if (!user_catalog) {
    user_catalog = user_kernel_catalog_path();
  }
  return g_file_test(user_catalog, G_FILE_TEST_IS_REGULAR)
             ? user_catalog
             : "/etc/quocker/kernels.json";
}

static void fail(const char *format, ...) G_GNUC_PRINTF(1, 2);

static void fail(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  g_printerr("quocker: ");
  vfprintf(stderr, format, ap);
  g_printerr("\n");
  va_end(ap);
}

static YNode *node_new(NodeKind kind, const char *tag) {
  YNode *node = g_new0(YNode, 1);
  node->kind = kind;
  node->tag = g_strdup(tag ? tag : TAG_STR);
  node->scalar_style = YAML_PLAIN_SCALAR_STYLE;
  if (kind != NODE_SCALAR) {
    node->items = g_ptr_array_new();
  }
  return node;
}

static void node_free(YNode *node) {
  if (!node) {
    return;
  }
  if (node->kind == NODE_MAPPING) {
    for (guint i = 0; i < node->items->len; i++) {
      YPair *pair = g_ptr_array_index(node->items, i);
      node_free(pair->key);
      node_free(pair->value);
      g_free(pair);
    }
  } else if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      node_free(g_ptr_array_index(node->items, i));
    }
  }
  if (node->items) {
    g_ptr_array_free(node->items, TRUE);
  }
  g_free(node->tag);
  g_free(node->scalar);
  g_free(node);
}

static YNode *node_clone(const YNode *src) {
  if (!src) {
    return NULL;
  }
  YNode *dst = node_new(src->kind, src->tag);
  if (src->kind == NODE_SCALAR) {
    dst->scalar = g_strdup(src->scalar);
    dst->scalar_style = src->scalar_style;
  } else if (src->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < src->items->len; i++) {
      g_ptr_array_add(dst->items, node_clone(g_ptr_array_index(src->items, i)));
    }
  } else {
    for (guint i = 0; i < src->items->len; i++) {
      YPair *old = g_ptr_array_index(src->items, i);
      YPair *pair = g_new0(YPair, 1);
      pair->key = node_clone(old->key);
      pair->value = node_clone(old->value);
      g_ptr_array_add(dst->items, pair);
    }
  }
  return dst;
}

static const char *node_string(const YNode *node) {
  return node && node->kind == NODE_SCALAR ? node->scalar : NULL;
}

static YNode *map_get(const YNode *map, const char *key) {
  if (!map || map->kind != NODE_MAPPING) {
    return NULL;
  }
  for (guint i = 0; i < map->items->len; i++) {
    YPair *pair = g_ptr_array_index(map->items, i);
    const char *name = node_string(pair->key);
    if (name && g_str_equal(name, key)) {
      return pair->value;
    }
  }
  return NULL;
}

static gboolean valid_kernel_module_release(const char *release) {
  if (!release || !*release || strlen(release) > 128) {
    return FALSE;
  }
  for (const char *p = release; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '+' ||
          *p == '-')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean append_kernel_module_releases(GPtrArray *releases,
                                              YNode *node,
                                              const char *service_name) {
  GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      const char *release = node_string(g_ptr_array_index(node->items, i));
      if (!release) {
        g_ptr_array_free(items, TRUE);
        fail("service '%s': kernel module releases must be strings",
             service_name);
        return FALSE;
      }
      g_ptr_array_add(items, g_strdup(release));
    }
  } else if (node->kind == NODE_SCALAR) {
    gchar **tokens = g_strsplit(node->scalar, ",", -1);
    for (guint i = 0; tokens[i]; i++) {
      g_ptr_array_add(items, g_strdup(g_strstrip(tokens[i])));
    }
    g_strfreev(tokens);
  } else {
    g_ptr_array_free(items, TRUE);
    fail("service '%s': kernel module releases must be a string or sequence",
         service_name);
    return FALSE;
  }
  for (guint i = 0; i < items->len; i++) {
    const char *release = g_ptr_array_index(items, i);
    if (!valid_kernel_module_release(release)) {
      fail("service '%s': invalid kernel module release '%s'", service_name,
           release);
      g_ptr_array_free(items, TRUE);
      return FALSE;
    }
    gboolean duplicate = FALSE;
    for (guint j = 0; j < releases->len; j++) {
      duplicate |= g_str_equal(release, g_ptr_array_index(releases, j));
    }
    if (!duplicate) {
      g_ptr_array_add(releases, g_strdup(release));
    }
  }
  g_ptr_array_free(items, TRUE);
  return TRUE;
}

static void map_set(YNode *map, const char *key, YNode *value) {
  g_assert(map && map->kind == NODE_MAPPING);
  for (guint i = 0; i < map->items->len; i++) {
    YPair *pair = g_ptr_array_index(map->items, i);
    if (g_strcmp0(node_string(pair->key), key) == 0) {
      node_free(pair->value);
      pair->value = value;
      return;
    }
  }
  YPair *pair = g_new0(YPair, 1);
  pair->key = node_new(NODE_SCALAR, TAG_STR);
  pair->key->scalar = g_strdup(key);
  pair->value = value;
  g_ptr_array_add(map->items, pair);
}

static gboolean yaml_node_convert(yaml_document_t *document,
                                  yaml_node_t *source, YNode **result,
                                  guint depth, char **problem);

static void yaml_merge_mapping_defaults(YNode *defaults,
                                        const YNode *mapping) {
  for (guint i = 0; i < mapping->items->len; i++) {
    YPair *pair = g_ptr_array_index(mapping->items, i);
    const char *key = node_string(pair->key);
    if (key && !map_get(defaults, key)) {
      map_set(defaults, key, node_clone(pair->value));
    }
  }
}

static gboolean yaml_node_convert(yaml_document_t *document,
                                  yaml_node_t *source, YNode **result,
                                  guint depth, char **problem) {
  if (depth > 256) {
    if (problem && !*problem) {
      *problem = g_strdup("YAML aliases exceed the maximum nesting depth");
    }
    return FALSE;
  }
  YNode *node;
  if (source->type == YAML_SCALAR_NODE) {
    node = node_new(NODE_SCALAR, (const char *)source->tag);
    node->scalar = g_strndup((const char *)source->data.scalar.value,
                             source->data.scalar.length);
    node->scalar_style = source->data.scalar.style;
  } else if (source->type == YAML_SEQUENCE_NODE) {
    node = node_new(NODE_SEQUENCE, (const char *)source->tag);
    for (yaml_node_item_t *item = source->data.sequence.items.start;
         item < source->data.sequence.items.top; item++) {
      yaml_node_t *child = yaml_document_get_node(document, *item);
      YNode *converted = NULL;
      if (!yaml_node_convert(document, child, &converted, depth + 1, problem)) {
        node_free(node);
        return FALSE;
      }
      g_ptr_array_add(node->items, converted);
    }
  } else if (source->type == YAML_MAPPING_NODE) {
    node = node_new(NODE_MAPPING, (const char *)source->tag);
    YNode *defaults = node_new(NODE_MAPPING, TAG_MAP);
    for (yaml_node_pair_t *item = source->data.mapping.pairs.start;
         item < source->data.mapping.pairs.top; item++) {
      yaml_node_t *key = yaml_document_get_node(document, item->key);
      yaml_node_t *value = yaml_document_get_node(document, item->value);
      if (key->type == YAML_SCALAR_NODE &&
          key->data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
          key->data.scalar.length == 2 &&
          memcmp(key->data.scalar.value, "<<", 2) == 0) {
        if (value->type == YAML_MAPPING_NODE) {
          YNode *converted = NULL;
          if (!yaml_node_convert(document, value, &converted, depth + 1,
                                 problem)) {
            node_free(defaults);
            node_free(node);
            return FALSE;
          }
          yaml_merge_mapping_defaults(defaults, converted);
          node_free(converted);
        } else if (value->type == YAML_SEQUENCE_NODE) {
          for (yaml_node_item_t *source_mapping =
                   value->data.sequence.items.start;
               source_mapping < value->data.sequence.items.top;
               source_mapping++) {
            yaml_node_t *mapping =
                yaml_document_get_node(document, *source_mapping);
            YNode *converted = NULL;
            if (mapping->type != YAML_MAPPING_NODE ||
                !yaml_node_convert(document, mapping, &converted, depth + 1,
                                   problem)) {
              if (problem && !*problem) {
                *problem = g_strdup("YAML merge sequences must contain mappings");
              }
              node_free(converted);
              node_free(defaults);
              node_free(node);
              return FALSE;
            }
            yaml_merge_mapping_defaults(defaults, converted);
            node_free(converted);
          }
        } else {
          if (problem && !*problem) {
            *problem = g_strdup("YAML merge keys must reference a mapping");
          }
          node_free(defaults);
          node_free(node);
          return FALSE;
        }
        continue;
      }
      YPair *pair = g_new0(YPair, 1);
      if (!yaml_node_convert(document, key, &pair->key, depth + 1, problem) ||
          !yaml_node_convert(document, value, &pair->value, depth + 1,
                             problem)) {
        node_free(pair->key);
        node_free(pair->value);
        g_free(pair);
        node_free(defaults);
        node_free(node);
        return FALSE;
      }
      if (pair->key->kind != NODE_SCALAR) {
        if (problem && !*problem) {
          *problem = g_strdup("Compose mapping keys must be scalar values");
        }
        node_free(pair->key);
        node_free(pair->value);
        g_free(pair);
        node_free(defaults);
        node_free(node);
        return FALSE;
      }
      for (guint i = 0; i < node->items->len; i++) {
        YPair *existing = g_ptr_array_index(node->items, i);
        if (g_strcmp0(node_string(existing->key), node_string(pair->key)) ==
            0) {
          if (problem && !*problem) {
            *problem = g_strdup_printf("duplicate YAML mapping key '%s'",
                                       node_string(pair->key));
          }
          node_free(pair->key);
            node_free(pair->value);
            g_free(pair);
            node_free(defaults);
            node_free(node);
            return FALSE;
        }
      }
      g_ptr_array_add(node->items, pair);
    }
    for (guint i = 0; i < defaults->items->len; i++) {
      YPair *pair = g_ptr_array_index(defaults->items, i);
      const char *key = node_string(pair->key);
      if (key && !map_get(node, key)) {
        map_set(node, key, node_clone(pair->value));
      }
    }
    node_free(defaults);
  } else {
    return FALSE;
  }
  *result = node;
  return TRUE;
}

static const char *interpolation_end(const char *text) {
  guint nested = 0;
  for (const char *p = text; *p; p++) {
    if (p[0] == '$' && p[1] == '{') {
      nested++;
      p++;
    } else if (*p == '}') {
      if (nested == 0) {
        return p;
      }
      nested--;
    }
  }
  return NULL;
}

static char *interpolate_text_depth(const char *text, GHashTable *environment,
                                    guint depth) {
  if (depth > 64) {
    fail("Compose interpolation exceeded the maximum nesting depth");
    return NULL;
  }
  GString *out = g_string_new(NULL);
  for (const char *p = text; *p;) {
    if (*p != '$') {
      g_string_append_c(out, *p++);
      continue;
    }
    if (p[1] == '$') {
      g_string_append_c(out, '$');
      p += 2;
      continue;
    }
    const char *name_start;
    const char *name_end;
    const char *end;
    const char *operator = NULL;
    const char *operand = NULL;
    gboolean braced = p[1] == '{';
    if (braced) {
      name_start = p + 2;
      if (!(g_ascii_isalpha(*name_start) || *name_start == '_')) {
        g_string_append_c(out, *p++);
        continue;
      }
      name_end = name_start;
      while (g_ascii_isalnum(*name_end) || *name_end == '_') {
        name_end++;
      }
      end = interpolation_end(name_end);
      if (!end) {
        g_string_append_c(out, *p++);
        continue;
      }
      if (name_end[0] == ':' && strchr("-?+", name_end[1])) {
        operator = name_end;
        operand = name_end + 2;
      } else if (strchr("-?+", name_end[0])) {
        operator = name_end;
        operand = name_end + 1;
      }
    } else {
      name_start = p + 1;
      name_end = name_start;
      if (!(g_ascii_isalpha(*name_end) || *name_end == '_')) {
        g_string_append_c(out, *p++);
        continue;
      }
      while (g_ascii_isalnum(*name_end) || *name_end == '_') {
        name_end++;
      }
      end = name_end;
    }
    char *name = g_strndup(name_start, name_end - name_start);
    const char *value = g_hash_table_lookup(environment, name);
    gboolean set = value != NULL;
    gboolean nonempty = set && *value;
    if (operator) {
      gboolean colon = operator[0] == ':';
      char op = operator[colon ? 1 : 0];
      gboolean use_operand = FALSE;
      if (op == '-' && (colon ? !nonempty : !set)) {
        use_operand = TRUE;
      } else if (op == '+' && (colon ? nonempty : set)) {
        use_operand = TRUE;
      } else if (op == '?' && (colon ? !nonempty : !set)) {
        char *raw_message = g_strndup(operand, end - operand);
        char *message =
            interpolate_text_depth(raw_message, environment, depth + 1);
        if (message) {
          fail("Compose interpolation error: %s", *message ? message : name);
        }
        g_free(raw_message);
        g_free(message);
        g_free(name);
        g_string_free(out, TRUE);
        return NULL;
      }
      if (use_operand) {
        char *raw_operand = g_strndup(operand, end - operand);
        char *expanded_operand =
            interpolate_text_depth(raw_operand, environment, depth + 1);
        g_free(raw_operand);
        if (!expanded_operand) {
          g_free(name);
          g_string_free(out, TRUE);
          return NULL;
        }
        g_string_append(out, expanded_operand);
        g_free(expanded_operand);
      }
    } else if (value) {
      g_string_append(out, value);
    } else {
      g_printerr("quocker: warning: Compose variable '%s' is not set; "
                 "substituting an empty string\n",
                 name);
    }
    p = braced ? end + 1 : end;
    g_free(name);
  }
  return g_string_free(out, FALSE);
}

static char *interpolate_text(const char *text, GHashTable *environment) {
  return interpolate_text_depth(text, environment, 0);
}

static gboolean interpolate_node(YNode *node, GHashTable *environment) {
  if (node->kind == NODE_SCALAR) {
    char *expanded =
        interpolate_text(node->scalar ? node->scalar : "", environment);
    if (!expanded) {
      return FALSE;
    }
    g_free(node->scalar);
    node->scalar = expanded;
    return TRUE;
  }
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      if (!interpolate_node(g_ptr_array_index(node->items, i), environment)) {
        return FALSE;
      }
    }
    return TRUE;
  }
  for (guint i = 0; i < node->items->len; i++) {
    YPair *pair = g_ptr_array_index(node->items, i);
    if (!interpolate_node(pair->value, environment)) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean read_project_env_file(GHashTable *environment,
                                      const char *path, gboolean optional) {
  GError *error = NULL;
  gboolean ok = quocker_env_file_read(
      environment, path, optional, environment, TRUE, TRUE, FALSE,
      interpolate_text, &error);
  if (!ok) {
    fail("%s", error ? error->message : "cannot parse environment file");
  }
  g_clear_error(&error);
  return ok;
}

static gboolean project_name_valid_explicit(const char *name) {
  if (!name || !(g_ascii_islower(name[0]) || g_ascii_isdigit(name[0]))) {
    return FALSE;
  }
  for (const char *p = name; *p; p++) {
    if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '-' ||
          *p == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static char *project_name_normalize(const char *name) {
  char *lower = g_ascii_strdown(name ? name : "", -1);
  GString *normalized = g_string_new(NULL);
  for (const char *p = lower; *p; p++) {
    if (g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '-' ||
        *p == '_') {
      g_string_append_c(normalized, *p);
    }
  }
  g_free(lower);
  while (normalized->len &&
         (normalized->str[0] == '-' || normalized->str[0] == '_')) {
    g_string_erase(normalized, 0, 1);
  }
  return g_string_free(normalized, FALSE);
}

static gboolean resolve_project_name(YNode *config, const char *root,
                                     const char *explicit_name,
                                     GHashTable *environment,
                                     char **project_name_out) {
  const char *configured_name = node_string(map_get(config, "name"));
  char *interpolated_config_name = NULL;
  if (configured_name) {
    interpolated_config_name =
        interpolate_text(configured_name, environment);
    if (!interpolated_config_name) {
      return FALSE;
    }
  }
  char *project_name = NULL;
  if (explicit_name) {
    if (!project_name_valid_explicit(explicit_name)) {
      fail("invalid project name '%s': use lowercase letters, digits, '-' "
           "or '_', and begin with a lowercase letter or digit",
           explicit_name);
      g_free(interpolated_config_name);
      return FALSE;
    }
    project_name = g_strdup(explicit_name);
  } else {
    const char *environment_name =
        g_hash_table_lookup(environment, "COMPOSE_PROJECT_NAME");
    const char *source = environment_name && *environment_name
                             ? environment_name
                         : interpolated_config_name &&
                                   *interpolated_config_name
                             ? interpolated_config_name
                             : NULL;
    char *directory_name = NULL;
    if (!source) {
      directory_name = g_path_get_basename(root);
      source = directory_name;
    }
    project_name = project_name_normalize(source);
    g_free(directory_name);
  }
  g_free(interpolated_config_name);
  if (!*project_name) {
    fail("project name is empty after Compose normalization");
    g_free(project_name);
    return FALSE;
  }
  YNode *name_node = node_new(NODE_SCALAR, TAG_STR);
  name_node->scalar = g_strdup(project_name);
  map_set(config, "name", name_node);
  g_hash_table_replace(environment, g_strdup("COMPOSE_PROJECT_NAME"),
                       g_strdup(project_name));
  *project_name_out = project_name;
  return TRUE;
}

static gboolean interpolate_compose_values(YNode *config,
                                            GHashTable *environment) {
  for (guint i = 0; i < config->items->len; i++) {
    YPair *pair = g_ptr_array_index(config->items, i);
    if (g_strcmp0(node_string(pair->key), "name") != 0 &&
        !interpolate_node(pair->value, environment)) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean yaml_parse_text(const char *text, YNode **root,
                                char **problem) {
  yaml_parser_t parser;
  yaml_document_t document;
  if (!yaml_parser_initialize(&parser)) {
    *problem = g_strdup("could not initialize YAML parser");
    return FALSE;
  }
  yaml_parser_set_input_string(&parser, (const unsigned char *)text,
                               strlen(text));
  if (!yaml_parser_load(&parser, &document)) {
    *problem = g_strdup(parser.problem ? parser.problem : "invalid YAML");
    yaml_parser_delete(&parser);
    return FALSE;
  }
  yaml_node_t *source = yaml_document_get_root_node(&document);
  gboolean ok =
      source && yaml_node_convert(&document, source, root, 0, problem);
  if (!ok) {
    if (!*problem) {
      *problem = g_strdup("expected a YAML mapping without recursive aliases");
    }
  }
  yaml_document_delete(&document);
  yaml_event_t trailing_event;
  if (ok && !yaml_parser_parse(&parser, &trailing_event)) {
    *problem = g_strdup(parser.problem ? parser.problem
                                       : "invalid trailing YAML content");
    node_free(*root);
    *root = NULL;
    ok = FALSE;
  } else if (ok) {
    if (trailing_event.type != YAML_STREAM_END_EVENT) {
      *problem =
          g_strdup("Compose files must contain exactly one YAML document");
      node_free(*root);
      *root = NULL;
      ok = FALSE;
    }
    yaml_event_delete(&trailing_event);
  }
  yaml_parser_delete(&parser);
  return ok;
}

static YNode *node_merge(const YNode *base, const YNode *override,
                         const char *field);

static gboolean node_equivalent(const YNode *left, const YNode *right) {
  if (left->kind != right->kind) {
    return FALSE;
  }
  if (left->kind == NODE_SCALAR) {
    return g_strcmp0(left->scalar, right->scalar) == 0;
  }
  if (left->kind == NODE_SEQUENCE) {
    if (left->items->len != right->items->len) {
      return FALSE;
    }
    for (guint i = 0; i < left->items->len; i++) {
      if (!node_equivalent(g_ptr_array_index(left->items, i),
                           g_ptr_array_index(right->items, i))) {
        return FALSE;
      }
    }
    return TRUE;
  }
  if (left->items->len != right->items->len) {
    return FALSE;
  }
  for (guint i = 0; i < left->items->len; i++) {
    YPair *pair = g_ptr_array_index(left->items, i);
    YNode *value = map_get(right, node_string(pair->key));
    if (!value || !node_equivalent(pair->value, value)) {
      return FALSE;
    }
  }
  return TRUE;
}

static char *port_merge_key(const YNode *node) {
  const char *ip = "";
  const char *target = NULL;
  const char *published = "";
  const char *protocol = "tcp";
  char *owned_ip = NULL;
  char *owned_target = NULL;
  char *owned_published = NULL;
  char *owned_protocol = NULL;
  if (node->kind == NODE_MAPPING) {
    target = node_string(map_get(node, "target"));
    if (node_string(map_get(node, "host_ip"))) {
      ip = node_string(map_get(node, "host_ip"));
    }
    if (node_string(map_get(node, "published"))) {
      published = node_string(map_get(node, "published"));
    }
    if (node_string(map_get(node, "protocol"))) {
      owned_protocol = g_ascii_strdown(node_string(map_get(node, "protocol")),
                                       -1);
      protocol = owned_protocol;
    }
  } else if (node->kind == NODE_SCALAR) {
    char *copy = g_strdup(node->scalar ? node->scalar : "");
    char *slash = strrchr(copy, '/');
    if (slash) {
      *slash++ = '\0';
      owned_protocol = g_ascii_strdown(slash, -1);
      protocol = owned_protocol;
    }
    char *ports = copy;
    if (ports[0] == '[') {
      char *closing = strchr(ports, ']');
      if (closing && closing[1] == ':') {
        *closing = '\0';
        owned_ip = g_strdup(ports + 1);
        ip = owned_ip;
        ports = closing + 2;
      }
    }
    gchar **parts = g_strsplit(ports, ":", -1);
    guint count = g_strv_length(parts);
    if (count == 1) {
      owned_target = g_strdup(parts[0]);
    } else if (count == 2) {
      owned_published = g_strdup(parts[0]);
      owned_target = g_strdup(parts[1]);
    } else if (count == 3 && ports == copy) {
      owned_ip = g_strdup(parts[0]);
      ip = owned_ip;
      owned_published = g_strdup(parts[1]);
      owned_target = g_strdup(parts[2]);
    }
    target = owned_target;
    if (owned_published) {
      published = owned_published;
    }
    g_strfreev(parts);
    g_free(copy);
  }
  char *key = target && *target
                  ? g_strdup_printf("%s\037%s\037%s\037%s", ip, target,
                                    published, protocol)
                  : NULL;
  g_free(owned_ip);
  g_free(owned_target);
  g_free(owned_published);
  g_free(owned_protocol);
  return key;
}

static char *resource_merge_key(const char *field, const YNode *node) {
  if (g_strcmp0(field, "ports") == 0) {
    return port_merge_key(node);
  }
  if (g_strcmp0(field, "volumes") != 0 &&
      g_strcmp0(field, "secrets") != 0 &&
      g_strcmp0(field, "configs") != 0) {
    return NULL;
  }
  const char *target = NULL;
  char *owned_target = NULL;
  if (node->kind == NODE_MAPPING) {
    target = node_string(map_get(node, "target"));
    if (!target && g_strcmp0(field, "volumes") != 0) {
      target = node_string(map_get(node, "source"));
    }
  } else if (node->kind == NODE_SCALAR) {
    char **parts = g_strsplit(node->scalar ? node->scalar : "", ":", -1);
    guint count = g_strv_length(parts);
    if (g_strcmp0(field, "volumes") == 0) {
      owned_target = g_strdup(count > 1 ? parts[1] : parts[0]);
      target = owned_target;
    } else {
      owned_target = g_strdup(parts[0]);
      target = owned_target;
    }
    g_strfreev(parts);
  }
  char *key = target && *target ? g_strdup(target) : NULL;
  g_free(owned_target);
  return key;
}

static gboolean sequence_field_deduplicated(const char *field) {
  static const char *const fields[] = {
      "cap_add", "cap_drop", "device_cgroup_rules", "expose",
      "external_links", "security_opt", "constraints", "preferences",
      "generic_resources", NULL};
  for (const char *const *item = fields; *item; item++) {
    if (g_strcmp0(field, *item) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static YNode *node_merge(const YNode *base, const YNode *override,
                         const char *field) {
  if (g_str_equal(override->tag, "!reset")) {
    NodeKind kind = base ? base->kind : override->kind;
    YNode *result = node_new(kind, kind == NODE_MAPPING ? TAG_MAP
                                      : kind == NODE_SEQUENCE ? TAG_SEQ
                                                               : TAG_NULL);
    if (kind == NODE_SCALAR) {
      result->scalar = g_strdup("null");
    }
    return result;
  }
  if (g_str_equal(override->tag, "!override")) {
    YNode *result = node_clone(override);
    g_free(result->tag);
    result->tag = g_strdup(result->kind == NODE_SCALAR     ? TAG_STR
                           : result->kind == NODE_SEQUENCE ? TAG_SEQ
                                                           : TAG_MAP);
    return result;
  }
  if (base->kind == NODE_MAPPING && override->kind == NODE_MAPPING) {
    YNode *result = node_clone(base);
    for (guint i = 0; i < override->items->len; i++) {
      YPair *incoming = g_ptr_array_index(override->items, i);
      const char *key = node_string(incoming->key);
      YNode *old = key ? map_get(result, key) : NULL;
      YNode *value = NULL;
      if (old) {
        value = node_merge(old, incoming->value, key);
      } else if (incoming->value->kind == NODE_MAPPING ||
                 incoming->value->kind == NODE_SEQUENCE) {
        YNode *empty = node_new(incoming->value->kind, incoming->value->tag);
        value = node_merge(empty, incoming->value, key);
        node_free(empty);
      } else {
        value = node_clone(incoming->value);
      }
      map_set(result, key ? key : "", value);
    }
    return result;
  }
  if (base->kind == NODE_SEQUENCE && override->kind == NODE_SEQUENCE) {
    if (g_strcmp0(field, "command") == 0 ||
        g_strcmp0(field, "entrypoint") == 0 ||
        g_strcmp0(field, "test") == 0) {
      return node_clone(override);
    }
    YNode *result = node_clone(base);
    for (guint i = 0; i < override->items->len; i++) {
      YNode *item = g_ptr_array_index(override->items, i);
      char *incoming_key = resource_merge_key(field, item);
      gint existing_index = -1;
      if (incoming_key) {
        for (guint j = 0; j < result->items->len; j++) {
          char *existing_key =
              resource_merge_key(field, g_ptr_array_index(result->items, j));
          gboolean equal = g_strcmp0(incoming_key, existing_key) == 0;
          g_free(existing_key);
          if (equal) {
            existing_index = (gint)j;
            break;
          }
        }
      }
      if (existing_index >= 0) {
        YNode *old = g_ptr_array_index(result->items, existing_index);
        YNode *merged = node_merge(old, item, NULL);
        node_free(old);
        g_ptr_array_index(result->items, existing_index) = merged;
      } else if (sequence_field_deduplicated(field)) {
        gboolean duplicate = FALSE;
        for (guint j = 0; j < result->items->len; j++) {
          duplicate |= node_equivalent(g_ptr_array_index(result->items, j),
                                       item);
        }
        if (!duplicate) {
          g_ptr_array_add(result->items, node_clone(item));
        }
      } else {
        g_ptr_array_add(result->items, node_clone(item));
      }
      g_free(incoming_key);
    }
    return result;
  }
  return node_clone(override);
}

static gboolean emit_node(yaml_emitter_t *emitter, const YNode *node) {
  yaml_event_t event;
  if (node->kind == NODE_SCALAR) {
    yaml_scalar_style_t style = YAML_DOUBLE_QUOTED_SCALAR_STYLE;
    gboolean implicit = g_str_equal(node->tag, TAG_STR);
    if (!yaml_scalar_event_initialize(
            &event, NULL, (yaml_char_t *)node->tag,
            (yaml_char_t *)(node->scalar ? node->scalar : ""),
            node->scalar ? strlen(node->scalar) : 0, implicit, implicit,
            style)) {
      return FALSE;
    }
    return yaml_emitter_emit(emitter, &event);
  }
  if (node->kind == NODE_SEQUENCE) {
    if (!yaml_sequence_start_event_initialize(&event, NULL,
                                              (yaml_char_t *)TAG_SEQ, TRUE,
                                              YAML_BLOCK_SEQUENCE_STYLE) ||
        !yaml_emitter_emit(emitter, &event)) {
      return FALSE;
    }
    for (guint i = 0; i < node->items->len; i++) {
      if (!emit_node(emitter, g_ptr_array_index(node->items, i))) {
        return FALSE;
      }
    }
    return yaml_sequence_end_event_initialize(&event) &&
           yaml_emitter_emit(emitter, &event);
  }
  if (!yaml_mapping_start_event_initialize(&event, NULL, (yaml_char_t *)TAG_MAP,
                                           TRUE, YAML_BLOCK_MAPPING_STYLE) ||
      !yaml_emitter_emit(emitter, &event)) {
    return FALSE;
  }
  for (guint i = 0; i < node->items->len; i++) {
    YPair *pair = g_ptr_array_index(node->items, i);
    if (!emit_node(emitter, pair->key) || !emit_node(emitter, pair->value)) {
      return FALSE;
    }
  }
  return yaml_mapping_end_event_initialize(&event) &&
         yaml_emitter_emit(emitter, &event);
}

static gboolean yaml_write_stdout(const YNode *root) {
  yaml_emitter_t emitter;
  yaml_event_t event;
  gboolean ok = yaml_emitter_initialize(&emitter);
  if (!ok) {
    return FALSE;
  }
  yaml_emitter_set_output_file(&emitter, stdout);
  yaml_emitter_set_unicode(&emitter, TRUE);
  ok = yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING) &&
       yaml_emitter_emit(&emitter, &event) &&
       yaml_document_start_event_initialize(&event, NULL, NULL, NULL, TRUE) &&
       yaml_emitter_emit(&emitter, &event) && emit_node(&emitter, root) &&
       yaml_document_end_event_initialize(&event, TRUE) &&
       yaml_emitter_emit(&emitter, &event) &&
       yaml_stream_end_event_initialize(&event) &&
       yaml_emitter_emit(&emitter, &event);
  yaml_emitter_delete(&emitter);
  return ok;
}

static gboolean json_integer_value(const char *text, gint64 *value) {
  char *clean = g_strdup(text);
  char *write = clean;
  for (const char *read = text; *read; read++) {
    if (*read != '_') {
      *write++ = *read;
    }
  }
  *write = '\0';
  char *digits = clean;
  if (*digits == '+' || *digits == '-') {
    digits++;
  }
  gboolean integer = *digits != '\0';
  for (const char *p = digits; *p; p++) {
    if (!g_ascii_isdigit(*p)) {
      integer = FALSE;
      break;
    }
  }
  int base = 10;
  if (!integer && g_str_has_prefix(digits, "0x")) {
    base = 0;
    integer = TRUE;
  } else if (!integer && g_str_has_prefix(digits, "0o")) {
    base = 8;
    integer = TRUE;
    memmove(digits, digits + 2, strlen(digits + 2) + 1);
    if (*clean == '-') {
      memmove(clean + 1, digits, strlen(digits) + 1);
      digits = clean + 1;
    } else if (*clean == '+') {
      memmove(clean + 1, digits, strlen(digits) + 1);
      digits = clean + 1;
    }
  }
  errno = 0;
  char *end = NULL;
  gint64 parsed = integer ? g_ascii_strtoll(clean, &end, base) : 0;
  integer = integer && end != clean && !*end && errno != ERANGE;
  if (integer) {
    *value = parsed;
  }
  g_free(clean);
  return integer;
}

static void json_add_yaml_value(JsonBuilder *builder, const YNode *node,
                                gboolean mapping_key) {
  if (node->kind == NODE_SEQUENCE) {
    json_builder_begin_array(builder);
    for (guint i = 0; i < node->items->len; i++) {
      json_add_yaml_value(builder, g_ptr_array_index(node->items, i), FALSE);
    }
    json_builder_end_array(builder);
    return;
  }
  if (node->kind == NODE_MAPPING) {
    json_builder_begin_object(builder);
    for (guint i = 0; i < node->items->len; i++) {
      YPair *pair = g_ptr_array_index(node->items, i);
      json_builder_set_member_name(builder, node_string(pair->key));
      json_add_yaml_value(builder, pair->value, FALSE);
    }
    json_builder_end_object(builder);
    return;
  }
  const char *text = node->scalar ? node->scalar : "";
  if (mapping_key || node->scalar_style != YAML_PLAIN_SCALAR_STYLE) {
    json_builder_add_string_value(builder, text);
    return;
  }
  if (g_ascii_strcasecmp(text, "null") == 0 || g_str_equal(text, "~")) {
    json_builder_add_null_value(builder);
    return;
  }
  if (g_ascii_strcasecmp(text, "true") == 0) {
    json_builder_add_boolean_value(builder, TRUE);
    return;
  }
  if (g_ascii_strcasecmp(text, "false") == 0) {
    json_builder_add_boolean_value(builder, FALSE);
    return;
  }
  gint64 integer;
  if (json_integer_value(text, &integer)) {
    json_builder_add_int_value(builder, integer);
    return;
  }
  if (strpbrk(text, ".eE")) {
    char *end = NULL;
    errno = 0;
    double number = g_ascii_strtod(text, &end);
    if (end != text && !*end && errno != ERANGE && isfinite(number)) {
      json_builder_add_double_value(builder, number);
      return;
    }
  }
  json_builder_add_string_value(builder, text);
}

static gboolean json_write_stdout(const YNode *root) {
  JsonBuilder *builder = json_builder_new();
  json_add_yaml_value(builder, root, FALSE);
  JsonNode *document = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, document);
  char *text = json_generator_to_data(generator, NULL);
  gboolean ok = text != NULL;
  if (ok) {
    g_print("%s\n", text);
  }
  g_free(text);
  g_object_unref(generator);
  json_node_free(document);
  g_object_unref(builder);
  return ok;
}

static gint compare_string_pointers(gconstpointer left, gconstpointer right) {
  const char *const *left_string = left;
  const char *const *right_string = right;
  return g_strcmp0(*left_string, *right_string);
}

static gboolean config_write_list(const YNode *config, const char *mode) {
  GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
  YNode *services = map_get(config, "services");
  if (g_strcmp0(mode, "profiles") == 0) {
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; services && i < services->items->len; i++) {
      YPair *service_pair = g_ptr_array_index(services->items, i);
      YNode *profiles = map_get(service_pair->value, "profiles");
      if (!profiles) {
        continue;
      }
      guint count = profiles->kind == NODE_SEQUENCE ? profiles->items->len : 1;
      for (guint j = 0; j < count; j++) {
        YNode *profile = profiles->kind == NODE_SEQUENCE
                             ? g_ptr_array_index(profiles->items, j)
                             : profiles;
        const char *name = node_string(profile);
        if (name && *name && !g_hash_table_contains(seen, name)) {
          g_hash_table_add(seen, (gpointer)name);
          g_ptr_array_add(values, g_strdup(name));
        }
      }
    }
    g_hash_table_destroy(seen);
  } else if (g_strcmp0(mode, "services") == 0 ||
             g_strcmp0(mode, "images") == 0) {
    for (guint i = 0; services && i < services->items->len; i++) {
      YPair *service_pair = g_ptr_array_index(services->items, i);
      const char *name = node_string(service_pair->key);
      const char *image = node_string(map_get(service_pair->value, "image"));
      const char *value = g_strcmp0(mode, "services") == 0 ? name : image;
      if (value && *value) {
        g_ptr_array_add(values, g_strdup(value));
      }
    }
  } else {
    YNode *resources = map_get(config, mode);
    for (guint i = 0; resources && i < resources->items->len; i++) {
      YPair *resource = g_ptr_array_index(resources->items, i);
      const char *name = node_string(resource->key);
      if (name && *name) {
        g_ptr_array_add(values, g_strdup(name));
      }
    }
  }
  g_ptr_array_sort(values, compare_string_pointers);
  for (guint i = 0; i < values->len; i++) {
    g_print("%s\n", (char *)g_ptr_array_index(values, i));
  }
  g_ptr_array_free(values, TRUE);
  return TRUE;
}

static gboolean config_write_environment(GHashTable *environment) {
  GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iterator;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iterator, environment);
  while (g_hash_table_iter_next(&iterator, &key, &value)) {
    g_ptr_array_add(values, g_strdup_printf("%s=%s", (const char *)key,
                                            value ? (const char *)value : ""));
  }
  g_ptr_array_sort(values, compare_string_pointers);
  for (guint i = 0; i < values->len; i++) {
    g_print("%s\n", (char *)g_ptr_array_index(values, i));
  }
  g_ptr_array_free(values, TRUE);
  return TRUE;
}

static char *absolute_path(const char *path, const char *base) {
  if (g_path_is_absolute(path)) {
    return g_canonicalize_filename(path, NULL);
  }
  return g_canonicalize_filename(path, base);
}

static void map_remove(YNode *map, const char *key) {
  for (guint i = 0; i < map->items->len; i++) {
    YPair *pair = g_ptr_array_index(map->items, i);
    if (g_strcmp0(node_string(pair->key), key) == 0) {
      node_free(pair->key);
      node_free(pair->value);
      g_free(pair);
      g_ptr_array_remove_index(map->items, i);
      return;
    }
  }
}

static gboolean include_add_path(YNode *node, GPtrArray *paths) {
  if (node->kind == NODE_SCALAR) {
    const char *path = node_string(node);
    if (path && *path) {
      g_ptr_array_add(paths, g_strdup(path));
      return TRUE;
    }
    return FALSE;
  }
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      if (!include_add_path(g_ptr_array_index(node->items, i), paths)) {
        return FALSE;
      }
    }
    return TRUE;
  }
  return FALSE;
}

static const char *include_first_path(const YNode *node) {
  if (node->kind == NODE_SCALAR) {
    return node_string(node);
  }
  if (node->kind == NODE_SEQUENCE && node->items->len > 0) {
    return include_first_path(g_ptr_array_index(node->items, 0));
  }
  return NULL;
}

static gboolean absolutize_include_node(YNode *node, const char *base) {
  if (node->kind == NODE_SCALAR) {
    const char *path = node_string(node);
    if (!path || !*path) {
      return FALSE;
    }
    char *absolute = absolute_path(path, base);
    g_free(node->scalar);
    node->scalar = absolute;
    return TRUE;
  }
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      if (!absolutize_include_node(g_ptr_array_index(node->items, i), base)) {
        return FALSE;
      }
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean absolutize_compose_include_paths(YNode *model,
                                                 const char *compose_path,
                                                 const char *project_root) {
  YNode *includes = map_get(model, "include");
  if (!includes) {
    return TRUE;
  }
  if (includes->kind != NODE_SEQUENCE) {
    return TRUE;
  }
  char *base =
      compose_path ? g_path_get_dirname(compose_path) : g_strdup(project_root);
  for (guint i = 0; i < includes->items->len; i++) {
    YNode *entry = g_ptr_array_index(includes->items, i);
    YNode *path = entry->kind == NODE_MAPPING ? map_get(entry, "path") : entry;
    if (path && !absolutize_include_node(path, base)) {
      fail("Compose include path must be a path or list of paths");
      g_free(base);
      return FALSE;
    }
    if (entry->kind == NODE_MAPPING && path) {
      YNode *project_directory = map_get(entry, "project_directory");
      if (project_directory &&
          !absolutize_include_node(project_directory, base)) {
        fail("Compose include project_directory must be a path");
        g_free(base);
        return FALSE;
      }
      const char *directory = node_string(project_directory);
      char *default_directory = NULL;
      if (!directory) {
        const char *first_path = include_first_path(path);
        default_directory =
            first_path ? g_path_get_dirname(first_path) : g_strdup(base);
        directory = default_directory;
      }
      YNode *env_file = map_get(entry, "env_file");
      if (env_file && !absolutize_include_node(env_file, directory)) {
        fail("Compose include env_file must be a path or list of paths");
        g_free(default_directory);
        g_free(base);
        return FALSE;
      }
      g_free(default_directory);
    }
  }
  g_free(base);
  return TRUE;
}

static void include_copy_missing_resources(YNode *destination,
                                           const YNode *source) {
  for (guint i = 0; i < source->items->len; i++) {
    YPair *source_pair = g_ptr_array_index(source->items, i);
    const char *section = node_string(source_pair->key);
    if (!section || g_str_equal(section, "include") ||
        g_str_equal(section, "name") || g_str_equal(section, "version")) {
      continue;
    }
    if (source_pair->value->kind == NODE_MAPPING) {
      YNode *target = map_get(destination, section);
      if (!target) {
        map_set(destination, section, node_clone(source_pair->value));
        continue;
      }
      if (target->kind != NODE_MAPPING) {
        g_printerr(
            "quocker: warning: included Compose section '%s' "
            "conflicts with the current project; keeping current value\n",
            section);
        continue;
      }
      for (guint j = 0; j < source_pair->value->items->len; j++) {
        YPair *resource = g_ptr_array_index(source_pair->value->items, j);
        const char *name = node_string(resource->key);
        if (name && !map_get(target, name)) {
          map_set(target, name, node_clone(resource->value));
        } else if (name) {
          g_printerr("quocker: warning: included Compose resource '%s.%s' "
                     "conflicts with the current project; keeping current "
                     "resource\n",
                     section, name);
        }
      }
    } else if (!map_get(destination, section)) {
      map_set(destination, section, node_clone(source_pair->value));
    }
  }
}

static GHashTable *include_environment_new(GHashTable *parent_environment,
                                           const char *project_directory,
                                           YNode *env_file) {
  GHashTable *environment =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  gchar **process_environment = g_get_environ();
  for (guint i = 0; process_environment[i]; i++) {
    char **entry = g_strsplit(process_environment[i], "=", 2);
    if (entry[0] && entry[1]) {
      g_hash_table_replace(environment, g_strdup(entry[0]), g_strdup(entry[1]));
    }
    g_strfreev(entry);
  }
  g_strfreev(process_environment);

  GPtrArray *env_files = g_ptr_array_new_with_free_func(g_free);
  if (env_file &&
      (!include_add_path(env_file, env_files) || env_files->len == 0)) {
    fail("Compose include env_file must be a path or list of paths");
    g_ptr_array_free(env_files, TRUE);
    g_hash_table_destroy(environment);
    return NULL;
  }
  gboolean ok = TRUE;
  if (env_files->len == 0) {
    char *default_env = g_build_filename(project_directory, ".env", NULL);
    ok = read_project_env_file(environment, default_env, TRUE);
    g_free(default_env);
  } else {
    for (guint i = 0; ok && i < env_files->len; i++) {
      char *path =
          absolute_path(g_ptr_array_index(env_files, i), project_directory);
      ok = read_project_env_file(environment, path, FALSE);
      g_free(path);
    }
  }
  g_ptr_array_free(env_files, TRUE);
  if (!ok) {
    g_hash_table_destroy(environment);
    return NULL;
  }
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, parent_environment);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_hash_table_replace(environment, g_strdup(key), g_strdup(value));
  }
  return environment;
}

static void resolve_included_service_paths(YNode *service,
                                           const char *project_directory) {
  YNode *image = map_get(service, "image");
  const char *image_name = node_string(image);
  gboolean local_image = image_name && (g_path_is_absolute(image_name) ||
                                        g_str_has_prefix(image_name, "./") ||
                                        g_str_has_prefix(image_name, "../"));
  if (image_name && !local_image) {
    char *candidate = g_build_filename(project_directory, image_name, NULL);
    local_image = g_file_test(candidate, G_FILE_TEST_IS_REGULAR);
    g_free(candidate);
  }
  if (local_image) {
    char *absolute = absolute_path(image_name, project_directory);
    g_free(image->scalar);
    image->scalar = absolute;
  }
  YNode *env_file = map_get(service, "env_file");
  if (env_file) {
    if (env_file->kind == NODE_SCALAR) {
      absolutize_include_node(env_file, project_directory);
    } else if (env_file->kind == NODE_SEQUENCE) {
      for (guint i = 0; i < env_file->items->len; i++) {
        YNode *entry = g_ptr_array_index(env_file->items, i);
        if (entry->kind == NODE_SCALAR) {
          absolutize_include_node(entry, project_directory);
        } else if (entry->kind == NODE_MAPPING) {
          YNode *path = map_get(entry, "path");
          if (path) {
            absolutize_include_node(path, project_directory);
          }
        }
      }
    } else if (env_file->kind == NODE_MAPPING) {
      YNode *path = map_get(env_file, "path");
      if (path) {
        absolutize_include_node(path, project_directory);
      }
    }
  }
}

static void resolve_included_model_paths(YNode *model,
                                         const char *project_directory) {
  YNode *services = map_get(model, "services");
  if (services && services->kind == NODE_MAPPING) {
    for (guint i = 0; i < services->items->len; i++) {
      YPair *pair = g_ptr_array_index(services->items, i);
      resolve_included_service_paths(pair->value, project_directory);
    }
  }
  for (const char *section_name = "configs"; section_name;
       section_name = g_str_equal(section_name, "configs") ? "secrets" : NULL) {
    YNode *section = map_get(model, section_name);
    if (!section || section->kind != NODE_MAPPING) {
      continue;
    }
    for (guint i = 0; i < section->items->len; i++) {
      YPair *pair = g_ptr_array_index(section->items, i);
      YNode *file = map_get(pair->value, "file");
      if (file && file->kind == NODE_SCALAR) {
        absolutize_include_node(file, project_directory);
      }
    }
  }
}

static gboolean expand_compose_includes(YNode *model, const char *compose_path,
                                        const char *project_root,
                                        GHashTable *environment,
                                        GHashTable *include_stack,
                                        guint depth) {
  YNode *includes = map_get(model, "include");
  if (!includes) {
    return TRUE;
  }
  if (includes->kind != NODE_SEQUENCE) {
    fail("Compose include must be a sequence");
    return FALSE;
  }
  if (depth >= 32) {
    fail("Compose include nesting exceeds 32 files");
    return FALSE;
  }
  char *containing_directory =
      compose_path ? g_path_get_dirname(compose_path) : g_strdup(project_root);
  YNode *imported_model = node_new(NODE_MAPPING, TAG_MAP);
  for (guint i = 0; i < includes->items->len; i++) {
    YNode *entry = g_ptr_array_index(includes->items, i);
    YNode *path_node = entry;
    YNode *env_file_node = NULL;
    YNode *project_directory_node = NULL;
    if (entry->kind == NODE_MAPPING) {
      path_node = map_get(entry, "path");
      env_file_node = map_get(entry, "env_file");
      project_directory_node = map_get(entry, "project_directory");
    }
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (!path_node || !include_add_path(path_node, paths) || paths->len == 0) {
      fail("Compose include entries require a path or list of paths");
      g_ptr_array_free(paths, TRUE);
      node_free(imported_model);
      g_free(containing_directory);
      return FALSE;
    }
    const char *first_path = include_first_path(path_node);
    const char *project_directory_value = node_string(project_directory_node);
    if (project_directory_node && !project_directory_value) {
      fail("Compose include project_directory must be a path");
      g_ptr_array_free(paths, TRUE);
      node_free(imported_model);
      g_free(containing_directory);
      return FALSE;
    }
    char *default_project_directory =
        project_directory_value
            ? absolute_path(project_directory_value, containing_directory)
            : absolute_path(first_path ? first_path : ".",
                            containing_directory);
    if (!project_directory_value) {
      char *directory = g_path_get_dirname(default_project_directory);
      g_free(default_project_directory);
      default_project_directory = directory;
    }
    GHashTable *included_environment = include_environment_new(
        environment, default_project_directory, env_file_node);
    if (!included_environment) {
      g_ptr_array_free(paths, TRUE);
      node_free(imported_model);
      g_free(default_project_directory);
      g_free(containing_directory);
      return FALSE;
    }
    YNode *entry_model = node_new(NODE_MAPPING, TAG_MAP);
    for (guint j = 0; j < paths->len; j++) {
      const char *path = g_ptr_array_index(paths, j);
      char *absolute = absolute_path(path, containing_directory);
      if (g_hash_table_contains(include_stack, absolute)) {
        fail("Compose include cycle detected at %s", absolute);
        g_free(absolute);
        g_ptr_array_free(paths, TRUE);
        node_free(entry_model);
        node_free(imported_model);
        g_hash_table_destroy(included_environment);
        g_free(default_project_directory);
        g_free(containing_directory);
        return FALSE;
      }
      gchar *contents = NULL;
      if (!g_file_get_contents(absolute, &contents, NULL, NULL)) {
        fail("cannot read included Compose file %s", absolute);
        g_free(absolute);
        g_ptr_array_free(paths, TRUE);
        node_free(entry_model);
        node_free(imported_model);
        g_hash_table_destroy(included_environment);
        g_free(default_project_directory);
        g_free(containing_directory);
        return FALSE;
      }
      YNode *included = NULL;
      char *problem = NULL;
      gboolean ok = yaml_parse_text(contents, &included, &problem);
      g_free(contents);
      if (!ok || !included || included->kind != NODE_MAPPING) {
        fail("invalid included Compose YAML in %s: %s", absolute,
             problem ? problem : "expected a mapping");
        g_free(problem);
        node_free(included);
        g_free(absolute);
        g_ptr_array_free(paths, TRUE);
        node_free(entry_model);
        node_free(imported_model);
        g_hash_table_destroy(included_environment);
        g_free(default_project_directory);
        g_free(containing_directory);
        return FALSE;
      }
      g_hash_table_add(include_stack, g_strdup(absolute));
      gboolean interpolated =
          interpolate_compose_values(included, included_environment);
      ok = interpolated && expand_compose_includes(
                               included, absolute, default_project_directory,
                               included_environment, include_stack, depth + 1);
      g_hash_table_remove(include_stack, absolute);
      if (!ok) {
        node_free(included);
        g_free(absolute);
        g_ptr_array_free(paths, TRUE);
        node_free(entry_model);
        node_free(imported_model);
        g_hash_table_destroy(included_environment);
        g_free(default_project_directory);
        g_free(containing_directory);
        return FALSE;
      }
      YNode *next = node_merge(entry_model, included, NULL);
      node_free(entry_model);
      entry_model = next;
      node_free(included);
      g_free(absolute);
    }
    g_ptr_array_free(paths, TRUE);
    resolve_included_model_paths(entry_model, default_project_directory);
    include_copy_missing_resources(imported_model, entry_model);
    node_free(entry_model);
    g_hash_table_destroy(included_environment);
    g_free(default_project_directory);
  }
  map_remove(model, "include");
  include_copy_missing_resources(model, imported_model);
  node_free(imported_model);
  g_free(containing_directory);
  return TRUE;
}

static gboolean extends_sequence_unique(const char *field) {
  static const char *const fields[] = {"cap_add",
                                       "cap_drop",
                                       "configs",
                                       "constraints",
                                       "preferences",
                                       "generic_resources",
                                       "device_cgroup_rules",
                                       "expose",
                                       "external_links",
                                       "ports",
                                       "secrets",
                                       "security_opt",
                                       NULL};
  for (const char *const *item = fields; *item; item++) {
    if (g_strcmp0(field, *item) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static char *extends_sequence_key(const char *field, const YNode *node) {
  if (g_strcmp0(field, "ports") == 0 || g_strcmp0(field, "volumes") == 0 ||
      g_strcmp0(field, "configs") == 0 || g_strcmp0(field, "secrets") == 0) {
    return resource_merge_key(field, node);
  }
  if (g_strcmp0(field, "devices") == 0) {
    if (node->kind == NODE_MAPPING) {
      return g_strdup(node_string(map_get(node, "target")));
    }
    if (node->kind == NODE_SCALAR) {
      gchar **parts = g_strsplit(node->scalar ? node->scalar : "", ":", -1);
      const char *target = g_strv_length(parts) > 1 ? parts[1] : parts[0];
      char *key = g_strdup(target);
      g_strfreev(parts);
      return key;
    }
  }
  if (g_str_has_prefix(field, "device_") &&
      (g_str_has_suffix(field, "_bps") || g_str_has_suffix(field, "_iops")) &&
      node->kind == NODE_MAPPING) {
    return g_strdup(node_string(map_get(node, "path")));
  }
  return NULL;
}

static YNode *node_merge_extends(const YNode *base, const YNode *override,
                                 const char *field) {
  if (base->kind == NODE_MAPPING && override->kind == NODE_MAPPING) {
    YNode *result = node_clone(base);
    for (guint i = 0; i < override->items->len; i++) {
      YPair *incoming = g_ptr_array_index(override->items, i);
      const char *key = node_string(incoming->key);
      YNode *old = key ? map_get(result, key) : NULL;
      YNode *value = old ? node_merge_extends(old, incoming->value, key)
                         : node_clone(incoming->value);
      map_set(result, key ? key : "", value);
    }
    return result;
  }
  if (base->kind == NODE_SEQUENCE && override->kind == NODE_SEQUENCE) {
    YNode *result = node_clone(base);
    for (guint i = 0; i < override->items->len; i++) {
      YNode *incoming = g_ptr_array_index(override->items, i);
      char *incoming_key = extends_sequence_key(field, incoming);
      gint existing = -1;
      if (incoming_key) {
        for (guint j = 0; j < result->items->len; j++) {
          char *existing_key =
              extends_sequence_key(field, g_ptr_array_index(result->items, j));
          gboolean equal = g_strcmp0(incoming_key, existing_key) == 0;
          g_free(existing_key);
          if (equal) {
            existing = (gint)j;
            break;
          }
        }
      }
      if (existing >= 0) {
        YNode *old = g_ptr_array_index(result->items, existing);
        YNode *merged = node_merge_extends(old, incoming, NULL);
        node_free(old);
        g_ptr_array_index(result->items, existing) = merged;
      } else if (extends_sequence_unique(field)) {
        gboolean duplicate = FALSE;
        for (guint j = 0; j < result->items->len; j++) {
          duplicate |=
              node_equivalent(g_ptr_array_index(result->items, j), incoming);
        }
        if (!duplicate) {
          g_ptr_array_add(result->items, node_clone(incoming));
        }
      } else {
        g_ptr_array_add(result->items, node_clone(incoming));
      }
      g_free(incoming_key);
    }
    return result;
  }
  return node_clone(override);
}

static gboolean resolve_service_extends(YNode *model, const char *service_name,
                                        const char *origin_file,
                                        const char *project_root,
                                        GHashTable *environment,
                                        GHashTable *resolution_stack,
                                        guint depth);

static gboolean resolve_service_extends(YNode *model, const char *service_name,
                                        const char *origin_file,
                                        const char *project_root,
                                        GHashTable *environment,
                                        GHashTable *resolution_stack,
                                        guint depth) {
  if (depth >= 64) {
    fail("Compose extends nesting exceeds 64 services");
    return FALSE;
  }
  char *identity = g_strdup_printf(
      "%s\037%s", origin_file ? origin_file : project_root, service_name);
  if (g_hash_table_contains(resolution_stack, identity)) {
    fail("Compose extends cycle detected at service '%s'", service_name);
    g_free(identity);
    return FALSE;
  }
  YNode *services = map_get(model, "services");
  YNode *service = map_get(services, service_name);
  if (!service || service->kind != NODE_MAPPING) {
    fail("Compose extends service '%s' was not found", service_name);
    g_free(identity);
    return FALSE;
  }
  YNode *extends = map_get(service, "extends");
  if (!extends) {
    g_free(identity);
    return TRUE;
  }
  const char *base_name = NULL;
  const char *base_file = NULL;
  if (extends->kind == NODE_SCALAR) {
    base_name = node_string(extends);
  } else if (extends->kind == NODE_MAPPING) {
    for (guint i = 0; i < extends->items->len; i++) {
      YPair *pair = g_ptr_array_index(extends->items, i);
      const char *key = node_string(pair->key);
      if (!key || (!g_str_equal(key, "service") && !g_str_equal(key, "file"))) {
        fail("service '%s': extends accepts only service and file",
             service_name);
        g_free(identity);
        return FALSE;
      }
    }
    base_name = node_string(map_get(extends, "service"));
    YNode *file_node = map_get(extends, "file");
    if (file_node) {
      base_file = node_string(file_node);
      if (!base_file || !*base_file) {
        fail("service '%s': extends file must be a non-empty path",
             service_name);
        g_free(identity);
        return FALSE;
      }
    }
  }
  if (!base_name || !*base_name) {
    fail("service '%s': extends requires a service name", service_name);
    g_free(identity);
    return FALSE;
  }
  g_hash_table_add(resolution_stack, identity);

  YNode *base_model = model;
  YNode *external_model = NULL;
  char *absolute_base_file = NULL;
  if (base_file && *base_file) {
    absolute_base_file = absolute_path(base_file, project_root);
    gchar *contents = NULL;
    char *problem = NULL;
    if (!g_file_get_contents(absolute_base_file, &contents, NULL, NULL)) {
      fail("service '%s': cannot read extends file %s", service_name,
           absolute_base_file);
      g_hash_table_remove(resolution_stack, identity);
      g_free(absolute_base_file);
      return FALSE;
    }
    gboolean parsed_ok = yaml_parse_text(contents, &external_model, &problem);
    g_free(contents);
    if (!parsed_ok || !external_model || external_model->kind != NODE_MAPPING) {
      fail("service '%s': invalid extends file %s: %s", service_name,
           absolute_base_file, problem ? problem : "expected a mapping");
      g_free(problem);
      node_free(external_model);
      g_hash_table_remove(resolution_stack, identity);
      g_free(absolute_base_file);
      return FALSE;
    }
    gboolean prepared = interpolate_compose_values(external_model, environment);
    if (prepared) {
      GHashTable *include_stack =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      prepared =
          expand_compose_includes(external_model, absolute_base_file,
                                  project_root, environment, include_stack, 0);
      g_hash_table_destroy(include_stack);
    }
    if (!prepared) {
      node_free(external_model);
      g_hash_table_remove(resolution_stack, identity);
      g_free(absolute_base_file);
      return FALSE;
    }
    base_model = external_model;
  }
  YNode *base_services = map_get(base_model, "services");
  YNode *base_service = map_get(base_services, base_name);
  gboolean ok = base_service && base_service->kind == NODE_MAPPING;
  if (!ok) {
    fail("service '%s': extends service '%s' was not found", service_name,
         base_name);
  } else {
    ok = resolve_service_extends(base_model, base_name, absolute_base_file,
                                 project_root, environment, resolution_stack,
                                 depth + 1);
  }
  if (ok) {
    base_service = map_get(map_get(base_model, "services"), base_name);
    if (absolute_base_file) {
      char *base_directory = g_path_get_dirname(absolute_base_file);
      resolve_included_service_paths(base_service, base_directory);
      g_free(base_directory);
    }
    YNode *resolved = node_merge_extends(base_service, service, NULL);
    map_remove(resolved, "extends");
    for (guint i = 0; i < services->items->len; i++) {
      YPair *pair = g_ptr_array_index(services->items, i);
      if (g_strcmp0(node_string(pair->key), service_name) == 0) {
        node_free(pair->value);
        pair->value = resolved;
        break;
      }
    }
  }
  node_free(external_model);
  g_free(absolute_base_file);
  g_hash_table_remove(resolution_stack, identity);
  return ok;
}

static gboolean resolve_compose_extends(YNode *model, const char *project_root,
                                        GHashTable *environment) {
  YNode *services = map_get(model, "services");
  if (!services || services->kind != NODE_MAPPING) {
    return TRUE;
  }
  GHashTable *resolution_stack =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean ok = TRUE;
  for (guint i = 0; ok && i < services->items->len; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    const char *name = node_string(pair->key);
    if (name) {
      ok = resolve_service_extends(model, name, NULL, project_root, environment,
                                   resolution_stack, 0);
    }
  }
  g_hash_table_destroy(resolution_stack);
  return ok;
}

static char *discover_file(const char *directory) {
  for (const char **name = compose_names; *name; name++) {
    char *path = g_build_filename(directory, *name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      return path;
    }
    g_free(path);
  }
  return NULL;
}

static GPtrArray *resolve_files(Options *opts, char **root_out) {
  GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
  char *cwd = g_get_current_dir();
  char *root = opts->project_directory
                   ? absolute_path(opts->project_directory, cwd)
                   : NULL;
  if (opts->files->len == 0) {
    const char *compose_env = g_getenv("COMPOSE_FILE");
    if (compose_env && *compose_env) {
      const char *separator = g_getenv("COMPOSE_PATH_SEPARATOR");
      if (!separator || !*separator) {
        separator = G_SEARCHPATH_SEPARATOR_S;
      }
      gchar **parts = g_strsplit(compose_env, separator, -1);
      for (guint i = 0; parts[i]; i++) {
        if (*parts[i]) {
          g_ptr_array_add(opts->files, g_strdup(parts[i]));
        }
      }
      g_strfreev(parts);
    } else {
      char *dir = root ? g_strdup(root) : g_strdup(cwd);
      for (;;) {
        char *found = discover_file(dir);
        if (found) {
          g_ptr_array_add(files, found);
          if (!root) {
            root = g_strdup(dir);
          }
          break;
        }
        char *parent = g_path_get_dirname(dir);
        if (g_str_equal(parent, dir)) {
          g_free(parent);
          break;
        }
        g_free(dir);
        dir = parent;
      }
      if (files->len == 0) {
        if (!root) {
          root = g_strdup(cwd);
        }
        g_ptr_array_add(files, g_build_filename(root, "compose.yaml", NULL));
      }
      g_free(dir);
    }
  }
  if (opts->files->len) {
    for (guint i = 0; i < opts->files->len; i++) {
      const char *file = g_ptr_array_index(opts->files, i);
      g_ptr_array_add(files,
                      g_str_equal(file, "-") ? NULL : absolute_path(file, cwd));
    }
  }
  if (!root) {
    for (guint i = 0; i < files->len; i++) {
      const char *file = g_ptr_array_index(files, i);
      if (file) {
        root = g_path_get_dirname(file);
        break;
      }
    }
  }
  if (!root) {
    root = g_strdup(cwd);
  }
  *root_out = root;
  g_free(cwd);
  return files;
}

static gboolean parse_compose_files(GPtrArray *files, const char *root,
                                    GPtrArray *env_files,
                                    const char *explicit_project_name,
                                    YNode **config,
                                    GHashTable **environment_out,
                                    char **project_name_out) {
  GHashTable *environment =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  /* Copy process environment explicitly; GLib owns the returned vector. */
  gchar **process_env = g_get_environ();
  for (guint i = 0; process_env[i]; i++) {
    char **entry = g_strsplit(process_env[i], "=", 2);
    if (entry[0] && entry[1]) {
      g_hash_table_replace(environment, g_strdup(entry[0]), g_strdup(entry[1]));
    }
    g_strfreev(entry);
  }
  g_strfreev(process_env);
  if (env_files->len == 0) {
    const char *compose_env_files = g_getenv("COMPOSE_ENV_FILES");
    if (compose_env_files && *compose_env_files) {
      gchar **parts = g_strsplit(compose_env_files, ",", -1);
      for (guint i = 0; parts[i]; i++) {
        char *path = g_strstrip(parts[i]);
        if (*path) {
          g_ptr_array_add(env_files, g_strdup(path));
        }
      }
      g_strfreev(parts);
    }
  }
  if (env_files->len == 0) {
    const char *disable_default = g_getenv("COMPOSE_DISABLE_ENV_FILE");
    gboolean disabled =
        disable_default && (g_str_equal(disable_default, "1") ||
                            g_ascii_strcasecmp(disable_default, "true") == 0);
    if (!disabled) {
      char *dotenv = g_build_filename(root, ".env", NULL);
      gboolean read_ok = read_project_env_file(environment, dotenv, TRUE);
      g_free(dotenv);
      if (!read_ok) {
        g_hash_table_destroy(environment);
        return FALSE;
      }
    }
  } else {
    for (guint i = 0; i < env_files->len; i++) {
      char *path = absolute_path(g_ptr_array_index(env_files, i), root);
      gboolean read_ok = read_project_env_file(environment, path, FALSE);
      g_free(path);
      if (!read_ok) {
        g_hash_table_destroy(environment);
        return FALSE;
      }
    }
  }
  YNode *merged = node_new(NODE_MAPPING, TAG_MAP);
  GPtrArray *parsed_files =
      g_ptr_array_new_with_free_func((GDestroyNotify)node_free);
  for (guint i = 0; i < files->len; i++) {
    const char *path = g_ptr_array_index(files, i);
    gchar *contents = NULL;
    if (!path) {
      GString *stdin_text = g_string_new(NULL);
      char buffer[4096];
      size_t got;
      while ((got = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        g_string_append_len(stdin_text, buffer, got);
      }
      contents = g_string_free(stdin_text, FALSE);
    } else if (!g_file_get_contents(path, &contents, NULL, NULL)) {
      fail("cannot read Compose file %s", path);
      node_free(merged);
      g_ptr_array_free(parsed_files, TRUE);
      g_hash_table_destroy(environment);
      return FALSE;
    }
    YNode *parsed = NULL;
    char *problem = NULL;
    gboolean ok = yaml_parse_text(contents, &parsed, &problem);
    g_free(contents);
    if (!ok || !parsed || parsed->kind != NODE_MAPPING) {
      fail("invalid Compose YAML in %s: %s", path ? path : "stdin",
           problem ? problem : "expected a mapping");
      g_free(problem);
      node_free(parsed);
      node_free(merged);
      g_ptr_array_free(parsed_files, TRUE);
      g_hash_table_destroy(environment);
      return FALSE;
    }
    g_ptr_array_add(parsed_files, parsed);
    YNode *next = node_merge(merged, parsed, NULL);
    node_free(merged);
    merged = next;
  }
  if (!resolve_project_name(merged, root, explicit_project_name, environment,
                            project_name_out)) {
    node_free(merged);
    g_ptr_array_free(parsed_files, TRUE);
    g_hash_table_destroy(environment);
    return FALSE;
  }
  YNode *interpolated = node_new(NODE_MAPPING, TAG_MAP);
  gboolean interpolation_ok = TRUE;
  for (guint i = 0; i < parsed_files->len && interpolation_ok; i++) {
    YNode *parsed = node_clone(g_ptr_array_index(parsed_files, i));
    interpolation_ok = interpolate_compose_values(parsed, environment);
    if (interpolation_ok) {
      interpolation_ok = absolutize_compose_include_paths(
          parsed, g_ptr_array_index(files, i), root);
    }
    if (interpolation_ok) {
      YNode *next = node_merge(interpolated, parsed, NULL);
      node_free(interpolated);
      interpolated = next;
    }
    node_free(parsed);
  }
  g_ptr_array_free(parsed_files, TRUE);
  if (!interpolation_ok) {
    node_free(interpolated);
    node_free(merged);
    g_hash_table_destroy(environment);
    g_free(*project_name_out);
    *project_name_out = NULL;
    return FALSE;
  }
  GHashTable *include_stack =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean includes_ok = expand_compose_includes(interpolated, NULL, root,
                                                 environment, include_stack, 0);
  g_hash_table_destroy(include_stack);
  if (!includes_ok) {
    node_free(interpolated);
    node_free(merged);
    g_hash_table_destroy(environment);
    g_free(*project_name_out);
    *project_name_out = NULL;
    return FALSE;
  }
  if (!resolve_compose_extends(interpolated, root, environment)) {
    node_free(interpolated);
    node_free(merged);
    g_hash_table_destroy(environment);
    g_free(*project_name_out);
    *project_name_out = NULL;
    return FALSE;
  }
  YNode *normalized_name = node_new(NODE_SCALAR, TAG_STR);
  normalized_name->scalar = g_strdup(*project_name_out);
  map_set(interpolated, "name", normalized_name);
  node_free(merged);
  merged = interpolated;
  const char *schema_override = g_getenv("QUOCKER_COMPOSE_SCHEMA");
  const char *schema_path =
      schema_override && *schema_override ? schema_override : NULL;
  if (!schema_path &&
      g_file_test(QUOCKER_SCHEMA_INSTALL_PATH, G_FILE_TEST_IS_REGULAR)) {
    schema_path = QUOCKER_SCHEMA_INSTALL_PATH;
  }
  if (!schema_path &&
      g_file_test(QUOCKER_SCHEMA_BUILD_PATH, G_FILE_TEST_IS_REGULAR)) {
    schema_path = QUOCKER_SCHEMA_BUILD_PATH;
  }
  if (!schema_path) {
    fail("cannot find the pinned Compose schema; reinstall Quocker or set "
         "QUOCKER_COMPOSE_SCHEMA");
    node_free(merged);
    g_hash_table_destroy(environment);
    return FALSE;
  }
  JsonBuilder *builder = json_builder_new();
  json_add_yaml_value(builder, merged, FALSE);
  JsonNode *instance = json_builder_get_root(builder);
  char *schema_error = NULL;
  gboolean schema_valid = quocker_compose_schema_validate(
      schema_path, instance, &schema_error);
  json_node_free(instance);
  g_object_unref(builder);
  if (!schema_valid) {
    fail("%s", schema_error ? schema_error : "Compose schema validation failed");
    g_free(schema_error);
    node_free(merged);
    g_hash_table_destroy(environment);
    return FALSE;
  }
  *config = merged;
  *environment_out = environment;
  return TRUE;
}

static gboolean valid_service_name(const char *name) {
  if (!name || !(g_ascii_isalnum(*name))) {
    return FALSE;
  }
  for (const char *p = name; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '_' || *p == '.' || *p == '-')) {
      return FALSE;
    }
  }
  return TRUE;
}

static char *service_dir(const char *root, const char *project) {
  return g_build_filename(root, ".quocker", project, NULL);
}

static char *state_path(const char *directory, const char *service) {
  return g_strdup_printf("%s/%s.state", directory, service);
}

static char *state_disk_basename(const char *directory, const char *service) {
  char *path = state_path(directory, service);
  gchar *contents = NULL;
  char *disk = NULL;
  if (g_file_get_contents(path, &contents, NULL, NULL)) {
    gchar **lines = g_strsplit(contents, "\n", 0);
    for (guint i = 1; lines[i]; i++) {
      if (*lines[i] && !strchr(lines[i], '/') &&
          !g_str_equal(lines[i], ".") && !g_str_equal(lines[i], "..") &&
          g_str_has_suffix(lines[i], ".qcow2")) {
        disk = g_strdup(lines[i]);
        break;
      }
    }
    g_strfreev(lines);
    g_free(contents);
  }
  g_free(path);
  return disk;
}

static guint64 process_start_time(pid_t pid) {
  char *path = g_strdup_printf("/proc/%d/stat", pid);
  gchar *contents = NULL;
  guint64 start_time = 0;
  if (g_file_get_contents(path, &contents, NULL, NULL)) {
    /* comm is parenthesized and may itself contain spaces or parentheses. */
    char *fields = strrchr(contents, ')');
    if (fields && fields[1] == ' ') {
      fields += 2;
      char **tokens = g_strsplit(fields, " ", 0);
      /* After comm, token 0 is field 3; token 19 is field 22. */
      if (tokens[19] && *tokens[19]) {
        char *end = NULL;
        guint64 parsed = g_ascii_strtoull(tokens[19], &end, 10);
        if (end != tokens[19] && (*end == '\n' || *end == '\0')) {
          start_time = parsed;
        }
      }
      g_strfreev(tokens);
    }
    g_free(contents);
  }
  g_free(path);
  return start_time;
}

static guint64 state_process_start_time(const char *directory,
                                        const char *service) {
  char *path = state_path(directory, service);
  gchar *contents = NULL;
  guint64 start_time = 0;
  if (g_file_get_contents(path, &contents, NULL, NULL)) {
    gchar **lines = g_strsplit(contents, "\n", 0);
    gboolean after_disk = FALSE;
    for (guint i = 0; lines[i]; i++) {
      if (after_disk && *lines[i]) {
        char *end = NULL;
        guint64 parsed = g_ascii_strtoull(lines[i], &end, 10);
        if (end != lines[i] && (*end == '\n' || *end == '\0')) {
          start_time = parsed;
        }
        break;
      }
      after_disk = *lines[i] && !strchr(lines[i], '/') &&
                   g_str_has_suffix(lines[i], ".qcow2");
    }
    g_strfreev(lines);
    g_free(contents);
  }
  g_free(path);
  return start_time;
}

static gboolean service_overlay_name(const char *filename,
                                     const char *service) {
  char *exact = g_strdup_printf("%s.qcow2", service);
  gboolean exact_match = g_str_equal(filename, exact);
  g_free(exact);
  if (exact_match) {
    return TRUE;
  }
  char *prefix = g_strdup_printf("%s-", service);
  gboolean match = g_str_has_prefix(filename, prefix) &&
                   g_str_has_suffix(filename, ".qcow2");
  g_free(prefix);
  return match;
}

static char *find_service_overlay(const char *directory, const char *service) {
  char *selected = state_disk_basename(directory, service);
  if (selected) {
    char *path = g_build_filename(directory, selected, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      g_free(selected);
      return path;
    }
    g_free(path);
    g_free(selected);
  }
  GDir *dir = g_dir_open(directory, 0, NULL);
  if (!dir) {
    return NULL;
  }
  const char *entry;
  char *latest = NULL;
  time_t latest_mtime = 0;
  while ((entry = g_dir_read_name(dir))) {
    if (!service_overlay_name(entry, service)) {
      continue;
    }
    char *path = g_build_filename(directory, entry, NULL);
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
        (!latest || st.st_mtime >= latest_mtime)) {
      g_free(latest);
      latest = g_strdup(path);
      latest_mtime = st.st_mtime;
    }
    g_free(path);
  }
  g_dir_close(dir);
  return latest;
}

static pid_t read_pid(const char *directory, const char *service) {
  char *path = state_path(directory, service);
  gchar *contents = NULL;
  pid_t pid = 0;
  if (g_file_get_contents(path, &contents, NULL, NULL)) {
    char *start = contents;
    if (g_str_has_prefix(contents, "{")) {
      start = strstr(contents, "\"pid\"");
      start = start ? strchr(start, ':') : NULL;
      if (start) {
        start++;
      }
    }
    if (start) {
      pid = (pid_t)g_ascii_strtoll(start, NULL, 10);
    }
    g_free(contents);
  }
  g_free(path);
  return pid;
}

static gboolean process_running(pid_t pid, const char *project,
                                const char *service,
                                guint64 expected_start_time) {
  if (pid <= 1 || (kill(pid, 0) != 0 && errno != EPERM)) {
    return FALSE;
  }
  char *proc = g_strdup_printf("/proc/%d/cmdline", pid);
  if (!g_file_test("/proc", G_FILE_TEST_IS_DIR)) {
    g_free(proc);
    return expected_start_time == 0;
  }
  gchar *cmdline = NULL;
  gsize length = 0;
  if (!g_file_get_contents(proc, &cmdline, &length, NULL)) {
    g_free(proc);
    return FALSE;
  }
  if (expected_start_time &&
      process_start_time(pid) != expected_start_time) {
    g_free(cmdline);
    g_free(proc);
    return FALSE;
  }
  char *expected = g_strdup_printf("%s-%s", project, service);
  gboolean found = FALSE;
  gboolean after_name_option = FALSE;
  for (gsize start = 0; start < length;) {
    gsize end = start;
    while (end < length && cmdline[end]) {
      end++;
    }
    char *arg = g_strndup(cmdline + start, end - start);
    if (after_name_option && g_str_equal(arg, expected)) {
      found = TRUE;
    }
    after_name_option = g_str_equal(arg, "-name");
    g_free(arg);
    start = end + 1;
  }
  g_free(expected);
  g_free(cmdline);
  g_free(proc);
  return found;
}

static gboolean pid_exists(pid_t pid) {
  return pid > 1 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int signal_process(pid_t pid, const char *project, const char *service,
                          guint64 expected_start_time, int signal_number) {
  if (pid <= 1) {
    errno = ESRCH;
    return -1;
  }
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  int pidfd = syscall(SYS_pidfd_open, pid, 0);
  if (pidfd >= 0) {
    if (!process_running(pid, project, service, expected_start_time)) {
      close(pidfd);
      errno = ESRCH;
      return -1;
    }
    int result = syscall(SYS_pidfd_send_signal, pidfd, signal_number, NULL, 0);
    int saved_errno = errno;
    close(pidfd);
    if (result == 0 || saved_errno != ENOSYS) {
      errno = saved_errno;
      return result;
    }
  } else if (errno != ENOSYS && errno != EINVAL) {
    return -1;
  }
#endif
  /* Older kernels lack pidfds; revalidate immediately before kill(). */
  if (!process_running(pid, project, service, expected_start_time)) {
    errno = ESRCH;
    return -1;
  }
  return kill(pid, signal_number);
}

static gboolean qmp_read_line(int fd, char **line, GError **error) {
  GString *buffer = g_string_sized_new(256);
  while (buffer->len < 65536) {
    char byte;
    ssize_t count = recv(fd, &byte, 1, 0);
    if (count == 1) {
      if (byte == '\n') {
        *line = g_string_free(buffer, FALSE);
        return TRUE;
      }
      g_string_append_c(buffer, byte);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "reading QMP response failed: %s",
                  count == 0 ? "connection closed" : g_strerror(errno));
      g_string_free(buffer, TRUE);
      return FALSE;
    }
  }
  g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                      "QMP response exceeds 64 KiB");
  g_string_free(buffer, TRUE);
  return FALSE;
}

static gboolean qmp_read_reply(int fd, char **string_reply, GError **error) {
  for (;;) {
    char *line = NULL;
    if (!qmp_read_line(fd, &line, error)) {
      return FALSE;
    }
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    gboolean parsed = json_parser_load_from_data(parser, line, -1,
                                                  &parse_error);
    g_free(line);
    if (!parsed) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                  "invalid JSON from QMP: %s", parse_error->message);
      g_clear_error(&parse_error);
      g_object_unref(parser);
      return FALSE;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
      g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                          "QMP response is not a JSON object");
      g_object_unref(parser);
      return FALSE;
    }
    JsonObject *object = json_node_get_object(root);
    if (json_object_has_member(object, "event")) {
      g_object_unref(parser);
      continue;
    }
    if (json_object_has_member(object, "error")) {
      JsonObject *details = json_object_get_object_member(object, "error");
      const char *description = details &&
                                        json_object_has_member(details,
                                                               "desc")
                                    ? json_object_get_string_member(details,
                                                                    "desc")
                                    : "QMP command was rejected";
      g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                          description);
      g_object_unref(parser);
      return FALSE;
    }
    gboolean has_reply = json_object_has_member(object, "return");
    if (has_reply && string_reply) {
      JsonNode *reply = json_object_get_member(object, "return");
      if (!JSON_NODE_HOLDS_VALUE(reply) ||
          json_node_get_value_type(reply) != G_TYPE_STRING) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "QMP reply is not a string");
        g_object_unref(parser);
        return FALSE;
      }
      *string_reply = g_strdup(json_node_get_string(reply));
    }
    g_object_unref(parser);
    if (has_reply) {
      return TRUE;
    }
  }
}

static gboolean qmp_send(int fd, const char *request, GError **error) {
  gsize remaining = strlen(request);
  const char *cursor = request;
  while (remaining) {
    ssize_t sent = send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      int saved_errno = sent < 0 ? errno : EPIPE;
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                  "writing QMP request failed: %s", g_strerror(saved_errno));
      return FALSE;
    }
    cursor += sent;
    remaining -= sent;
  }
  return TRUE;
}

static gboolean qmp_command_request(const char *directory, const char *service,
                                    const char *request,
                                    char **string_reply, GError **error) {
  char *path = g_strdup_printf("%s/%s.qmp", directory, service);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof(address.sun_path)) {
    g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_NAMETOOLONG,
                        "QMP socket path exceeds the host limit");
    g_free(path);
    return FALSE;
  }
  g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
  g_free(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "cannot create QMP client socket: %s", g_strerror(errno));
    return FALSE;
  }
  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "cannot connect to QMP socket: %s", g_strerror(errno));
    close(fd);
    return FALSE;
  }
  char *greeting = NULL;
  gboolean ok = qmp_read_line(fd, &greeting, error);
  if (ok) {
    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    gboolean valid = json_parser_load_from_data(parser, greeting, -1,
                                                 &parse_error) &&
                     JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)) &&
                     json_object_has_member(
                         json_node_get_object(json_parser_get_root(parser)),
                         "QMP");
    if (!valid) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                  "invalid QMP greeting%s%s",
                  parse_error ? ": " : "",
                  parse_error ? parse_error->message : "");
      g_clear_error(&parse_error);
      ok = FALSE;
    }
    g_object_unref(parser);
    g_free(greeting);
  }
  if (ok) {
    const char capabilities[] = "{\"execute\":\"qmp_capabilities\"}\r\n";
    ok = qmp_send(fd, capabilities, error) &&
         qmp_read_reply(fd, NULL, error);
  }
  if (ok) {
    ok = qmp_send(fd, request, error) &&
         qmp_read_reply(fd, string_reply, error);
  }
  close(fd);
  return ok;
}

static gboolean qmp_command(const char *directory, const char *service,
                            const char *command, GError **error) {
  char *request = g_strdup_printf("{\"execute\":\"%s\"}\r\n", command);
  gboolean ok = qmp_command_request(directory, service, request, NULL, error);
  g_free(request);
  return ok;
}

static char *qmp_usernet_info(const char *directory, const char *service,
                              GError **error) {
  const char request[] = "{\"execute\":\"human-monitor-command\","
                         "\"arguments\":{\"command-line\":"
                         "\"info usernet\"}}\r\n";
  char *reply = NULL;
  if (!qmp_command_request(directory, service, request, &reply, error)) {
    g_free(reply);
    return NULL;
  }
  return reply;
}

static void qmp_socket_remove(const char *directory, const char *service) {
  char *path = g_strdup_printf("%s/%s.qmp", directory, service);
  g_unlink(path);
  g_free(path);
}

static char *find_program(const char *environment, const char *fallback) {
  const char *configured = g_getenv(environment);
  char *found =
      g_find_program_in_path(configured && *configured ? configured : fallback);
  if (!found) {
    fail("cannot find %s; set %s", fallback, environment);
  }
  return found;
}

static gboolean run_capture(const char *const *argv, char **stdout_text,
                            char **stderr_text) {
  gint status = 0;
  GError *error = NULL;
  guint argc = 0;
  while (argv[argc]) {
    argc++;
  }
  char **spawn_argv = g_new0(char *, argc + 1);
  for (guint i = 0; i < argc; i++) {
    spawn_argv[i] = (char *)argv[i];
  }
  gboolean spawned =
      g_spawn_sync(NULL, spawn_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                   stdout_text, stderr_text, &status, &error);
  g_free(spawn_argv);
  if (!spawned) {
    fail("cannot run %s: %s", argv[0], error->message);
    g_error_free(error);
    return FALSE;
  }
  if (!g_spawn_check_wait_status(status, &error)) {
    fail("%s failed: %s", argv[0],
         stderr_text && *stderr_text ? *stderr_text : error->message);
    g_clear_error(&error);
    return FALSE;
  }
  return TRUE;
}

static gboolean overlay_uses_base(const char *qemu_img, const char *overlay,
                                  const char *base) {
  const char *info_argv[] = {qemu_img, "info", "--output=json", overlay, NULL};
  char *info = NULL;
  if (!run_capture(info_argv, &info, NULL)) {
    g_free(info);
    return FALSE;
  }
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  gboolean valid = json_parser_load_from_data(parser, info, -1, &error);
  const char *backing = NULL;
  if (valid) {
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *object = json_node_get_object(root);
      if (json_object_has_member(object, "backing-filename")) {
        backing = json_object_get_string_member(object, "backing-filename");
      }
    }
  }
  char *canonical_base = g_canonicalize_filename(base, NULL);
  char *canonical_backing =
      backing ? g_canonicalize_filename(backing, NULL) : NULL;
  gboolean matches =
      canonical_backing && g_strcmp0(canonical_base, canonical_backing) == 0;
  g_free(canonical_base);
  g_free(canonical_backing);
  g_clear_error(&error);
  g_object_unref(parser);
  g_free(info);
  return matches;
}

static gboolean parse_memory(const char *value, guint64 *mib) {
  if (!value || !*value) {
    *mib = 1024;
    return TRUE;
  }
  char *end = NULL;
  guint64 number = g_ascii_strtoull(value, &end, 10);
  if (end == value) {
    return FALSE;
  }
  char unit = g_ascii_toupper(*end);
  if (*end && end[1] && !(end[1] == 'B' && !end[2])) {
    return FALSE;
  }
  switch (unit) {
  case 'K':
    *mib = number / (1024 * 1024);
    break;
  case 'M':
    *mib = number / 1024;
    break;
  case 'G':
    *mib = number * 1024;
    break;
  case '\0':
    *mib = number / (1024 * 1024);
    break;
  case 'B':
    *mib = number / (1024 * 1024);
    break;
  default:
    return FALSE;
  }
  return *mib > 0;
}

static gboolean parse_port_range(const char *text, guint *first, guint *last) {
  if (!text || !*text) {
    return FALSE;
  }
  gboolean dash_seen = FALSE;
  for (const char *p = text; *p; p++) {
    if (*p == '-' && !dash_seen) {
      dash_seen = TRUE;
    } else if (!g_ascii_isdigit(*p)) {
      return FALSE;
    }
  }
  char *copy = g_strdup(text);
  char *separator = strchr(copy, '-');
  if (separator) {
    *separator++ = '\0';
  }
  char *end = NULL;
  guint64 start = g_ascii_strtoull(copy, &end, 10);
  gboolean ok = end != copy && !*end && start > 0 && start <= 65535;
  guint64 finish = start;
  if (ok && separator) {
    finish = g_ascii_strtoull(separator, &end, 10);
    ok = end != separator && !*end && finish >= start && finish <= 65535;
  }
  if (ok) {
    *first = (guint)start;
    *last = (guint)finish;
  }
  g_free(copy);
  return ok;
}

static gboolean valid_host_ip(const char *host_ip) {
  if (!host_ip || !*host_ip) {
    return TRUE;
  }
  GInetAddress *address = g_inet_address_new_from_string(host_ip);
  if (!address) {
    return FALSE;
  }
  gboolean ipv4 = g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4;
  g_object_unref(address);
  return ipv4;
}

typedef struct PortBinding {
  char *host_ip;
  char *protocol;
  guint host_port;
  guint guest_port;
  gboolean dynamic;
} PortBinding;

static void port_binding_free(PortBinding *binding) {
  g_free(binding->host_ip);
  g_free(binding->protocol);
  g_free(binding);
}

static char *port_forwards(YNode *service, const char *name, gboolean *valid,
                           GPtrArray *bindings) {
  YNode *ports = map_get(service, "ports");
  GString *forwards = g_string_new(NULL);
  *valid = TRUE;
  if (!ports) {
    return g_string_free(forwards, FALSE);
  }
  if (ports->kind != NODE_SEQUENCE) {
    fail("service '%s': ports must be a sequence", name);
    *valid = FALSE;
    return g_string_free(forwards, FALSE);
  }
  guint mapping_count = 0;
  for (guint i = 0; i < ports->items->len; i++) {
    YNode *port = g_ptr_array_index(ports->items, i);
    char *host_ip = g_strdup("");
    char *host = NULL;
    char *guest = NULL;
    char *protocol = g_strdup("tcp");
    char *mode = NULL;
    if (port->kind == NODE_MAPPING) {
      YNode *v = map_get(port, "target");
      guest = g_strdup(node_string(v));
      v = map_get(port, "published");
      host = g_strdup(node_string(v));
      v = map_get(port, "host_ip");
      if (node_string(v)) {
        g_free(host_ip);
        host_ip = g_strdup(node_string(v));
      }
      v = map_get(port, "protocol");
      if (node_string(v)) {
        g_free(protocol);
        protocol = g_strdup(node_string(v));
      }
      v = map_get(port, "mode");
      mode = g_strdup(node_string(v) ? node_string(v) : "host");
      static const char *const allowed_fields[] = {
          "target", "published", "host_ip", "protocol", "mode",
          "app_protocol", "name", NULL};
      for (guint field = 0; field < port->items->len; field++) {
        YPair *pair = g_ptr_array_index(port->items, field);
        const char *key = node_string(pair->key);
        gboolean known = FALSE;
        for (const char *const *allowed = allowed_fields; *allowed; allowed++) {
          known |= g_strcmp0(key, *allowed) == 0;
        }
        if (!known) {
          fail("service '%s': unknown ports field '%s'", name,
               key ? key : "(invalid)");
          *valid = FALSE;
          break;
        }
      }
    } else if (port->kind == NODE_SCALAR) {
      char *copy = g_strdup(port->scalar);
      char *slash = strchr(copy, '/');
      if (slash) {
        *slash++ = '\0';
        g_free(protocol);
        protocol = g_strdup(slash);
      }
      char *remaining = copy;
      if (copy[0] == '[') {
        char *closing = strchr(copy, ']');
        if (closing && closing[1] == ':') {
          *closing = '\0';
          g_free(host_ip);
          host_ip = g_strdup(copy + 1);
          remaining = closing + 2;
        }
      }
      gchar **parts = g_strsplit(remaining, ":", -1);
      guint count = g_strv_length(parts);
      if (count == 1 && remaining == copy) {
        guest = g_strdup(parts[0]);
      } else if (count == 2) {
        host = g_strdup(parts[0]);
        guest = g_strdup(parts[1]);
      } else if (count == 3 && remaining == copy) {
        g_free(host_ip);
        host_ip = g_strdup(parts[0]);
        host = g_strdup(parts[1]);
        guest = g_strdup(parts[2]);
      }
      g_strfreev(parts);
      g_free(copy);
    }
    guint host_first = 0, host_last = 0, guest_first = 0, guest_last = 0;
    gboolean dynamic = !host || !*host || g_str_equal(host, "0");
    gboolean guest_valid = parse_port_range(guest, &guest_first, &guest_last);
    gboolean host_valid = dynamic ||
                          parse_port_range(host, &host_first, &host_last);
    guint guest_count = guest_valid ? guest_last - guest_first + 1 : 0;
    guint host_count = dynamic ? guest_count
                               : (host_valid ? host_last - host_first + 1 : 0);
    gboolean ranges_valid = guest_valid && host_valid;
    if (ranges_valid && !dynamic && host_count != guest_count) {
      ranges_valid = FALSE;
    }
    if (*valid &&
        (!ranges_valid || !valid_host_ip(host_ip) ||
         !(g_str_equal(protocol, "tcp") || g_str_equal(protocol, "udp")) ||
         (mode && !g_str_equal(mode, "host")))) {
      fail("service '%s': unsupported port mapping; use equal numeric "
           "HOST:GUEST[/tcp|udp] ranges, an IPv4 host_ip, and mode: host "
           "(QEMU user networking does not accept IPv6 host bindings)",
           name);
      *valid = FALSE;
    } else if (*valid) {
      if (host_count > 1024 || mapping_count > 1024 - host_count) {
        fail("service '%s': total published port count exceeds 1024", name);
        *valid = FALSE;
      }
      for (guint offset = 0; *valid && offset < host_count; offset++) {
        if (bindings) {
          PortBinding *binding = g_new0(PortBinding, 1);
          binding->host_ip = g_strdup(host_ip);
          binding->protocol = g_strdup(protocol);
          binding->host_port = dynamic ? 0 : host_first + offset;
          binding->guest_port = guest_first + offset;
          binding->dynamic = dynamic;
          g_ptr_array_add(bindings, binding);
        }
        if (forwards->len) {
          g_string_append_c(forwards, ',');
        }
        if (dynamic) {
          g_string_append_printf(forwards, "hostfwd=%s:%s:0-:%u", protocol,
                                 host_ip, guest_first + offset);
        } else {
          g_string_append_printf(forwards, "hostfwd=%s:%s:%u-:%u", protocol,
                                 host_ip, host_first + offset,
                                 guest_first + offset);
        }
        mapping_count++;
      }
    }
    g_free(host_ip);
    g_free(host);
    g_free(guest);
    g_free(protocol);
    g_free(mode);
    if (!*valid) {
      break;
    }
  }
  return g_string_free(forwards, FALSE);
}

static gboolean unsupported_service_settings(YNode *service, const char *name,
                                             gboolean oci_guest) {
  static const char *unsupported[] = {
      "build",
      "command",
      "entrypoint",
      "environment",
      "env_file",
      "volumes",
      "networks",
      "network_mode",
      "healthcheck",
      "secrets",
      "configs",
      "container_name",
      "hostname",
      "user",
      "working_dir",
      "privileged",
      "cap_add",
      "cap_drop",
      "devices",
      "tmpfs",
      "expose",
      "read_only",
      "stdin_open",
      "tty",
      "restart",
      "develop",
      NULL,
  };
  for (const char **key = unsupported; *key; key++) {
    if (oci_guest &&
      (g_str_equal(*key, "command") || g_str_equal(*key, "entrypoint") ||
         g_str_equal(*key, "environment") || g_str_equal(*key, "user") ||
         g_str_equal(*key, "working_dir") ||
         g_str_equal(*key, "env_file"))) {
      continue;
    }
    YNode *value = map_get(service, *key);
    gboolean nonempty =
        value &&
        !(value->kind == NODE_SCALAR &&
          (g_str_equal(value->scalar, "") ||
           g_str_equal(value->scalar, "false"))) &&
        !(value->kind == NODE_SEQUENCE && value->items->len == 0) &&
        !(value->kind == NODE_MAPPING && value->items->len == 0);
    if (nonempty) {
      fail("service '%s': 'up' cannot translate Compose option '%s' into QEMU "
           "VM settings yet",
           name, *key);
      return TRUE;
    }
  }
  return FALSE;
}

typedef struct QuockerBootAssets {
  const char *root_disk;
  const char *kernel_path;
  const char *initrd_path;
  const char *architecture;
  const char *manifest_digest;
  const QuockerKernel *kernel;
} QuockerBootAssets;

static gboolean run_qemu_service(const char *project, const char *root,
                                 const char *directory, const char *name,
                                 YNode *service,
                                 const QuockerBootAssets *boot) {
  gboolean oci_guest = boot != NULL;
  if (oci_guest) {
    GError *error = NULL;
    if (!quocker_kernel_verify_assets(boot->kernel, &error)) {
      fail("service '%s': selected kernel changed before VM start: %s", name,
           error ? error->message : "asset digest mismatch");
      g_clear_error(&error);
      return FALSE;
    }
  }
  if (unsupported_service_settings(service, name, oci_guest)) {
    return FALSE;
  }
  YNode *extension = map_get(service, "x-quocker");
  const char *image = node_string(map_get(extension, "image"));
  if (!image) {
    image = node_string(map_get(service, "image"));
  }
  const char *memory = node_string(map_get(service, "mem_limit"));
  if (!memory) {
    memory = node_string(map_get(service, "memory"));
  }
  const char *cpus_text = node_string(map_get(service, "cpus"));
  if (!image || !*image) {
    fail("service '%s': image must name an existing local QEMU disk image",
         name);
    return FALSE;
  }
  char *image_path =
      oci_guest ? g_strdup(boot->root_disk) : absolute_path(image, root);
  if (!g_file_test(image_path, G_FILE_TEST_IS_REGULAR)) {
    fail("service '%s': image '%s' is not a local disk image; OCI images are "
         "not bootable QEMU disks",
         name, image);
    g_free(image_path);
    return FALSE;
  }
  guint64 mem_mib;
  if (!parse_memory(memory, &mem_mib)) {
    fail("service '%s': invalid memory value", name);
    g_free(image_path);
    return FALSE;
  }
  guint cpus = cpus_text ? (guint)g_ascii_strtoull(cpus_text, NULL, 10) : 1;
  if (!cpus) {
    fail("service '%s': CPU count must be positive", name);
    g_free(image_path);
    return FALSE;
  }
  gboolean ports_valid;
  char *forwards = port_forwards(service, name, &ports_valid, NULL);
  if (!ports_valid) {
    g_free(forwards);
    g_free(image_path);
    return FALSE;
  }
  const char *qemu_default = "qemu-system-x86_64";
  if (oci_guest && g_str_equal(boot->architecture, "arm64")) {
    qemu_default = "qemu-system-aarch64";
  } else if (oci_guest && g_str_equal(boot->architecture, "386")) {
    qemu_default = "qemu-system-i386";
  } else if (oci_guest && g_str_equal(boot->architecture, "arm")) {
    qemu_default = "qemu-system-arm";
  } else if (oci_guest && g_str_equal(boot->architecture, "ppc64le")) {
    qemu_default = "qemu-system-ppc64";
  } else if (oci_guest && g_str_equal(boot->architecture, "s390x")) {
    qemu_default = "qemu-system-s390x";
  }
  char *qemu = find_program("QUOCKER_QEMU", qemu_default);
  char *qemu_img = find_program("QUOCKER_QEMU_IMG", "qemu-img");
  if (!qemu || !qemu_img) {
    g_free(qemu);
    g_free(qemu_img);
    g_free(forwards);
    g_free(image_path);
    return FALSE;
  }
  char *digest_suffix = NULL;
  if (oci_guest && g_str_has_prefix(boot->manifest_digest, "sha256:") &&
      strlen(boot->manifest_digest) == 71) {
    digest_suffix = g_strndup(boot->manifest_digest + 7, 16);
  }
  char *disk = digest_suffix ? g_strdup_printf("%s/%s-%s.qcow2", directory,
                                               name, digest_suffix)
                             : g_strdup_printf("%s/%s.qcow2", directory, name);
  g_free(digest_suffix);
  char *cache_directory = NULL;
  int cache_lock_fd = -1;
  if (oci_guest) {
    char *rootfs_parent = g_path_get_dirname(image_path);
    cache_directory = g_path_get_dirname(rootfs_parent);
    g_free(rootfs_parent);
    cache_lock_fd = quocker_oci_cache_lock(cache_directory);
    if (cache_lock_fd < 0) {
      goto error;
    }
  }
  if (!g_file_test(disk, G_FILE_TEST_EXISTS)) {
    const char *info_argv[] = {qemu_img, "info", "--output=json", image_path,
                               NULL};
    char *info = NULL;
    char *err = NULL;
    if (!run_capture(info_argv, &info, &err)) {
      g_free(info);
      g_free(err);
      goto error;
    }
    GRegex *regex =
        g_regex_new("\\\"format\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"", 0, 0, NULL);
    GMatchInfo *match = NULL;
    char *format = NULL;
    if (g_regex_match(regex, info, 0, &match)) {
      format = g_match_info_fetch(match, 1);
    }
    g_match_info_free(match);
    g_regex_unref(regex);
    g_free(info);
    g_free(err);
    if (!format) {
      fail("service '%s': qemu-img returned no image format", name);
      goto error;
    }
    const char *create_argv[] = {qemu_img, "create", "-f",       "qcow2", "-F",
                                 format,   "-b",     image_path, disk,    NULL};
    gboolean created = run_capture(create_argv, NULL, NULL);
    g_free(format);
    if (!created) {
      goto error;
    }
  } else if (!overlay_uses_base(qemu_img, disk, image_path)) {
    fail("service '%s': disk overlay uses a different base image; run "
         "'quocker down --volumes %s' before updating the image",
         name, name);
    goto error;
  }
  if (oci_guest) {
    gboolean referenced = quocker_oci_cache_reference_locked(
        cache_directory, directory, name, disk, image_path);
    if (!referenced) {
      fail("service '%s': could not protect its OCI base disk from cache "
           "pruning", name);
      goto error;
    }
  }
  char *log = g_strdup_printf("%s/%s.log", directory, name);
  char *pidfile = g_strdup_printf("%s/%s.pid", directory, name);
  char *qmp_socket = g_strdup_printf("%s/%s.qmp", directory, name);
  struct sockaddr_un qmp_address = {.sun_family = AF_UNIX};
  if (strlen(qmp_socket) >= sizeof(qmp_address.sun_path)) {
    fail("service '%s': QMP socket path exceeds the host limit", name);
    g_free(log);
    g_free(pidfile);
    g_free(qmp_socket);
    goto error;
  }
  g_unlink(pidfile);
  g_unlink(qmp_socket);
  char *qname = g_strdup_printf("%s-%s", project, name);
  char *mem_arg = g_strdup_printf("%" G_GUINT64_FORMAT, mem_mib);
  char *cpu_arg = g_strdup_printf("%u", cpus);
  char *drive = g_strdup_printf("file=%s,if=virtio,format=qcow2", disk);
  char *serial = g_strdup_printf("file:%s", log);
  char *disk_basename = g_path_get_basename(disk);
  char *boot_arguments =
      oci_guest ? g_strdup("console=ttyS0 panic=1 ip=dhcp") : NULL;
  char *netdev =
      *forwards ? g_strdup_printf("user,id=quocker-net,%s", forwards) : NULL;
  const char *qargv[48];
  guint q = 0;
  qargv[q++] = qemu;
  qargv[q++] = "-name";
  qargv[q++] = qname;
  qargv[q++] = "-m";
  qargv[q++] = mem_arg;
  qargv[q++] = "-smp";
  qargv[q++] = cpu_arg;
  qargv[q++] = "-drive";
  qargv[q++] = drive;
  if (oci_guest) {
    if (g_str_equal(boot->architecture, "arm64") ||
        g_str_equal(boot->architecture, "arm")) {
      qargv[q++] = "-machine";
      qargv[q++] = "virt";
      qargv[q++] = "-cpu";
      qargv[q++] = "max";
    }
    qargv[q++] = "-kernel";
    qargv[q++] = boot->kernel_path;
    qargv[q++] = "-initrd";
    qargv[q++] = boot->initrd_path;
    qargv[q++] = "-append";
    qargv[q++] = boot_arguments;
  }
  qargv[q++] = "-display";
  qargv[q++] = "none";
  qargv[q++] = "-serial";
  qargv[q++] = serial;
  qargv[q++] = "-monitor";
  qargv[q++] = "none";
  qargv[q++] = "-qmp";
  char *qmp_option = g_strdup_printf("unix:%s,server=on,wait=off", qmp_socket);
  qargv[q++] = qmp_option;
  if (netdev) {
    qargv[q++] = "-netdev";
    qargv[q++] = netdev;
    qargv[q++] = "-device";
    qargv[q++] = "virtio-net-pci,netdev=quocker-net";
  } else {
    qargv[q++] = "-nic";
    qargv[q++] = "user,model=virtio-net-pci";
  }
  qargv[q++] = "-daemonize";
  qargv[q++] = "-pidfile";
  qargv[q++] = pidfile;
  qargv[q] = NULL;
  if (!run_capture(qargv, NULL, NULL)) {
    g_free(qmp_option);
    g_free(qmp_socket);
    g_free(log);
    g_free(pidfile);
    g_free(qname);
    g_free(mem_arg);
    g_free(cpu_arg);
    g_free(drive);
    g_free(serial);
    g_free(netdev);
    g_free(forwards);
    g_free(disk_basename);
    g_free(boot_arguments);
    goto error;
  }
  g_free(qmp_option);
  gchar *pid_contents = NULL;
  if (!g_file_get_contents(pidfile, &pid_contents, NULL, NULL)) {
    fail("service '%s': QEMU started without writing its PID file", name);
    g_free(log);
    g_free(pidfile);
    g_free(qmp_socket);
    g_free(qname);
    g_free(mem_arg);
    g_free(cpu_arg);
    g_free(drive);
    g_free(serial);
    g_free(netdev);
    g_free(forwards);
    g_free(disk_basename);
    g_free(boot_arguments);
    goto error;
  }
  g_strchomp(pid_contents);
  char *state = state_path(directory, name);
  char *qname_state = g_strdup_printf("%s-%s", project, name);
  pid_t qemu_pid = (pid_t)g_ascii_strtoll(pid_contents, NULL, 10);
  guint64 qemu_start_time = process_start_time(qemu_pid);
  char *state_contents =
      g_strdup_printf("%s\n%s\n%s\n%" G_GUINT64_FORMAT "\n", pid_contents,
                      qname_state, disk_basename, qemu_start_time);
  gboolean saved = g_file_set_contents(state, state_contents, -1, NULL);
  if (saved) {
    g_print("[%s] %s: started (pid %s)", project, name, pid_contents);
  }
  g_free(state_contents);
  g_free(qname_state);
  g_free(disk_basename);
  g_free(boot_arguments);
  g_free(state);
  g_free(pid_contents);
  g_free(log);
  g_free(pidfile);
  g_free(qmp_socket);
  g_free(qname);
  g_free(mem_arg);
  g_free(cpu_arg);
  g_free(drive);
  g_free(serial);
  g_free(netdev);
  g_free(forwards);
  g_free(disk);
  if (cache_lock_fd >= 0) {
    quocker_oci_cache_unlock(cache_lock_fd);
  }
  g_free(cache_directory);
  g_free(qemu);
  g_free(qemu_img);
  g_free(image_path);
  return saved;
error:
  g_free(disk);
  if (cache_lock_fd >= 0) {
    quocker_oci_cache_unlock(cache_lock_fd);
  }
  g_free(cache_directory);
  g_free(qemu);
  g_free(qemu_img);
  g_free(image_path);
  return FALSE;
}

typedef struct PullContext {
  const char *cache_directory;
  const char *mirror_url;
  const char *kernel_catalog;
  const char *kernel_public_key;
} PullContext;

static gboolean pull_one(const char *name, YNode *service, void *opaque) {
  PullContext *context = opaque;
  const char *image = node_string(map_get(service, "image"));
  YNode *extension = map_get(service, "x-quocker");
  const char *extension_image = node_string(map_get(extension, "image"));
  if (extension_image) {
    image = extension_image;
  }
  if (!image || !*image) {
    fail("service '%s': pull requires an image reference", name);
    return FALSE;
  }
  if (g_file_test(image, G_FILE_TEST_IS_REGULAR) || g_path_is_absolute(image) ||
      g_str_has_prefix(image, "./") || g_str_has_prefix(image, "../")) {
    g_print("Image %s is a local VM disk; skipping registry pull\n", image);
    return TRUE;
  }
  const char *platform = node_string(map_get(service, "platform"));
  QuockerOciImage *oci_image = NULL;
  gboolean ok = quocker_oci_pull(image, platform, context->cache_directory,
                                 context->mirror_url, &oci_image);
  int cache_lock_fd =
      ok ? quocker_oci_cache_lock(context->cache_directory) : -1;
  if (ok && cache_lock_fd < 0) {
    ok = FALSE;
  }
  QuockerImageDefaults *defaults = oci_image ? oci_image->defaults : NULL;
  if (ok && defaults) {
    g_print("Image runtime defaults: Entrypoint %u arg(s), Cmd %u arg(s), "
            "Env %u variable(s), WorkingDir %s, User %s, Volumes %u\n",
            defaults->entrypoint->len, defaults->command->len,
            g_hash_table_size(defaults->environment),
            defaults->working_directory ? defaults->working_directory
                                        : "(unset)",
            defaults->user ? defaults->user : "(unset)",
            g_hash_table_size(defaults->volumes));
    if (defaults->kernel_id || defaults->kernel_minimum ||
        defaults->kernel_features->len ||
        defaults->kernel_module_releases->len) {
      g_print("Image kernel requirements: id %s, minimum %s, %u feature(s), "
              "%u required module release(s)\n",
              defaults->kernel_id ? defaults->kernel_id : "(unset)",
              defaults->kernel_minimum ? defaults->kernel_minimum : "(unset)",
              defaults->kernel_features->len,
              defaults->kernel_module_releases->len);
    }
  }
  if (ok && oci_image) {
    if (!defaults) {
      fail("service '%s': OCI image defaults are unavailable", name);
      quocker_oci_cache_unlock(cache_lock_fd);
      quocker_oci_image_free(oci_image);
      return FALSE;
    }
    QuockerDistroInfo distro = {0};
    gboolean have_hints =
        quocker_rootfs_detect_distro(oci_image->rootfs_path, &distro);
    const char *kernel_id = defaults->kernel_id;
    const char *kernel_minimum = defaults->kernel_minimum;
    GPtrArray *required_features = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *required_modules = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < defaults->kernel_features->len; i++) {
      g_ptr_array_add(required_features, g_strdup(g_ptr_array_index(
                                             defaults->kernel_features, i)));
    }
    for (guint i = 0; i < defaults->kernel_module_releases->len; i++) {
      g_ptr_array_add(required_modules,
                      g_strdup(g_ptr_array_index(
                          defaults->kernel_module_releases, i)));
    }
    YNode *kernel_config = map_get(map_get(service, "x-quocker"), "kernel");
    gboolean requirements_valid =
        !kernel_config || kernel_config->kind == NODE_MAPPING;
    if (kernel_config && kernel_config->kind == NODE_MAPPING) {
      for (guint i = 0; i < kernel_config->items->len; i++) {
        YPair *pair = g_ptr_array_index(kernel_config->items, i);
        const char *key = node_string(pair->key);
        if (!key || (!g_str_equal(key, "id") && !g_str_equal(key, "minimum") &&
                     !g_str_equal(key, "require") &&
                     !g_str_equal(key, "module_releases"))) {
          requirements_valid = FALSE;
        }
      }
      YNode *id_node = map_get(kernel_config, "id");
      YNode *minimum_node = map_get(kernel_config, "minimum");
      const char *override_id = node_string(map_get(kernel_config, "id"));
      const char *override_minimum =
          node_string(map_get(kernel_config, "minimum"));
      requirements_valid &=
          (!id_node || override_id) && (!minimum_node || override_minimum);
      if (override_id) {
        kernel_id = override_id;
      }
      if (override_minimum) {
        kernel_minimum = override_minimum;
      }
      YNode *required = map_get(kernel_config, "require");
      if (required && required->kind == NODE_SEQUENCE) {
        for (guint i = 0; i < required->items->len; i++) {
          const char *feature =
              node_string(g_ptr_array_index(required->items, i));
          if (feature && *feature) {
            g_ptr_array_add(required_features, g_strdup(feature));
          } else {
            requirements_valid = FALSE;
          }
        }
      } else if (required && required->kind == NODE_SCALAR) {
        gchar **tokens = g_strsplit(required->scalar, ",", -1);
        for (guint i = 0; tokens[i]; i++) {
          char *feature = g_strstrip(tokens[i]);
          if (*feature) {
            g_ptr_array_add(required_features, g_strdup(feature));
          }
        }
        g_strfreev(tokens);
      } else if (required) {
        requirements_valid = FALSE;
      }
      YNode *module_releases = map_get(kernel_config, "module_releases");
      if (module_releases &&
          !append_kernel_module_releases(required_modules, module_releases,
                                         name)) {
        requirements_valid = FALSE;
      }
    }
    char *image_platform = platform ? g_strdup(platform)
                                    : g_strdup_printf("%s/%s", oci_image->os,
                                                      oci_image->architecture);
    if (!requirements_valid) {
      fail("service '%s': x-quocker.kernel must contain scalar id/minimum "
           "and a scalar or sequence of required features",
           name);
      g_free(image_platform);
      g_ptr_array_free(required_features, TRUE);
      g_ptr_array_free(required_modules, TRUE);
      quocker_distro_info_clear(&distro);
      quocker_oci_cache_unlock(cache_lock_fd);
      quocker_oci_image_free(oci_image);
      return FALSE;
    }
    if (g_file_test(context->kernel_catalog, G_FILE_TEST_IS_REGULAR)) {
      QuockerKernel *kernel = NULL;
      GError *error = NULL;
      ok = quocker_kernel_select_for_image(
          context->kernel_catalog, context->kernel_public_key, image_platform,
          have_hints ? &distro : NULL, kernel_id, kernel_minimum,
          required_features, required_modules, &kernel, &error);
      if (ok) {
        g_print("Selected kernel %s%s%s%s for %s: %s\n", kernel->id,
                kernel->version ? " (" : "",
                kernel->version ? kernel->version : "",
                kernel->version ? ")" : "", image, kernel->reason);
        quocker_kernel_free(kernel);
      } else {
        fail("service '%s': kernel selection failed: %s", name,
             error ? error->message : "no compatible catalog entry");
      }
      g_clear_error(&error);
    } else {
      g_print("Kernel selection deferred: catalog %s is unavailable\n",
              context->kernel_catalog);
    }
    g_free(image_platform);
    g_ptr_array_free(required_features, TRUE);
    g_ptr_array_free(required_modules, TRUE);
    quocker_distro_info_clear(&distro);
  }
  if (ok && oci_image) {
    char *disk_path = g_strdup_printf("%s.ext4", oci_image->rootfs_path);
    struct stat disk_stat;
    if (lstat(disk_path, &disk_stat) == 0) {
      if (!S_ISREG(disk_stat.st_mode)) {
        fail("service '%s': cached VM disk is not a regular file", name);
        ok = FALSE;
      } else {
        g_print("Prepared VM root disk: %s (cached raw ext4)\n", disk_path);
      }
    } else if (errno != ENOENT) {
      fail("service '%s': cannot inspect VM disk cache: %s", name,
           g_strerror(errno));
      ok = FALSE;
    } else {
      guint64 estimated_size = 0;
      GError *error = NULL;
      if (!quocker_rootfs_ext4_estimate(oci_image->rootfs_path, &estimated_size,
                                        &error)) {
        fail("service '%s': cannot estimate guest disk: %s", name,
             error ? error->message : "invalid root filesystem");
        g_clear_error(&error);
        ok = FALSE;
      } else if (!quocker_oci_cache_has_room(context->cache_directory,
                                             estimated_size)) {
        ok = FALSE;
      } else if (!quocker_rootfs_to_ext4(oci_image->rootfs_path, disk_path,
                                         &error)) {
        fail("service '%s': guest disk conversion failed: %s", name,
             error ? error->message : "conversion failed");
        g_clear_error(&error);
        ok = FALSE;
      } else {
        g_print("Prepared VM root disk: %s (raw ext4, %" G_GUINT64_FORMAT
                " MiB virtual size)\n",
                disk_path, estimated_size / (1024 * 1024));
      }
    }
    g_free(disk_path);
  }
  quocker_oci_cache_unlock(cache_lock_fd);
  quocker_oci_image_free(oci_image);
  return ok;
}

static gboolean yaml_null(const YNode *node) {
  return node && node->kind == NODE_SCALAR &&
         g_str_equal(node->tag, "tag:yaml.org,2002:null");
}

static gboolean valid_environment_name(const char *name) {
  return quocker_env_name_valid(name);
}

static gboolean parse_argument_override(YNode *node, const char *service_name,
                                        const char *field,
                                        GPtrArray **arguments_out) {
  *arguments_out = NULL;
  if (!node || yaml_null(node)) {
    return TRUE;
  }
  GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      const char *value = node_string(g_ptr_array_index(node->items, i));
      if (!value) {
        fail("service '%s': %s entries must be strings", service_name, field);
        g_ptr_array_free(arguments, TRUE);
        return FALSE;
      }
      g_ptr_array_add(arguments, g_strdup(value));
    }
  } else if (node->kind == NODE_SCALAR) {
    gint count = 0;
    gchar **parsed = NULL;
    GError *error = NULL;
    if (!g_shell_parse_argv(node->scalar, &count, &parsed, &error)) {
      fail("service '%s': cannot parse %s: %s", service_name, field,
           error->message);
      g_clear_error(&error);
      g_ptr_array_free(arguments, TRUE);
      return FALSE;
    }
    for (gint i = 0; i < count; i++) {
      g_ptr_array_add(arguments, g_strdup(parsed[i]));
    }
    g_strfreev(parsed);
  } else {
    fail("service '%s': %s must be a string or string sequence", service_name,
         field);
    g_ptr_array_free(arguments, TRUE);
    return FALSE;
  }
  *arguments_out = arguments;
  return TRUE;
}

static gboolean add_environment_override(GHashTable *environment,
                                         const char *entry,
                                         const char *service_name) {
  const char *equals = strchr(entry, '=');
  char *name = equals ? g_strndup(entry, equals - entry) : g_strdup(entry);
  if (!valid_environment_name(name)) {
    fail("service '%s': environment contains an invalid variable name",
         service_name);
    g_free(name);
    return FALSE;
  }
  const char *value = equals ? equals + 1 : g_getenv(name);
  g_hash_table_replace(environment, name, g_strdup(value));
  return TRUE;
}

static gboolean parse_environment_override(YNode *node,
                                           const char *service_name,
                                           GHashTable **environment_out) {
  *environment_out =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!node || yaml_null(node)) {
    return TRUE;
  }
  if (node->kind == NODE_MAPPING) {
    for (guint i = 0; i < node->items->len; i++) {
      YPair *pair = g_ptr_array_index(node->items, i);
      const char *name = node_string(pair->key);
      const char *value = node_string(pair->value);
      if (!name || (!value && !yaml_null(pair->value)) ||
          !valid_environment_name(name)) {
        fail("service '%s': environment mapping must contain valid string "
             "names and scalar values",
             service_name);
        g_hash_table_destroy(*environment_out);
        *environment_out = NULL;
        return FALSE;
      }
      const char *resolved = yaml_null(pair->value) ? g_getenv(name) : value;
      g_hash_table_replace(*environment_out, g_strdup(name),
                           g_strdup(resolved));
    }
    return TRUE;
  }
  if (node->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < node->items->len; i++) {
      const char *entry = node_string(g_ptr_array_index(node->items, i));
      if (!entry ||
          !add_environment_override(*environment_out, entry, service_name)) {
        if (entry == NULL) {
          fail("service '%s': environment sequence entries must be strings",
               service_name);
        }
        g_hash_table_destroy(*environment_out);
        *environment_out = NULL;
        return FALSE;
      }
    }
    return TRUE;
  }
  fail("service '%s': environment must be a mapping or string sequence",
       service_name);
  g_hash_table_destroy(*environment_out);
  *environment_out = NULL;
  return FALSE;
}

static GHashTable *environment_copy(GHashTable *source) {
  GHashTable *copy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_free);
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, source);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_hash_table_insert(copy, g_strdup(key), g_strdup(value));
  }
  return copy;
}

static gboolean env_file_required(YNode *node, gboolean *required,
                                  const char *service_name) {
  *required = TRUE;
  if (!node) {
    return TRUE;
  }
  const char *value = node_string(node);
  if (!value || (g_strcmp0(value, "true") != 0 &&
                 g_strcmp0(value, "false") != 0)) {
    fail("service '%s': env_file.required must be a boolean", service_name);
    return FALSE;
  }
  *required = g_str_equal(value, "true");
  return TRUE;
}

static gboolean read_service_env_file(YNode *spec, const char *project_root,
                                      const char *service_name,
                                      GHashTable *values,
                                      GHashTable *interpolation_environment) {
  const char *path = NULL;
  gboolean required = TRUE;
  gboolean raw = FALSE;
  if (spec->kind == NODE_SCALAR) {
    path = node_string(spec);
  } else if (spec->kind == NODE_MAPPING) {
    for (guint i = 0; i < spec->items->len; i++) {
      YPair *pair = g_ptr_array_index(spec->items, i);
      const char *key = node_string(pair->key);
      if (!key || (!g_str_equal(key, "path") &&
                   !g_str_equal(key, "required") &&
                   !g_str_equal(key, "format"))) {
        fail("service '%s': env_file mapping has an unknown field",
             service_name);
        return FALSE;
      }
    }
    path = node_string(map_get(spec, "path"));
    if (!env_file_required(map_get(spec, "required"), &required,
                           service_name)) {
      return FALSE;
    }
    const char *format = node_string(map_get(spec, "format"));
    if (format && !g_str_equal(format, "raw")) {
      fail("service '%s': env_file.format must be 'raw' when specified",
           service_name);
      return FALSE;
    }
    raw = format != NULL;
  } else {
    fail("service '%s': env_file entries must be paths or mappings",
         service_name);
    return FALSE;
  }
  if (!path || !*path) {
    fail("service '%s': env_file.path must be a non-empty string",
         service_name);
    return FALSE;
  }
  char *absolute = absolute_path(path, project_root);
  GError *error = NULL;
  gboolean ok = quocker_env_file_read(
      values, absolute, !required, interpolation_environment, FALSE, !raw,
      raw, interpolate_text, &error);
  if (!ok) {
    fail("service '%s': %s", service_name,
         error ? error->message : "cannot load env_file");
  }
  g_clear_error(&error);
  g_free(absolute);
  return ok;
}

static gboolean parse_service_env_files(YNode *node, const char *project_root,
                                        const char *service_name,
                                        GHashTable *project_environment,
                                        GHashTable **values_out) {
  *values_out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GHashTable *lookup = environment_copy(project_environment);
  gboolean ok = TRUE;
  if (node && !yaml_null(node)) {
    if (node->kind == NODE_SCALAR) {
      ok = read_service_env_file(node, project_root, service_name, *values_out,
                                 lookup);
    } else if (node->kind == NODE_SEQUENCE) {
      for (guint i = 0; ok && i < node->items->len; i++) {
        ok = read_service_env_file(g_ptr_array_index(node->items, i),
                                   project_root, service_name, *values_out,
                                   lookup);
      }
    } else {
      fail("service '%s': env_file must be a path, sequence, or mapping",
           service_name);
      ok = FALSE;
    }
  }
  g_hash_table_destroy(lookup);
  if (!ok) {
    g_hash_table_destroy(*values_out);
    *values_out = NULL;
  }
  return ok;
}

static gboolean compose_runtime_config(YNode *service,
                                       const QuockerImageDefaults *defaults,
                                       const char *service_name,
                                       const char *project_root,
                                       GHashTable *project_environment,
                                       QuockerRuntimeConfig **runtime_out) {
  GPtrArray *entrypoint = NULL;
  GPtrArray *command = NULL;
  GHashTable *environment = NULL;
  GError *error = NULL;
  GHashTable *env_file_values = NULL;
  if (!parse_argument_override(map_get(service, "entrypoint"), service_name,
                               "entrypoint", &entrypoint) ||
      !parse_argument_override(map_get(service, "command"), service_name,
                               "command", &command) ||
      !parse_service_env_files(map_get(service, "env_file"), project_root,
                               service_name, project_environment,
                               &env_file_values) ||
      !parse_environment_override(map_get(service, "environment"), service_name,
                                  &environment)) {
    if (entrypoint) {
      g_ptr_array_free(entrypoint, TRUE);
    }
    if (command) {
      g_ptr_array_free(command, TRUE);
    }
    if (environment) {
      g_hash_table_destroy(environment);
    }
    if (env_file_values) {
      g_hash_table_destroy(env_file_values);
    }
    return FALSE;
  }
  GHashTableIter env_iter;
  gpointer env_key;
  gpointer env_value;
  g_hash_table_iter_init(&env_iter, env_file_values);
  while (g_hash_table_iter_next(&env_iter, &env_key, &env_value)) {
    if (!g_hash_table_contains(environment, env_key)) {
      g_hash_table_replace(environment, g_strdup(env_key),
                           g_strdup(env_value));
    }
  }
  const char *working_directory = node_string(map_get(service, "working_dir"));
  const char *user = node_string(map_get(service, "user"));
  gboolean ok = quocker_runtime_config_merge(defaults, entrypoint, command,
                                             environment, working_directory,
                                             user, runtime_out, &error);
  if (!ok) {
    fail("service '%s': invalid guest runtime config: %s", service_name,
         error ? error->message : "invalid overrides");
  }
  g_clear_error(&error);
  if (entrypoint) {
    g_ptr_array_free(entrypoint, TRUE);
  }
  if (command) {
    g_ptr_array_free(command, TRUE);
  }
  g_hash_table_destroy(environment);
  g_hash_table_destroy(env_file_values);
  return ok;
}

static gboolean
image_kernel_requirements(YNode *service, const QuockerImageDefaults *defaults,
                          const char **kernel_id_out, const char **minimum_out,
                          GPtrArray **features_out,
                          GPtrArray **module_releases_out,
                          const char *service_name) {
  *kernel_id_out = defaults->kernel_id;
  *minimum_out = defaults->kernel_minimum;
  *features_out = g_ptr_array_new_with_free_func(g_free);
  *module_releases_out = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(*features_out, g_strdup("ext4"));
  g_ptr_array_add(*features_out, g_strdup("virtio_blk"));
  g_ptr_array_add(*features_out, g_strdup("virtio_net"));
  g_ptr_array_add(*features_out, g_strdup("ip_pnp"));
  for (guint i = 0; i < defaults->kernel_features->len; i++) {
    g_ptr_array_add(*features_out,
                    g_strdup(g_ptr_array_index(defaults->kernel_features, i)));
  }
  for (guint i = 0; i < defaults->kernel_module_releases->len; i++) {
    g_ptr_array_add(*module_releases_out,
                    g_strdup(g_ptr_array_index(
                        defaults->kernel_module_releases, i)));
  }
  YNode *kernel = map_get(map_get(service, "x-quocker"), "kernel");
  if (!kernel) {
    return TRUE;
  }
  if (kernel->kind != NODE_MAPPING) {
    fail("service '%s': x-quocker.kernel must be a mapping", service_name);
    return FALSE;
  }
  for (guint i = 0; i < kernel->items->len; i++) {
    YPair *pair = g_ptr_array_index(kernel->items, i);
    const char *key = node_string(pair->key);
    if (!key || (!g_str_equal(key, "id") && !g_str_equal(key, "minimum") &&
                 !g_str_equal(key, "require") &&
                 !g_str_equal(key, "module_releases"))) {
      fail("service '%s': x-quocker.kernel has an unknown field", service_name);
      return FALSE;
    }
  }
  YNode *id_node = map_get(kernel, "id");
  YNode *minimum_node = map_get(kernel, "minimum");
  const char *kernel_id = node_string(id_node);
  const char *minimum = node_string(minimum_node);
  if ((id_node && !kernel_id) || (minimum_node && !minimum)) {
    fail("service '%s': x-quocker.kernel id/minimum must be strings",
         service_name);
    return FALSE;
  }
  if (kernel_id) {
    *kernel_id_out = kernel_id;
  }
  if (minimum) {
    *minimum_out = minimum;
  }
  YNode *required = map_get(kernel, "require");
  YNode *module_releases = map_get(kernel, "module_releases");
  if (module_releases &&
      !append_kernel_module_releases(*module_releases_out, module_releases,
                                     service_name)) {
    return FALSE;
  }
  if (!required) {
    return TRUE;
  }
  if (required->kind == NODE_SEQUENCE) {
    for (guint i = 0; i < required->items->len; i++) {
      const char *feature = node_string(g_ptr_array_index(required->items, i));
      if (!feature || !*feature) {
        fail("service '%s': x-quocker.kernel.require entries must be strings",
             service_name);
        return FALSE;
      }
      g_ptr_array_add(*features_out, g_strdup(feature));
    }
  } else if (required->kind == NODE_SCALAR) {
    gchar **tokens = g_strsplit(required->scalar, ",", -1);
    for (guint i = 0; tokens[i]; i++) {
      char *feature = g_strstrip(tokens[i]);
      if (!*feature) {
        g_strfreev(tokens);
        fail("service '%s': x-quocker.kernel.require has an empty feature",
             service_name);
        return FALSE;
      }
      g_ptr_array_add(*features_out, g_strdup(feature));
    }
    g_strfreev(tokens);
  } else {
    fail("service '%s': x-quocker.kernel.require must be a string or sequence",
         service_name);
    return FALSE;
  }
  return TRUE;
}

static char *find_guest_init(void) {
  const char *configured = g_getenv("QUOCKER_GUEST_INIT");
  if (configured && *configured &&
      g_file_test(configured, G_FILE_TEST_IS_REGULAR)) {
    return g_canonicalize_filename(configured, NULL);
  }
  char executable_path[PATH_MAX + 1];
  ssize_t length = readlink("/proc/self/exe", executable_path, PATH_MAX);
  if (length > 0 && length < PATH_MAX) {
    executable_path[length] = '\0';
    char *directory = g_path_get_dirname(executable_path);
    char *candidate = g_build_filename(directory, "quocker-guest-init", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      g_free(directory);
      return candidate;
    }
    g_free(candidate);
    candidate = g_build_filename(directory, "..", "libexec", "quocker",
                                 "quocker-guest-init", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      char *resolved = g_canonicalize_filename(candidate, NULL);
      g_free(candidate);
      g_free(directory);
      return resolved;
    }
    g_free(candidate);
    candidate = g_build_filename(directory, "..", "lib", "quocker",
                                 "quocker-guest-init", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      char *resolved = g_canonicalize_filename(candidate, NULL);
      g_free(candidate);
      g_free(directory);
      return resolved;
    }
    g_free(candidate);
    g_free(directory);
  }
  return g_find_program_in_path("quocker-guest-init");
}

static gboolean prepare_oci_boot(const char *name, const char *directory,
                                 const char *project_root, YNode *service,
                                 PullContext *context,
                                 GHashTable *project_environment,
                                 QuockerOciImage **image_out,
                                 QuockerKernel **kernel_out,
                                 QuockerRuntimeConfig **runtime_out,
                                 char **initrd_out) {
  *image_out = NULL;
  *kernel_out = NULL;
  *runtime_out = NULL;
  *initrd_out = NULL;
  const char *image_reference = node_string(map_get(service, "image"));
  const char *extension_image =
      node_string(map_get(map_get(service, "x-quocker"), "image"));
  if (extension_image) {
    image_reference = extension_image;
  }
  const char *platform = node_string(map_get(service, "platform"));
  QuockerOciImage *image = NULL;
  if (!image_reference ||
      !quocker_oci_pull(image_reference, platform, context->cache_directory,
                        context->mirror_url, &image)) {
    return FALSE;
  }
  int cache_lock_fd = quocker_oci_cache_lock(context->cache_directory);
  if (cache_lock_fd < 0) {
    quocker_oci_image_free(image);
    return FALSE;
  }
  gboolean ok = FALSE;
  QuockerImageDefaults *defaults = image->defaults;
  QuockerDistroInfo distro = {0};
  GPtrArray *features = NULL;
  GPtrArray *module_releases = NULL;
  const char *kernel_id = NULL;
  const char *minimum = NULL;
  GError *error = NULL;
  if (!defaults || g_hash_table_size(defaults->volumes)) {
    fail("service '%s': OCI Volumes require explicit VM volume support and "
         "are not booted yet",
         name);
    goto done;
  }
  if (!compose_runtime_config(service, defaults, name, project_root,
                             project_environment, runtime_out)) {
    goto done;
  }
  if (!context->kernel_catalog ||
      !g_file_test(context->kernel_catalog, G_FILE_TEST_IS_REGULAR)) {
    fail("service '%s': signed kernel catalog %s is unavailable", name,
         context->kernel_catalog ? context->kernel_catalog : "(unset)");
    goto done;
  }
  if (!image_kernel_requirements(service, defaults, &kernel_id, &minimum,
                                 &features, &module_releases, name)) {
    goto done;
  }
  gboolean have_distro =
      quocker_rootfs_detect_distro(image->rootfs_path, &distro);
  char *image_platform =
      platform ? g_strdup(platform)
               : g_strdup_printf("%s/%s", image->os, image->architecture);
  ok = quocker_kernel_select_for_image(
      context->kernel_catalog, context->kernel_public_key, image_platform,
      have_distro ? &distro : NULL, kernel_id, minimum, features,
      module_releases, kernel_out, &error);
  g_free(image_platform);
  if (!ok) {
    fail("service '%s': kernel selection failed: %s", name,
         error ? error->message : "no compatible signed kernel");
    goto done;
  }
  if (!quocker_kernel_fetch_assets(*kernel_out, &error) ||
      !quocker_kernel_verify_assets(*kernel_out, &error)) {
    fail("service '%s': selected kernel failed digest verification: %s", name,
         error ? error->message : "asset verification failed");
    ok = FALSE;
    goto done;
  }
  char *root_disk = g_strdup_printf("%s.ext4", image->rootfs_path);
  struct stat disk_stat;
  int disk_status = lstat(root_disk, &disk_stat);
  if (disk_status < 0 && errno != ENOENT) {
    fail("service '%s': cannot inspect cached guest root disk: %s", name,
         g_strerror(errno));
    g_free(root_disk);
    ok = FALSE;
    goto done;
  }
  if (disk_status < 0) {
    guint64 estimated_size = 0;
    if (!quocker_rootfs_ext4_estimate(image->rootfs_path, &estimated_size,
                                      &error) ||
        !quocker_oci_cache_has_room(context->cache_directory, estimated_size) ||
        !quocker_rootfs_to_ext4(image->rootfs_path, root_disk, &error)) {
      fail("service '%s': cannot prepare guest root disk: %s", name,
           error ? error->message : "disk cache quota exceeded");
      g_free(root_disk);
      g_clear_error(&error);
      ok = FALSE;
      goto done;
    }
  } else if (!S_ISREG(disk_stat.st_mode)) {
    fail("service '%s': cached guest root disk is not a regular file", name);
    g_free(root_disk);
    ok = FALSE;
    goto done;
  }
  g_free(root_disk);
  char *guest_init = find_guest_init();
  if (!guest_init) {
    fail("cannot locate statically linked quocker-guest-init; set "
         "QUOCKER_GUEST_INIT");
    ok = FALSE;
    goto done;
  }
  char *initrd_path = g_strdup_printf("%s/%s.initrd", directory, name);
  struct stat initrd_stat;
  if (lstat(initrd_path, &initrd_stat) == 0) {
    if (!S_ISREG(initrd_stat.st_mode) || g_unlink(initrd_path) < 0) {
      fail("service '%s': cannot safely replace the previous guest initrd",
           name);
      g_free(initrd_path);
      g_free(guest_init);
      ok = FALSE;
      goto done;
    }
  } else if (errno != ENOENT) {
    fail("service '%s': cannot inspect guest initrd output: %s", name,
         g_strerror(errno));
    g_free(initrd_path);
    g_free(guest_init);
    ok = FALSE;
    goto done;
  }
  ok = quocker_initrd_build(
      (*kernel_out)->initrd_path, (*kernel_out)->initrd_digest, guest_init,
      image->architecture, *runtime_out, initrd_path, &error);
  g_free(guest_init);
  if (!ok) {
    fail("service '%s': cannot assemble guest initrd: %s", name,
         error ? error->message : "initrd assembly failed");
    g_free(initrd_path);
    g_clear_error(&error);
    goto done;
  }
  *image_out = image;
  *initrd_out = initrd_path;
  image = NULL;
done:
  g_clear_error(&error);
  if (features) {
    g_ptr_array_free(features, TRUE);
  }
  if (module_releases) {
    g_ptr_array_free(module_releases, TRUE);
  }
  quocker_distro_info_clear(&distro);
  quocker_oci_cache_unlock(cache_lock_fd);
  quocker_oci_image_free(image);
  if (!ok) {
    quocker_kernel_free(*kernel_out);
    *kernel_out = NULL;
    quocker_runtime_config_free(*runtime_out);
    *runtime_out = NULL;
  }
  return ok;
}

static gboolean service_profile_enabled(YNode *service, Options *opts) {
  YNode *profiles = map_get(service, "profiles");
  guint count = 0;
  if (profiles && profiles->kind == NODE_SEQUENCE) {
    count = profiles->items->len;
  } else if (profiles && profiles->kind == NODE_SCALAR) {
    count = 1;
  }
  if (!count) {
    return TRUE;
  }
  for (guint i = 0; i < count; i++) {
    const char *profile =
        profiles->kind == NODE_SEQUENCE
            ? node_string(g_ptr_array_index(profiles->items, i))
            : node_string(profiles);
    for (guint j = 0; j < opts->profiles->len; j++) {
      const char *enabled = g_ptr_array_index(opts->profiles, j);
      if (g_strcmp0(profile, enabled) == 0 || g_str_equal(enabled, "*")) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static gboolean service_requested(Options *opts, const char *name) {
  if (opts->services->len == 0) {
    return TRUE;
  }
  for (guint i = 0; i < opts->services->len; i++) {
    if (g_strcmp0(name, g_ptr_array_index(opts->services, i)) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static void add_service_profiles(GHashTable *active_profiles, YNode *service) {
  YNode *profiles = map_get(service, "profiles");
  if (!profiles) {
    return;
  }
  guint count = profiles->kind == NODE_SEQUENCE ? profiles->items->len : 1;
  for (guint i = 0; i < count; i++) {
    YNode *profile = profiles->kind == NODE_SEQUENCE
                         ? g_ptr_array_index(profiles->items, i)
                         : profiles;
    const char *name = node_string(profile);
    if (name && *name) {
      g_hash_table_add(active_profiles, g_strdup(name));
    }
  }
}

static gboolean service_profiles_active(YNode *service,
                                        GHashTable *active_profiles) {
  YNode *profiles = map_get(service, "profiles");
  if (!profiles) {
    return TRUE;
  }
  guint count = profiles->kind == NODE_SEQUENCE ? profiles->items->len : 1;
  for (guint i = 0; i < count; i++) {
    YNode *profile = profiles->kind == NODE_SEQUENCE
                         ? g_ptr_array_index(profiles->items, i)
                         : profiles;
    const char *name = node_string(profile);
    if (name && (g_hash_table_contains(active_profiles, name) ||
                 g_hash_table_contains(active_profiles, "*"))) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
for_services(YNode *services, Options *opts,
             gboolean (*callback)(const char *, YNode *, void *), void *data) {
  if (!services || services->kind != NODE_MAPPING || !services->items->len) {
    fail("Compose configuration must contain a non-empty services mapping");
    return FALSE;
  }
  gboolean selected_any = FALSE;
  for (guint i = 0; i < services->items->len; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    const char *name = node_string(pair->key);
    if (!valid_service_name(name) || pair->value->kind != NODE_MAPPING) {
      fail("invalid service definition");
      return FALSE;
    }
    gboolean selected = service_requested(opts, name);
    if (!selected ||
        (opts->services->len == 0 &&
         !service_profile_enabled(pair->value, opts))) {
      continue;
    }
    if (selected) {
      selected_any = TRUE;
      if (!callback(name, pair->value, data)) {
        return FALSE;
      }
    }
  }
  if (!selected_any && opts->services->len) {
    fail("none of the requested services are defined or enabled");
    return FALSE;
  }
  return TRUE;
}

/* Compose dependencies are launched before dependents. Quocker currently
 * implements only service_started: a successful QEMU spawn is the readiness
 * boundary until guest-agent health reporting is available. */
typedef struct UpOrder {
  YNode *services;
  Options *opts;
  GHashTable *marks;
  GHashTable *active_profiles;
  GPtrArray *ordered;
  gboolean validate_runtime_conditions;
} UpOrder;

static YNode *service_by_name(YNode *services, const char *name) {
  for (guint i = 0; i < services->items->len; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    if (g_strcmp0(node_string(pair->key), name) == 0) {
      return pair->value;
    }
  }
  return NULL;
}

static gboolean up_order_visit(UpOrder *order, const char *name) {
  gpointer mark = g_hash_table_lookup(order->marks, name);
  if (GPOINTER_TO_INT(mark) == 2) {
    return TRUE;
  }
  if (GPOINTER_TO_INT(mark) == 1) {
    fail("service dependency cycle includes '%s'", name);
    return FALSE;
  }
  YNode *service = service_by_name(order->services, name);
  if (!service || service->kind != NODE_MAPPING) {
    fail("service '%s' is referenced by depends_on but is not defined", name);
    return FALSE;
  }
  if (order->validate_runtime_conditions &&
      !service_profiles_active(service, order->active_profiles)) {
    fail("service '%s' depends on a service gated by an inactive profile",
         name);
    return FALSE;
  }
  g_hash_table_insert(order->marks, g_strdup(name), GINT_TO_POINTER(1));
  YNode *depends = map_get(service, "depends_on");
  if (depends && depends->kind != NODE_SEQUENCE &&
      depends->kind != NODE_MAPPING) {
    fail("service '%s': depends_on must be a sequence or mapping", name);
    return FALSE;
  }
  for (guint i = 0; depends && i < depends->items->len; i++) {
    const char *dependency = NULL;
    YNode *options = NULL;
    gboolean required_dependency = TRUE;
    if (depends->kind == NODE_SEQUENCE) {
      dependency = node_string(g_ptr_array_index(depends->items, i));
    } else {
      YPair *pair = g_ptr_array_index(depends->items, i);
      dependency = node_string(pair->key);
      options = pair->value;
    }
    if (!valid_service_name(dependency)) {
      fail("service '%s': depends_on contains an invalid service name", name);
      return FALSE;
    }
    const char *condition = "service_started";
    if (options && !order->validate_runtime_conditions &&
        options->kind == NODE_MAPPING) {
      YNode *required = map_get(options, "required");
      if (required && required->scalar &&
          g_str_equal(required->scalar, "false")) {
        required_dependency = FALSE;
      }
    }
    if (options && order->validate_runtime_conditions) {
      if (options->kind != NODE_MAPPING) {
        fail("service '%s': depends_on.%s must be a mapping", name, dependency);
        return FALSE;
      }
      YNode *condition_node = map_get(options, "condition");
      if (condition_node) {
        condition = node_string(condition_node);
      }
      if (g_strcmp0(condition, "service_started") != 0) {
        fail("service '%s': depends_on.%s condition '%s' requires guest "
             "readiness support; only service_started is currently supported",
             name, dependency, condition ? condition : "(invalid)");
        return FALSE;
      }
      YNode *required = map_get(options, "required");
      if (required &&
          (!required->scalar || !(g_str_equal(required->scalar, "true") ||
                                  g_str_equal(required->scalar, "false")))) {
        fail("service '%s': depends_on.%s required must be a boolean", name,
             dependency);
        return FALSE;
      }
      required_dependency = !required || g_str_equal(required->scalar, "true");
      YNode *restart = map_get(options, "restart");
      if (restart &&
          (!restart->scalar || !(g_str_equal(restart->scalar, "true") ||
                                 g_str_equal(restart->scalar, "false")))) {
        fail("service '%s': depends_on.%s restart must be a boolean", name,
             dependency);
        return FALSE;
      }
      if (restart && g_str_equal(restart->scalar, "true")) {
        fail("service '%s': depends_on.%s restart requires recreation "
             "tracking and is not supported yet",
             name, dependency);
        return FALSE;
      }
      for (guint field = 0; field < options->items->len; field++) {
        YPair *entry = g_ptr_array_index(options->items, field);
        const char *key = node_string(entry->key);
        if (!key ||
            !(g_str_equal(key, "condition") || g_str_equal(key, "required") ||
              g_str_equal(key, "restart"))) {
          fail("service '%s': unknown depends_on.%s option", name, dependency);
          return FALSE;
        }
      }
    }
    YNode *dependency_service = service_by_name(order->services, dependency);
    if (!dependency_service) {
      if (!required_dependency) {
        g_warning("service '%s' has optional missing dependency '%s'", name,
                  dependency);
        continue;
      }
      fail("service '%s' depends on undefined service '%s'", name, dependency);
      return FALSE;
    }
    if (!up_order_visit(order, dependency)) {
      return FALSE;
    }
  }
  g_hash_table_replace(order->marks, g_strdup(name), GINT_TO_POINTER(2));
  g_ptr_array_add(order->ordered, (gpointer)name);
  return TRUE;
}

static gboolean for_up_services(YNode *services, Options *opts,
                                gboolean (*callback)(const char *, YNode *,
                                                     void *),
                                void *data) {
  UpOrder order = {0};
  order.services = services;
  order.opts = opts;
  order.marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  order.active_profiles =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  order.ordered = g_ptr_array_new();
  order.validate_runtime_conditions = TRUE;
  for (guint i = 0; i < opts->profiles->len; i++) {
    g_hash_table_add(order.active_profiles,
                     g_strdup(g_ptr_array_index(opts->profiles, i)));
  }
  for (guint i = 0; i < opts->services->len; i++) {
    YNode *target =
        service_by_name(services, g_ptr_array_index(opts->services, i));
    if (target) {
      add_service_profiles(order.active_profiles, target);
    }
  }
  gboolean ok = TRUE;
  for (guint i = 0; i < services->items->len && ok; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    const char *name = node_string(pair->key);
    if (!valid_service_name(name) || pair->value->kind != NODE_MAPPING) {
      fail("invalid service definition");
      ok = FALSE;
      break;
    }
    gboolean selected = service_requested(opts, name);
    if (selected && (opts->services->len > 0 ||
                     service_profile_enabled(pair->value, opts))) {
      ok = up_order_visit(&order, name);
    }
  }
  if (ok && opts->services->len && order.ordered->len == 0) {
    fail("none of the requested services are defined or enabled");
    ok = FALSE;
  }
  for (guint i = 0; i < order.ordered->len && ok; i++) {
    const char *name = g_ptr_array_index(order.ordered, i);
    ok = callback(name, service_by_name(services, name), data);
  }
  g_ptr_array_free(order.ordered, TRUE);
  g_hash_table_destroy(order.active_profiles);
  g_hash_table_destroy(order.marks);
  return ok;
}

static gboolean for_down_services(YNode *services, Options *opts,
                                  gboolean (*callback)(const char *, YNode *,
                                                       void *),
                                  void *data) {
  UpOrder order = {0};
  order.services = services;
  order.opts = opts;
  order.marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  order.ordered = g_ptr_array_new();
  order.validate_runtime_conditions = FALSE;
  gboolean ok = TRUE;
  for (guint i = 0; i < services->items->len && ok; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    const char *name = node_string(pair->key);
    if (!valid_service_name(name) || pair->value->kind != NODE_MAPPING) {
      fail("invalid service definition");
      ok = FALSE;
      break;
    }
    gboolean selected = service_requested(opts, name);
    if (selected && (opts->services->len > 0 ||
                     service_profile_enabled(pair->value, opts))) {
      ok = up_order_visit(&order, name);
    }
  }
  if (ok && opts->services->len && order.ordered->len == 0) {
    fail("none of the requested services are defined or enabled");
    ok = FALSE;
  }
  for (guint i = order.ordered->len; i > 0 && ok; i--) {
    const char *name = g_ptr_array_index(order.ordered, i - 1);
    ok = callback(name, service_by_name(services, name), data);
  }
  g_ptr_array_free(order.ordered, TRUE);
  g_hash_table_destroy(order.marks);
  return ok;
}

typedef struct UpContext {
  const char *project;
  const char *root;
  const char *directory;
  PullContext *pull_context;
  GHashTable *project_environment;
  gboolean dry_run;
  gboolean start_only;
} UpContext;

typedef struct PublishedPortOwner {
  const char *service;
  PortBinding *binding;
} PublishedPortOwner;

typedef struct PortPreflight {
  GPtrArray *published;
} PortPreflight;

static gboolean preflight_service_ports(const char *name, YNode *service,
                                        void *data) {
  PortPreflight *preflight = data;
  GPtrArray *bindings = g_ptr_array_new_with_free_func(
      (GDestroyNotify)port_binding_free);
  gboolean valid = FALSE;
  char *forwards = port_forwards(service, name, &valid, bindings);
  g_free(forwards);
  if (!valid) {
    g_ptr_array_free(bindings, TRUE);
    return FALSE;
  }
  for (guint i = 0; i < bindings->len; i++) {
    PortBinding *binding = g_ptr_array_index(bindings, i);
    if (binding->dynamic) {
      continue;
    }
    for (guint j = 0; j < preflight->published->len; j++) {
      PublishedPortOwner *owner = g_ptr_array_index(preflight->published, j);
      PortBinding *other = owner->binding;
      gboolean binding_wildcard = !*binding->host_ip ||
                                  g_str_equal(binding->host_ip, "0.0.0.0");
      gboolean other_wildcard = !*other->host_ip ||
                                g_str_equal(other->host_ip, "0.0.0.0");
      gboolean address_overlap = binding_wildcard || other_wildcard ||
                                 g_str_equal(binding->host_ip, other->host_ip);
      if (binding->host_port == other->host_port &&
          g_str_equal(binding->protocol, other->protocol) && address_overlap) {
        fail("services '%s' and '%s' both publish %s host port %u",
             owner->service, name, binding->protocol, binding->host_port);
        g_ptr_array_free(bindings, TRUE);
        return FALSE;
      }
    }
    PublishedPortOwner *owner = g_new0(PublishedPortOwner, 1);
    owner->service = name;
    owner->binding = g_new0(PortBinding, 1);
    owner->binding->host_ip = g_strdup(binding->host_ip);
    owner->binding->protocol = g_strdup(binding->protocol);
    owner->binding->host_port = binding->host_port;
    g_ptr_array_add(preflight->published, owner);
  }
  g_ptr_array_free(bindings, TRUE);
  return TRUE;
}

static void published_port_owner_free(PublishedPortOwner *owner) {
  if (owner) {
    port_binding_free(owner->binding);
    g_free(owner);
  }
}

static const char *service_image_reference(YNode *service) {
  const char *image = node_string(map_get(service, "image"));
  const char *extension_image =
      node_string(map_get(map_get(service, "x-quocker"), "image"));
  return extension_image ? extension_image : image;
}

static gboolean service_image_is_local(YNode *service, const char *root) {
  const char *image = service_image_reference(service);
  if (!image || !*image) {
    return FALSE;
  }
  if (g_path_is_absolute(image) || g_str_has_prefix(image, "./") ||
      g_str_has_prefix(image, "../") || g_str_has_prefix(image, "~/")) {
    return TRUE;
  }
  char *path = absolute_path(image, root);
  gboolean local = g_file_test(path, G_FILE_TEST_IS_REGULAR);
  g_free(path);
  return local;
}

static gboolean up_one(const char *name, YNode *service, void *data) {
  UpContext *ctx = data;
  if (ctx->dry_run) {
    gboolean ports_valid = FALSE;
    char *forwards = port_forwards(service, name, &ports_valid, NULL);
    if (ports_valid) {
      g_print("Would start service %s", name);
      if (*forwards) {
        g_print(" (QEMU user network: %s)", forwards);
      }
      g_print("\n");
    }
    g_free(forwards);
    return ports_valid;
  }
  pid_t pid = read_pid(ctx->directory, name);
  if (process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
    g_print("[%s] %s: already running (pid %d)\n", ctx->project, name, pid);
    return TRUE;
  }
  if (pid_exists(pid)) {
    fail("service '%s': saved PID %d belongs to a different or unverifiable "
         "process; refusing to start another VM until its state is inspected",
         name, pid);
    return FALSE;
  }
  if (ctx->start_only) {
    char *state = state_path(ctx->directory, name);
    gboolean created = g_file_test(state, G_FILE_TEST_IS_REGULAR);
    g_free(state);
    char *disk_name = created ? state_disk_basename(ctx->directory, name)
                              : NULL;
    char *disk_path = disk_name
                          ? g_build_filename(ctx->directory, disk_name, NULL)
                          : NULL;
    struct stat disk_stat;
    created = created && disk_path && lstat(disk_path, &disk_stat) == 0 &&
              S_ISREG(disk_stat.st_mode);
    g_free(disk_name);
    g_free(disk_path);
    if (!created) {
      fail("service '%s' has no saved VM state; use 'quocker up' to create it",
           name);
      return FALSE;
    }
  }
  if (service_image_is_local(service, ctx->root)) {
    return run_qemu_service(ctx->project, ctx->root, ctx->directory, name,
                            service, NULL);
  }
  if (unsupported_service_settings(service, name, TRUE)) {
    return FALSE;
  }
  QuockerOciImage *image = NULL;
  QuockerKernel *kernel = NULL;
  QuockerRuntimeConfig *runtime = NULL;
  char *initrd_path = NULL;
  if (!prepare_oci_boot(name, ctx->directory, ctx->root, service,
                        ctx->pull_context, ctx->project_environment,
                        &image, &kernel, &runtime, &initrd_path)) {
    return FALSE;
  }
  char *root_disk = g_strdup_printf("%s.ext4", image->rootfs_path);
  QuockerBootAssets boot = {.root_disk = root_disk,
                            .kernel_path = kernel->kernel_path,
                            .initrd_path = initrd_path,
                            .architecture = image->architecture,
                            .manifest_digest = image->manifest_digest,
                            .kernel = kernel};
  gboolean ok = run_qemu_service(ctx->project, ctx->root, ctx->directory, name,
                                 service, &boot);
  g_free(root_disk);
  g_free(initrd_path);
  quocker_runtime_config_free(runtime);
  quocker_kernel_free(kernel);
  quocker_oci_image_free(image);
  return ok;
}

static gboolean run_up_services(YNode *services, Options *opts,
                                const char *project, const char *root,
                                const char *directory,
                                GHashTable *project_environment,
                                gboolean start_only) {
  char *cache = g_build_filename(g_get_user_cache_dir(), "quocker", "oci",
                                 NULL);
  const char *catalog = g_getenv("QUOCKER_KERNEL_CATALOG");
  const char *public_key = g_getenv("QUOCKER_KERNEL_CATALOG_PUBKEY");
  PullContext pull_context = {
      cache,
      g_getenv("QUOCKER_REGISTRY_MIRROR"),
      catalog && *catalog ? catalog : default_kernel_catalog_path(),
      public_key && *public_key ? public_key
                                : "/etc/quocker/kernel-catalog.pub",
  };
  UpContext context = {project, root, directory, &pull_context,
                       project_environment, opts->dry_run, start_only};
  PortPreflight preflight = {
      g_ptr_array_new_with_free_func(
          (GDestroyNotify)published_port_owner_free)};
  gboolean ok = for_up_services(services, opts, preflight_service_ports,
                                &preflight);
  if (ok) {
    ok = for_up_services(services, opts, up_one, &context);
  }
  g_ptr_array_free(preflight.published, TRUE);
  if (ok && !opts->detach && !opts->dry_run) {
    g_print("[%s] running in the background; use 'quocker logs -f' to follow "
            "output\n",
            project);
  }
  g_free(cache);
  return ok;
}

typedef struct DownContext {
  const char *project;
  const char *directory;
  gboolean remove_volumes;
  gboolean dry_run;
} DownContext;

typedef struct ServiceSignalContext {
  const char *project;
  const char *directory;
  gboolean dry_run;
  int signal_number;
} ServiceSignalContext;

static gboolean stop_one(const char *name, YNode *service, void *data) {
  ServiceSignalContext *ctx = data;
  if (ctx->dry_run) {
    g_print("Would stop service %s\n", name);
    return TRUE;
  }
  pid_t pid = read_pid(ctx->directory, name);
  if (!process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
    if (pid_exists(pid)) {
      fail("service '%s': saved PID %d does not match this VM; refusing to "
           "signal or remove its state", name, pid);
      return FALSE;
    }
    g_print("[%s] %s: not running\n", ctx->project, name);
  } else {
    if (signal_process(pid, ctx->project, name,
                       state_process_start_time(ctx->directory, name),
                       SIGTERM) < 0 &&
        errno != ESRCH) {
      fail("service '%s': could not send SIGTERM: %s", name,
           g_strerror(errno));
      return FALSE;
    }
    for (guint i = 0; i < 100 && process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name)); i++) {
      g_usleep(100000);
    }
    if (process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
      if (signal_process(pid, ctx->project, name,
                         state_process_start_time(ctx->directory, name),
                         SIGKILL) < 0 &&
          errno != ESRCH) {
        fail("service '%s': could not send SIGKILL after timeout: %s", name,
             g_strerror(errno));
        return FALSE;
      }
      for (guint i = 0; i < 20 && process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name));
           i++) {
        g_usleep(100000);
      }
    }
    if (process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
      fail("service '%s': QEMU did not stop after SIGKILL", name);
      return FALSE;
    }
    g_print("[%s] %s: stopped\n", ctx->project, name);
  }
  char *pidfile = g_strdup_printf("%s/%s.pid", ctx->directory, name);
  g_unlink(pidfile);
  g_free(pidfile);
  qmp_socket_remove(ctx->directory, name);
  return TRUE;
}

static gboolean kill_one(const char *name, YNode *service, void *data) {
  ServiceSignalContext *ctx = data;
  const char *signal_name = g_strsignal(ctx->signal_number);
  if (ctx->dry_run) {
    g_print("Would send %s to service %s\n", signal_name, name);
    return TRUE;
  }
  pid_t pid = read_pid(ctx->directory, name);
  if (!process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
    if (pid_exists(pid)) {
      fail("service '%s': saved PID %d does not match this VM; refusing to "
           "signal it", name, pid);
      return FALSE;
    }
    g_print("[%s] %s: not running\n", ctx->project, name);
    return TRUE;
  }
  if (signal_process(pid, ctx->project, name,
                     state_process_start_time(ctx->directory, name),
                     ctx->signal_number) < 0 &&
      errno != ESRCH) {
    fail("service '%s': could not send %s: %s", name, signal_name,
         g_strerror(errno));
    return FALSE;
  }
  g_print("[%s] %s: sent %s\n", ctx->project, name, signal_name);
  if (ctx->signal_number == SIGKILL) {
    for (guint i = 0; i < 20 && process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name)); i++) {
      g_usleep(100000);
    }
    if (!process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name))) {
      char *pidfile = g_strdup_printf("%s/%s.pid", ctx->directory, name);
      g_unlink(pidfile);
      g_free(pidfile);
      qmp_socket_remove(ctx->directory, name);
    }
  }
  return TRUE;
}

typedef struct VmControlContext {
  const char *project;
  const char *directory;
  gboolean dry_run;
  const char *qmp_command;
  const char *action;
} VmControlContext;

static gboolean control_one(const char *name, YNode *service, void *data) {
  VmControlContext *ctx = data;
  if (ctx->dry_run) {
    g_print("Would %s service %s\n", ctx->action, name);
    return TRUE;
  }
  pid_t pid = read_pid(ctx->directory, name);
  guint64 start_time = state_process_start_time(ctx->directory, name);
  if (!process_running(pid, ctx->project, name, start_time)) {
    fail("service '%s' is not running", name);
    return FALSE;
  }
  GError *error = NULL;
  if (!qmp_command(ctx->directory, name, ctx->qmp_command, &error)) {
    fail("service '%s': could not %s through QMP: %s", name, ctx->action,
         error ? error->message : "unknown QMP error");
    g_clear_error(&error);
    return FALSE;
  }
  g_print("[%s] %s: %s\n", ctx->project, name, ctx->action);
  return TRUE;
}

static gboolean down_one(const char *name, YNode *service, void *data) {
  DownContext *ctx = data;
  if (ctx->dry_run) {
    g_print("Would stop service %s\n", name);
    return TRUE;
  }
  pid_t pid = read_pid(ctx->directory, name);
  guint64 start_time = state_process_start_time(ctx->directory, name);
  gboolean running = process_running(
      pid, ctx->project, name, start_time);
  if (!running && pid_exists(pid)) {
    fail("service '%s': saved PID %d does not match this VM; refusing to "
         "remove its state", name, pid);
    return FALSE;
  }
  if (running) {
    if (signal_process(pid, ctx->project, name, start_time, SIGTERM) < 0 &&
        errno != ESRCH) {
      fail("service '%s': could not send SIGTERM: %s", name,
           g_strerror(errno));
      return FALSE;
    }
    for (guint i = 0; i < 100 &&
                       process_running(pid, ctx->project, name, start_time);
         i++) {
      g_usleep(100000);
    }
    if (process_running(pid, ctx->project, name, start_time)) {
      if (signal_process(pid, ctx->project, name, start_time, SIGKILL) < 0 &&
          errno != ESRCH) {
        fail("service '%s': could not send SIGKILL after timeout: %s", name,
             g_strerror(errno));
        return FALSE;
      }
      for (guint i = 0; i < 20 &&
                         process_running(pid, ctx->project, name, start_time);
           i++) {
        g_usleep(100000);
      }
    }
    if (process_running(pid, ctx->project, name, start_time)) {
      fail("service '%s': QEMU did not stop after SIGKILL", name);
      return FALSE;
    }
    g_print("[%s] %s: stopped\n", ctx->project, name);
  } else {
    g_print("[%s] %s: not running\n", ctx->project, name);
  }
  char *state = state_path(ctx->directory, name);
  char *pidfile = g_strdup_printf("%s/%s.pid", ctx->directory, name);
  qmp_socket_remove(ctx->directory, name);
  g_unlink(state);
  g_unlink(pidfile);
  if (ctx->remove_volumes) {
    GDir *dir = g_dir_open(ctx->directory, 0, NULL);
    gboolean removed = FALSE;
    const char *entry;
    while (dir && (entry = g_dir_read_name(dir))) {
      if (service_overlay_name(entry, name)) {
        char *disk = g_build_filename(ctx->directory, entry, NULL);
        if (g_unlink(disk) == 0) {
          removed = TRUE;
        }
        g_free(disk);
      }
    }
    if (dir) {
      g_dir_close(dir);
    }
    if (removed) {
      g_print("[%s] %s: overlay(s) removed\n", ctx->project, name);
    }
    char *remaining_overlay = find_service_overlay(ctx->directory, name);
    if (!remaining_overlay) {
      char *cache = g_build_filename(g_get_user_cache_dir(), "quocker", "oci",
                                     NULL);
      gboolean unreferenced = quocker_oci_cache_unreference(
          cache, ctx->directory, name);
      g_free(cache);
      if (!unreferenced) {
        g_free(state);
        g_free(pidfile);
        return FALSE;
      }
    }
    g_free(remaining_overlay);
  }
  g_free(state);
  g_free(pidfile);
  return TRUE;
}

typedef struct ListContext {
  const char *project;
  const char *directory;
} ListContext;

static gboolean ps_one(const char *name, YNode *service, void *data) {
  ListContext *ctx = data;
  pid_t pid = read_pid(ctx->directory, name);
  gboolean active = process_running(pid, ctx->project, name, state_process_start_time(ctx->directory, name));
  char *disk = find_service_overlay(ctx->directory, name);
  char *pid_text = active ? g_strdup_printf("%d", pid) : g_strdup("-");
  g_print("%s-%s\t%s\t%s\t%s\n", ctx->project, name,
          active ? "running" : "stopped", pid_text, disk ? disk : "-");
  g_free(pid_text);
  g_free(disk);
  return TRUE;
}

typedef struct PortContext {
  const char *requested_port;
  const char *directory;
  const char *project;
} PortContext;

static gboolean dynamic_port_lookup(const char *directory, const char *project,
                                    const char *service,
                                    const PortBinding *binding,
                                    guint *host_port) {
  pid_t pid = read_pid(directory, service);
  if (!process_running(pid, project, service,
                       state_process_start_time(directory, service))) {
    fail("service '%s' must be running to query its dynamically published "
         "port", service);
    return FALSE;
  }
  GError *error = NULL;
  char *info = qmp_usernet_info(directory, service, &error);
  if (!info) {
    fail("service '%s': cannot query QEMU user network: %s", service,
         error->message);
    g_clear_error(&error);
    return FALSE;
  }
  gboolean found = FALSE;
  gchar **lines = g_strsplit(info, "\n", -1);
  for (guint i = 0; lines[i] && !found; i++) {
    gchar **raw = g_strsplit_set(lines[i], " \t\r", -1);
    GPtrArray *fields = g_ptr_array_new();
    for (guint j = 0; raw[j]; j++) {
      if (*raw[j]) {
        g_ptr_array_add(fields, raw[j]);
      }
    }
    if (fields->len >= 6) {
      const char *kind = g_ptr_array_index(fields, 0);
      const char *source_ip = g_ptr_array_index(fields, 2);
      const char *source_port_text = g_ptr_array_index(fields, 3);
      const char *guest_port_text = g_ptr_array_index(fields, 5);
      guint source_port = 0, guest_port = 0;
      const char *expected_kind = g_str_equal(binding->protocol, "tcp")
                                      ? "TCP[HOST_FORWARD]"
                                      : "UDP[HOST_FORWARD]";
      if (g_str_equal(kind, expected_kind) &&
          parse_port_range(source_port_text, &source_port, &source_port) &&
          parse_port_range(guest_port_text, &guest_port, &guest_port) &&
          guest_port == binding->guest_port &&
          (*source_ip == '*' || !*binding->host_ip ||
           g_str_equal(source_ip, binding->host_ip))) {
        *host_port = source_port;
        found = TRUE;
      }
    }
    g_ptr_array_free(fields, TRUE);
    g_strfreev(raw);
  }
  g_strfreev(lines);
  g_free(info);
  if (!found) {
    fail("service '%s': QEMU has not allocated a host port for %s guest "
         "port %u", service, binding->protocol, binding->guest_port);
  }
  return found;
}

static gboolean port_one(const char *name, YNode *service, void *data) {
  PortContext *context = data;
  char *copy = g_strdup(context->requested_port);
  char *slash = strchr(copy, '/');
  const char *protocol = "tcp";
  if (slash) {
    *slash++ = '\0';
    protocol = slash;
  }
  guint first = 0, last = 0;
  gboolean query_valid = parse_port_range(copy, &first, &last) &&
                         first == last &&
                         (g_str_equal(protocol, "tcp") ||
                          g_str_equal(protocol, "udp"));
  if (!query_valid) {
    fail("service '%s': port query must be PORT[/tcp|udp]", name);
    g_free(copy);
    return FALSE;
  }
  GPtrArray *bindings = g_ptr_array_new_with_free_func(
      (GDestroyNotify)port_binding_free);
  gboolean valid = FALSE;
  char *forwards = port_forwards(service, name, &valid, bindings);
  g_free(forwards);
  if (!valid) {
    g_free(copy);
    g_ptr_array_free(bindings, TRUE);
    return FALSE;
  }
  gboolean found = FALSE;
  for (guint i = 0; i < bindings->len; i++) {
    PortBinding *binding = g_ptr_array_index(bindings, i);
    if (binding->guest_port == first &&
        g_str_equal(binding->protocol, protocol)) {
      guint host_port = binding->host_port;
      if (binding->dynamic &&
          !dynamic_port_lookup(context->directory, context->project, name,
                               binding, &host_port)) {
        g_free(copy);
        g_ptr_array_free(bindings, TRUE);
        return FALSE;
      }
      g_print("%s:%u\n", *binding->host_ip ? binding->host_ip : "0.0.0.0",
              host_port);
      found = TRUE;
    }
  }
  if (!found) {
    fail("service '%s' has no published %s port %u", name, protocol, first);
  }
  g_free(copy);
  g_ptr_array_free(bindings, TRUE);
  return found;
}

typedef struct LogsContext {
  const char *directory;
  Options *opts;
} LogsContext;

static gboolean logs_one(const char *name, YNode *service, void *data) {
  LogsContext *ctx = data;
  char *path = g_strdup_printf("%s/%s.log", ctx->directory, name);
  gchar *contents = NULL;
  if (!ctx->opts->follow && g_file_get_contents(path, &contents, NULL, NULL)) {
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
      if (*lines[i]) {
        g_print("%s | %s\n", name, lines[i]);
      }
    }
    g_strfreev(lines);
  }
  g_free(contents);
  g_free(path);
  return TRUE;
}

static volatile sig_atomic_t follow_running = 1;

static void stop_follow(int signal_number) { follow_running = 0; }

static void follow_logs(YNode *services, Options *opts, const char *directory) {
  GHashTable *offsets =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  for (guint i = 0; i < services->items->len; i++) {
    YPair *pair = g_ptr_array_index(services->items, i);
    const char *name = node_string(pair->key);
    gboolean selected = service_requested(opts, name);
    if (!selected ||
        (opts->services->len == 0 &&
         !service_profile_enabled(pair->value, opts))) {
      continue;
    }
    if (selected) {
      char *path = g_strdup_printf("%s/%s.log", directory, name);
      GStatBuf statbuf;
      guint64 *offset = g_new0(guint64, 1);
      if (g_stat(path, &statbuf) == 0) {
        *offset = statbuf.st_size;
      }
      g_hash_table_insert(offsets, g_strdup(name), offset);
      g_free(path);
    }
  }
  signal(SIGINT, stop_follow);
  follow_running = 1;
  while (follow_running) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, offsets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      const char *name = key;
      guint64 *offset = value;
      char *path = g_strdup_printf("%s/%s.log", directory, name);
      gchar *contents = NULL;
      gsize length = 0;
      if (g_file_get_contents(path, &contents, &length, NULL) &&
          length > *offset) {
        const char *new_data = contents + *offset;
        fwrite(new_data, 1, length - *offset, stdout);
        fflush(stdout);
        *offset = length;
      }
      g_free(contents);
      g_free(path);
    }
    g_usleep(250000);
  }
  signal(SIGINT, SIG_DFL);
  g_hash_table_destroy(offsets);
}

static void usage(FILE *file) {
  fprintf(file,
          "Usage: quocker [OPTIONS] COMMAND\n"
          "       quocker compose [OPTIONS] COMMAND\n\n"
          "Manage QEMU virtual machines using a Compose-shaped YAML file.\n\n"
          "Options:\n"
          "  -f, --file FILE             Compose file (repeatable)\n"
          "  -p, --project-name NAME     Set project name\n"
          "      --project-directory DIR Set project directory\n"
          "      --env-file FILE         Set interpolation environment file\n"
          "      --profile PROFILE       Enable a service profile\n\n"
          "  config --format yaml|json   Select config output format\n"
          "  config --services|--profiles|--images  List config entries\n"
          "  config --volumes|--networks   List declared resources\n"
          "  config --environment        Print interpolation environment\n"
          "      --dry-run              Print the dependency-ordered lifecycle "
          "plan\n\n"
          "Commands: up, start, stop, restart, kill, pause, unpause, down, "
          "rm, ps, logs, "
          "pull, prune, config, port\n"
          "  kill: -s SIGNAL             Signal to send (default SIGKILL)\n"
          "Kernel tools: quocker kernel select|fetch --platform OS/ARCH "
          "[--rootfs DIR] [--pubkey FILE] [--module-release REL] [--json]\n"
          "              quocker kernel update --url HTTPS_URL "
          "[--catalog FILE] [--pubkey FILE]\n");
}

static gboolean parse_signal_number(const char *text, int *signal_out) {
  static const struct {
    const char *name;
    int number;
  } signals[] = {
      {"HUP", SIGHUP},      {"INT", SIGINT},      {"QUIT", SIGQUIT},
      {"KILL", SIGKILL},    {"TERM", SIGTERM},    {"USR1", SIGUSR1},
      {"USR2", SIGUSR2},    {"SIGHUP", SIGHUP},   {"SIGINT", SIGINT},
      {"SIGQUIT", SIGQUIT}, {"SIGKILL", SIGKILL}, {"SIGTERM", SIGTERM},
      {"SIGUSR1", SIGUSR1}, {"SIGUSR2", SIGUSR2}, {NULL, 0}};
  for (guint i = 0; signals[i].name; i++) {
    if (g_ascii_strcasecmp(text, signals[i].name) == 0) {
      *signal_out = signals[i].number;
      return TRUE;
    }
  }
  char *end = NULL;
  guint64 number = g_ascii_strtoull(text, &end, 10);
  if (end != text && !*end && number > 0 && number < NSIG) {
    *signal_out = (int)number;
    return TRUE;
  }
  return FALSE;
}

static gboolean parse_options(int argc, char **argv, Options *opts) {
  opts->files = g_ptr_array_new_with_free_func(g_free);
  opts->env_files = g_ptr_array_new_with_free_func(g_free);
  opts->profiles = g_ptr_array_new_with_free_func(g_free);
  opts->services = g_ptr_array_new_with_free_func(g_free);
  opts->signal_number = SIGKILL;
  const char *env_profiles = g_getenv("COMPOSE_PROFILES");
  if (env_profiles) {
    gchar **parts = g_strsplit(env_profiles, ",", -1);
    for (guint j = 0; parts[j]; j++) {
      char *profile = g_strstrip(parts[j]);
      if (*profile) {
        g_ptr_array_add(opts->profiles, g_strdup(profile));
      }
    }
    g_strfreev(parts);
  }
  int i = 1;
  if (i < argc && g_str_equal(argv[i], "compose")) {
    i++;
  }
  for (; i < argc; i++) {
    const char *arg = argv[i];
    if (g_str_equal(arg, "-h") || g_str_equal(arg, "--help")) {
      usage(stdout);
      exit(0);
    } else if (g_str_equal(arg, "--file") ||
               (g_str_equal(arg, "-f") &&
                !(opts->command && (g_str_equal(opts->command, "logs") ||
                                    g_str_equal(opts->command, "rm")))) ||
               g_str_equal(arg, "-p") || g_str_equal(arg, "--project-name") ||
               g_str_equal(arg, "--project-directory") ||
               g_str_equal(arg, "--env-file") ||
               g_str_equal(arg, "--profile")) {
      if (++i >= argc) {
        fail("option %s requires a value", arg);
        return FALSE;
      }
      if (g_str_equal(arg, "-f") || g_str_equal(arg, "--file")) {
        g_ptr_array_add(opts->files, g_strdup(argv[i]));
      } else if (g_str_equal(arg, "--env-file")) {
        g_ptr_array_add(opts->env_files, g_strdup(argv[i]));
      } else if (g_str_equal(arg, "--profile")) {
        g_ptr_array_add(opts->profiles, g_strdup(argv[i]));
      } else if (g_str_equal(arg, "-p") || g_str_equal(arg, "--project-name")) {
        opts->project_name = g_strdup(argv[i]);
      } else {
        opts->project_directory = g_strdup(argv[i]);
      }
    } else if (!opts->command && !g_str_has_prefix(arg, "-")) {
      opts->command = g_strdup(arg);
    } else if (opts->command && g_str_equal(arg, "-d")) {
      opts->detach = TRUE;
    } else if (opts->command && g_str_equal(arg, "--dry-run") &&
               (g_str_equal(opts->command, "up") ||
                g_str_equal(opts->command, "start") ||
                g_str_equal(opts->command, "stop") ||
                g_str_equal(opts->command, "restart") ||
                g_str_equal(opts->command, "kill") ||
                g_str_equal(opts->command, "pause") ||
                g_str_equal(opts->command, "unpause") ||
                g_str_equal(opts->command, "down"))) {
      opts->dry_run = TRUE;
    } else if (opts->command && g_str_equal(opts->command, "kill") &&
               (g_str_equal(arg, "-s") || g_str_equal(arg, "--signal"))) {
      if (++i >= argc || !parse_signal_number(argv[i], &opts->signal_number)) {
        fail("kill requires a supported signal name or number");
        return FALSE;
      }
    } else if (opts->command && g_str_equal(arg, "-v")) {
      opts->remove_volumes = TRUE;
    } else if (opts->command && g_str_equal(arg, "-f") &&
               g_str_equal(opts->command, "logs")) {
      opts->follow = TRUE;
    } else if (opts->command && g_str_equal(arg, "-f") &&
               g_str_equal(opts->command, "rm")) {
      /* Docker Compose's rm --force is accepted as a no-op here. */
    } else if (opts->command && g_str_equal(arg, "--quiet")) {
      opts->quiet = TRUE;
    } else if (opts->command && g_str_equal(opts->command, "config") &&
               (g_str_equal(arg, "--environment") ||
                g_str_equal(arg, "--services") ||
                g_str_equal(arg, "--profiles") ||
                g_str_equal(arg, "--images") ||
                g_str_equal(arg, "--volumes") ||
                g_str_equal(arg, "--networks"))) {
      const char *mode = arg + 2;
      if (opts->config_list_mode &&
          !g_str_equal(opts->config_list_mode, mode)) {
        fail("config output selection options are mutually exclusive");
        return FALSE;
      }
      g_free(opts->config_list_mode);
      opts->config_list_mode = g_strdup(mode);
    } else if (opts->command && g_str_equal(opts->command, "config") &&
               g_str_equal(arg, "--format")) {
      if (++i >= argc ||
          (!g_str_equal(argv[i], "yaml") && !g_str_equal(argv[i], "json"))) {
        fail("config --format must be 'yaml' or 'json'");
        return FALSE;
      }
      g_free(opts->config_format);
      opts->config_format = g_strdup(argv[i]);
    } else if (opts->command && g_str_equal(opts->command, "config") &&
               g_str_has_prefix(arg, "--format=")) {
      const char *format = arg + strlen("--format=");
      if (!g_str_equal(format, "yaml") && !g_str_equal(format, "json")) {
        fail("config --format must be 'yaml' or 'json'");
        return FALSE;
      }
      g_free(opts->config_format);
      opts->config_format = g_strdup(format);
    } else if (opts->command && g_str_equal(opts->command, "port") &&
               !g_str_has_prefix(arg, "-")) {
      if (opts->services->len == 0) {
        g_ptr_array_add(opts->services, g_strdup(arg));
      } else if (!opts->port_spec) {
        opts->port_spec = g_strdup(arg);
      } else {
        fail("port accepts one service and one private port argument");
        return FALSE;
      }
    } else if (opts->command && g_str_equal(arg, "--volumes")) {
      opts->remove_volumes = TRUE;
    } else if (opts->command && !g_str_has_prefix(arg, "-")) {
      g_ptr_array_add(opts->services, g_strdup(arg));
    } else if (g_str_equal(arg, "--version") || g_str_equal(arg, "version")) {
      g_print("quocker 0.1\n");
      exit(0);
    } else {
      fail("unknown option or argument: %s", arg);
      return FALSE;
    }
  }
  if (!opts->command) {
    usage(stderr);
    return FALSE;
  }
  if (opts->config_list_mode && opts->config_format) {
    fail("config output selection cannot be combined with --format");
    return FALSE;
  }
  if (g_str_equal(opts->command, "port") &&
      (opts->services->len != 1 || !opts->port_spec)) {
    fail("port requires SERVICE PRIVATE_PORT[/PROTOCOL]");
    return FALSE;
  }
  return TRUE;
}

static void json_add_string_array(JsonBuilder *builder, const char *name,
                                 const GPtrArray *values) {
  json_builder_set_member_name(builder, name);
  json_builder_begin_array(builder);
  if (values) {
    for (guint i = 0; i < values->len; i++) {
      json_builder_add_string_value(builder,
                                    g_ptr_array_index((GPtrArray *)values, i));
    }
  }
  json_builder_end_array(builder);
}

static void kernel_selection_write_json(
    const QuockerKernel *kernel, const char *platform,
    const char *requested_id, const char *minimum_version,
    const GPtrArray *required_features,
    const GPtrArray *required_module_releases,
    const QuockerDistroInfo *distro) {
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "platform");
  json_builder_add_string_value(builder, platform);
  json_builder_set_member_name(builder, "requirements");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kernel_id");
  if (requested_id) {
    json_builder_add_string_value(builder, requested_id);
  } else {
    json_builder_add_null_value(builder);
  }
  json_builder_set_member_name(builder, "minimum_version");
  if (minimum_version) {
    json_builder_add_string_value(builder, minimum_version);
  } else {
    json_builder_add_null_value(builder);
  }
  json_add_string_array(builder, "required_features", required_features);
  json_add_string_array(builder, "required_module_releases",
                        required_module_releases);
  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "selection");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "id");
  json_builder_add_string_value(builder, kernel->id);
  json_builder_set_member_name(builder, "version");
  if (kernel->version) {
    json_builder_add_string_value(builder, kernel->version);
  } else {
    json_builder_add_null_value(builder);
  }
  json_builder_set_member_name(builder, "reason");
  json_builder_add_string_value(builder, kernel->reason);
  json_builder_set_member_name(builder, "distro_id");
  if (distro->id) {
    json_builder_add_string_value(builder, distro->id);
  } else {
    json_builder_add_null_value(builder);
  }
  json_builder_set_member_name(builder, "distro_id_like");
  if (distro->id_like) {
    json_builder_add_string_value(builder, distro->id_like);
  } else {
    json_builder_add_null_value(builder);
  }
  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "module_evidence");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "advisory");
  json_builder_add_boolean_value(builder, TRUE);
  json_builder_set_member_name(builder, "detected");
  json_builder_add_boolean_value(builder, kernel->module_release_evidence);
  json_builder_set_member_name(builder, "matched");
  if (kernel->module_release_evidence) {
    json_builder_add_boolean_value(builder, kernel->module_release_match);
  } else {
    json_builder_add_null_value(builder);
  }
  json_add_string_array(builder, "observed_releases",
                        kernel->observed_module_releases);
  json_add_string_array(builder, "selected_kernel_releases",
                        kernel->module_releases);
  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "assets");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "verified");
  json_builder_add_boolean_value(builder, kernel->assets_verified);
  json_builder_set_member_name(builder, "kernel");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "path");
  json_builder_add_string_value(builder, kernel->kernel_path);
  json_builder_set_member_name(builder, "sha256");
  json_builder_add_string_value(builder, kernel->kernel_digest);
  json_builder_end_object(builder);
  json_builder_set_member_name(builder, "initrd");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "path");
  json_builder_add_string_value(builder, kernel->initrd_path);
  json_builder_set_member_name(builder, "sha256");
  json_builder_add_string_value(builder, kernel->initrd_digest);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, root);
  char *text = json_generator_to_data(generator, NULL);
  g_print("%s\n", text);
  g_free(text);
  g_object_unref(generator);
  json_node_free(root);
  g_object_unref(builder);
}

static int kernel_select_command(int argc, char **argv, gboolean fetch_assets) {
  const char *platform = NULL;
  const char *rootfs = NULL;
  const char *requested_id = NULL;
  const char *minimum_version = NULL;
  gboolean json_output = FALSE;
  GPtrArray *required_features = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *required_module_releases =
      g_ptr_array_new_with_free_func(g_free);
  const char *catalog = g_getenv("QUOCKER_KERNEL_CATALOG");
  const char *public_key = g_getenv("QUOCKER_KERNEL_CATALOG_PUBKEY");
  if (!catalog || !*catalog) {
    catalog = default_kernel_catalog_path();
  }
  if (!public_key || !*public_key) {
    public_key = "/etc/quocker/kernel-catalog.pub";
  }
  for (int i = 3; i < argc; i++) {
    const char **target = NULL;
    if (g_str_equal(argv[i], "--platform")) {
      target = &platform;
    } else if (g_str_equal(argv[i], "--catalog")) {
      target = &catalog;
    } else if (g_str_equal(argv[i], "--pubkey")) {
      target = &public_key;
    } else if (g_str_equal(argv[i], "--rootfs")) {
      target = &rootfs;
    } else if (g_str_equal(argv[i], "--id")) {
      target = &requested_id;
    } else if (g_str_equal(argv[i], "--minimum")) {
      target = &minimum_version;
    } else if (g_str_equal(argv[i], "--json")) {
      json_output = TRUE;
      continue;
    } else if (g_str_equal(argv[i], "--require")) {
      if (++i >= argc) {
        fail("option --require requires a value");
        g_ptr_array_free(required_features, TRUE);
        return 2;
      }
      gchar **tokens = g_strsplit(argv[i], ",", -1);
      for (guint j = 0; tokens[j]; j++) {
        char *token = g_strstrip(tokens[j]);
        if (*token) {
          g_ptr_array_add(required_features, g_strdup(token));
        }
      }
      g_strfreev(tokens);
      continue;
    } else if (g_str_equal(argv[i], "--module-release")) {
      if (++i >= argc) {
        fail("option --module-release requires a value");
        g_ptr_array_free(required_features, TRUE);
        g_ptr_array_free(required_module_releases, TRUE);
        return 2;
      }
      YNode *release = node_new(NODE_SCALAR, TAG_STR);
      release->scalar = g_strdup(argv[i]);
      gboolean valid = append_kernel_module_releases(
          required_module_releases, release, "kernel select");
      node_free(release);
      if (!valid) {
        g_ptr_array_free(required_features, TRUE);
        g_ptr_array_free(required_module_releases, TRUE);
        return 2;
      }
      continue;
    } else {
      fail("unknown kernel-select option '%s'", argv[i]);
      g_ptr_array_free(required_features, TRUE);
      g_ptr_array_free(required_module_releases, TRUE);
      return 2;
    }
    if (++i >= argc) {
      fail("option %s requires a value", argv[i - 1]);
      g_ptr_array_free(required_features, TRUE);
      g_ptr_array_free(required_module_releases, TRUE);
      return 2;
    }
    *target = argv[i];
  }
  if (!platform) {
    fail("kernel select requires --platform linux/ARCH[/VARIANT]");
    g_ptr_array_free(required_features, TRUE);
    g_ptr_array_free(required_module_releases, TRUE);
    return 2;
  }
  if (fetch_assets && !requested_id) {
    fail("kernel fetch requires --id");
    g_ptr_array_free(required_features, TRUE);
    g_ptr_array_free(required_module_releases, TRUE);
    return 2;
  }
  QuockerDistroInfo distro = {0};
  gboolean have_rootfs_hints = FALSE;
  if (rootfs) {
    have_rootfs_hints = quocker_rootfs_detect_distro(rootfs, &distro);
  }
  QuockerKernel *kernel = NULL;
  GError *error = NULL;
  gboolean ok = quocker_kernel_select_for_image(
      catalog, public_key, platform, have_rootfs_hints ? &distro : NULL,
      requested_id, minimum_version, required_features,
      required_module_releases, &kernel, &error);
  if (!ok) {
    fail("%s", error ? error->message : "kernel selection failed");
  } else if (fetch_assets &&
             !quocker_kernel_fetch_assets(kernel, &error)) {
    fail("%s", error ? error->message : "kernel asset fetch failed");
    ok = FALSE;
  } else if (json_output) {
    kernel_selection_write_json(kernel, platform, requested_id, minimum_version,
                                required_features, required_module_releases,
                                &distro);
  } else {
    g_print("Kernel: %s\nPlatform: %s/%s%s%s\nVersion: %s\nReason: %s\n"
            "Module evidence: %s\nAssets verified: %s\n"
            "Kernel asset: %s (%s)\nInitrd asset: %s (%s)\n",
            kernel->id, kernel->os, kernel->architecture,
            kernel->variant ? "/" : "", kernel->variant ? kernel->variant : "",
            kernel->version ? kernel->version : "(unspecified)", kernel->reason,
            !kernel->module_release_evidence
                ? "none detected"
                : kernel->module_release_match
                      ? "matched a catalog release (advisory)"
                      : "unmatched; advisory only",
            kernel->assets_verified ? "yes" : "no (fetch with quocker kernel fetch)",
            kernel->kernel_path, kernel->kernel_digest, kernel->initrd_path,
            kernel->initrd_digest);
    if (kernel->features->len) {
      g_print("Features:");
      for (guint i = 0; i < kernel->features->len; i++) {
        g_print(" %s", (char *)g_ptr_array_index(kernel->features, i));
      }
      g_print("\n");
    }
    if (distro.id) {
      g_print("Guest distro hint: %s%s%s%s\n", distro.id,
              distro.version_id ? " " : "",
              distro.version_id ? distro.version_id : "",
              distro.id_like ? " (ID_LIKE recorded)" : "");
    }
    if (distro.kernel_module_releases && distro.kernel_module_releases->len) {
      g_print("Observed module releases:");
      for (guint i = 0; i < distro.kernel_module_releases->len; i++) {
        g_print(" %s",
                (char *)g_ptr_array_index(distro.kernel_module_releases, i));
      }
      g_print("\n");
    }
  }
  g_clear_error(&error);
  quocker_kernel_free(kernel);
  quocker_distro_info_clear(&distro);
  g_ptr_array_free(required_features, TRUE);
  g_ptr_array_free(required_module_releases, TRUE);
  return ok ? 0 : 1;
}

static int kernel_update_command(int argc, char **argv) {
  const char *url = NULL;
  const char *catalog = g_getenv("QUOCKER_KERNEL_CATALOG");
  const char *public_key = g_getenv("QUOCKER_KERNEL_CATALOG_PUBKEY");
  char *default_catalog = NULL;
  if (!catalog || !*catalog) {
    default_catalog = user_kernel_catalog_path();
    catalog = default_catalog;
  }
  if (!public_key || !*public_key) {
    public_key = "/etc/quocker/kernel-catalog.pub";
  }
  for (int i = 3; i < argc; i++) {
    const char **target = NULL;
    if (g_str_equal(argv[i], "--url")) {
      target = &url;
    } else if (g_str_equal(argv[i], "--catalog")) {
      target = &catalog;
    } else if (g_str_equal(argv[i], "--pubkey")) {
      target = &public_key;
    } else {
      fail("unknown kernel-update option '%s'", argv[i]);
      g_free(default_catalog);
      return 2;
    }
    if (++i >= argc) {
      fail("option %s requires a value", argv[i - 1]);
      g_free(default_catalog);
      return 2;
    }
    *target = argv[i];
  }
  if (!url) {
    fail("kernel update requires --url HTTPS_URL");
    g_free(default_catalog);
    return 2;
  }
  GError *error = NULL;
  gboolean ok = quocker_kernel_catalog_update(url, catalog, public_key, &error);
  if (!ok) {
    fail("%s", error ? error->message : "kernel catalog update failed");
  }
  g_clear_error(&error);
  g_free(default_catalog);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc >= 3 && g_str_equal(argv[1], "kernel")) {
    if (g_str_equal(argv[2], "select")) {
      return kernel_select_command(argc, argv, FALSE);
    }
    if (g_str_equal(argv[2], "fetch")) {
      return kernel_select_command(argc, argv, TRUE);
    }
    if (g_str_equal(argv[2], "update")) {
      return kernel_update_command(argc, argv);
    }
    fail("unknown kernel command '%s'", argv[2]);
    usage(stderr);
    return 2;
  }
  Options opts = {0};
  if (!parse_options(argc, argv, &opts)) {
    return 2;
  }
  if (g_str_equal(opts.command, "prune")) {
    char *cache =
        g_build_filename(g_get_user_cache_dir(), "quocker", "oci", NULL);
    gboolean ok = quocker_oci_cache_prune(cache);
    g_free(cache);
    return ok ? 0 : 1;
  }
  char *root = NULL;
  GPtrArray *files = resolve_files(&opts, &root);
  YNode *config = NULL;
  GHashTable *project_environment = NULL;
  char *project_lower = NULL;
  if (!parse_compose_files(files, root, opts.env_files, opts.project_name,
                           &config, &project_environment, &project_lower)) {
    return 1;
  }
  YNode *services = map_get(config, "services");
  if (!services || services->kind != NODE_MAPPING || !services->items->len) {
    fail("Compose configuration must contain a non-empty services mapping");
    return 1;
  }
  char *directory = service_dir(root, project_lower);
  if (!g_str_equal(opts.command, "config") &&
      !g_str_equal(opts.command, "port") && !opts.dry_run) {
    if (g_mkdir_with_parents(directory, 0700) < 0) {
      fail("cannot create state directory %s: %s", directory,
           g_strerror(errno));
      return 1;
    }
  }
  gboolean ok = FALSE;
  if (g_str_equal(opts.command, "config")) {
    ok = opts.quiet ||
         (opts.config_list_mode
              ? (g_str_equal(opts.config_list_mode, "environment")
                     ? config_write_environment(project_environment)
                     : config_write_list(config, opts.config_list_mode))
              : (g_strcmp0(opts.config_format, "json") == 0
                     ? json_write_stdout(config)
                     : yaml_write_stdout(config)));
  } else if (g_str_equal(opts.command, "up") ||
             g_str_equal(opts.command, "start")) {
    ok = run_up_services(services, &opts, project_lower, root, directory,
                         project_environment,
                         g_str_equal(opts.command, "start"));
  } else if (g_str_equal(opts.command, "stop")) {
    ServiceSignalContext context = {project_lower, directory, opts.dry_run,
                                    SIGTERM};
    ok = for_down_services(services, &opts, stop_one, &context);
  } else if (g_str_equal(opts.command, "kill")) {
    ServiceSignalContext context = {project_lower, directory, opts.dry_run,
                                    opts.signal_number};
    ok = for_down_services(services, &opts, kill_one, &context);
  } else if (g_str_equal(opts.command, "pause") ||
             g_str_equal(opts.command, "unpause")) {
    gboolean pause = g_str_equal(opts.command, "pause");
    VmControlContext context = {
        project_lower, directory, opts.dry_run, pause ? "stop" : "cont",
        pause ? "paused" : "unpaused"};
    ok = for_down_services(services, &opts, control_one, &context);
  } else if (g_str_equal(opts.command, "restart")) {
    ServiceSignalContext stop_context = {project_lower, directory,
                                         opts.dry_run, SIGTERM};
    ok = for_down_services(services, &opts, stop_one, &stop_context);
    if (ok) {
      ok = run_up_services(services, &opts, project_lower, root, directory,
                           project_environment, TRUE);
    }
  } else if (g_str_equal(opts.command, "down") ||
             g_str_equal(opts.command, "rm")) {
    DownContext context = {project_lower, directory,
                           opts.remove_volumes ||
                               g_str_equal(opts.command, "rm"),
                           opts.dry_run};
    ok = for_down_services(services, &opts, down_one, &context);
  } else if (g_str_equal(opts.command, "ps")) {
    g_print("NAME\tSTATE\tPID\tDISK\n");
    ListContext context = {project_lower, directory};
    ok = for_services(services, &opts, ps_one, &context);
  } else if (g_str_equal(opts.command, "port")) {
    PortContext context = {opts.port_spec, directory, project_lower};
    ok = for_services(services, &opts, port_one, &context);
  } else if (g_str_equal(opts.command, "logs")) {
    LogsContext context = {directory, &opts};
    ok = for_services(services, &opts, logs_one, &context);
    if (ok && opts.follow) {
      follow_logs(services, &opts, directory);
    }
  } else if (g_str_equal(opts.command, "pull")) {
    const char *cache_home = g_get_user_cache_dir();
    char *cache = g_build_filename(cache_home, "quocker", "oci", NULL);
    const char *catalog = g_getenv("QUOCKER_KERNEL_CATALOG");
    const char *public_key = g_getenv("QUOCKER_KERNEL_CATALOG_PUBKEY");
    PullContext context = {
        cache,
        g_getenv("QUOCKER_REGISTRY_MIRROR"),
        catalog && *catalog ? catalog : default_kernel_catalog_path(),
        public_key && *public_key ? public_key
                                  : "/etc/quocker/kernel-catalog.pub",
    };
    ok = for_services(services, &opts, pull_one, &context);
    g_free(cache);
  } else {
    fail("unsupported command '%s'", opts.command);
    usage(stderr);
  }
  node_free(config);
  g_ptr_array_free(files, TRUE);
  g_ptr_array_free(opts.files, TRUE);
  g_ptr_array_free(opts.env_files, TRUE);
  g_ptr_array_free(opts.profiles, TRUE);
  g_ptr_array_free(opts.services, TRUE);
  g_free(opts.command);
  g_free(opts.config_format);
  g_free(opts.config_list_mode);
  g_free(opts.port_spec);
  g_free(opts.project_name);
  g_free(opts.project_directory);
  g_free(project_lower);
  g_free(directory);
  g_free(root);
  return ok ? 0 : 1;
}
