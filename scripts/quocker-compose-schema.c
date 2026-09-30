/*
 * Quocker - Compose Specification schema validation.
 *
 * Copyright (C) 2026 The Quocker Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 */

#include "quocker-compose-schema.h"

#include <math.h>
#include <string.h>

/* JSON-GLib's type-test macros call json_node_get_node_type() on NULL.
 * Optional schema keywords routinely produce NULL members, so make those
 * checks explicitly nullable in this translation unit. */
#undef JSON_NODE_HOLDS
#define JSON_NODE_HOLDS(node, type)                                            \
  ((node) != NULL && json_node_get_node_type(node) == (type))

static void schema_error(char **error, const char *path, const char *message) {
  if (error && !*error) {
    *error = g_strdup_printf("Compose schema: %s: %s", path, message);
  }
}

static JsonNode *schema_ref(JsonNode *root, const char *reference) {
  static const char prefix[] = "#/$defs/";
  if (!g_str_has_prefix(reference, prefix) ||
      strchr(reference + sizeof(prefix) - 1, '/')) {
    return NULL;
  }
  JsonObject *defs =
      json_object_get_object_member(json_node_get_object(root), "$defs");
  return defs ? json_object_get_member(defs, reference + sizeof(prefix) - 1)
              : NULL;
}

static gboolean node_is_type(JsonNode *node, const char *type) {
  if (g_str_equal(type, "object")) {
    return JSON_NODE_HOLDS_OBJECT(node);
  }
  if (g_str_equal(type, "array")) {
    return JSON_NODE_HOLDS_ARRAY(node);
  }
  if (g_str_equal(type, "null")) {
    return JSON_NODE_HOLDS_NULL(node);
  }
  if (!JSON_NODE_HOLDS_VALUE(node)) {
    return FALSE;
  }
  GType value_type = json_node_get_value_type(node);
  if (g_str_equal(type, "string")) {
    return value_type == G_TYPE_STRING;
  }
  if (g_str_equal(type, "boolean")) {
    return value_type == G_TYPE_BOOLEAN;
  }
  if (g_str_equal(type, "integer")) {
    return value_type == G_TYPE_INT64 || value_type == G_TYPE_INT ||
           value_type == G_TYPE_LONG || value_type == G_TYPE_UINT64 ||
           value_type == G_TYPE_UINT || value_type == G_TYPE_ULONG;
  }
  if (g_str_equal(type, "number")) {
    return node_is_type(node, "integer") || value_type == G_TYPE_DOUBLE;
  }
  return FALSE;
}

static gboolean schema_number(JsonNode *node, double *value) {
  if (!node || !JSON_NODE_HOLDS_VALUE(node)) {
    return FALSE;
  }
  GType type = json_node_get_value_type(node);
  if (type == G_TYPE_DOUBLE) {
    *value = json_node_get_double(node);
    return isfinite(*value);
  }
  if (type == G_TYPE_INT64 || type == G_TYPE_INT || type == G_TYPE_LONG ||
      type == G_TYPE_UINT64 || type == G_TYPE_UINT || type == G_TYPE_ULONG) {
    *value = (double)json_node_get_int(node);
    return TRUE;
  }
  return FALSE;
}

static gboolean validate_node(JsonNode *instance, JsonNode *schema,
                              JsonNode *root, const char *path,
                              GHashTable **evaluated_out, char **error,
                              guint depth);

static GHashTable *evaluated_new(void) {
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void evaluated_merge(GHashTable *into, GHashTable *from) {
  if (!from) {
    return;
  }
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, from);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    g_hash_table_add(into, g_strdup(key));
  }
}

static gboolean matches_pattern(const char *pattern, const char *text) {
  GError *error = NULL;
  GRegex *regex = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0, &error);
  if (!regex) {
    g_clear_error(&error);
    return FALSE;
  }
  gboolean matches = g_regex_match(regex, text, 0, NULL);
  g_regex_unref(regex);
  return matches;
}

static char *child_path(const char *path, const char *member) {
  return g_strdup_printf("%s/%s", path, member);
}

static gboolean keyword_in(const char *keyword, const char *const *keywords) {
  for (const char *const *item = keywords; *item; item++) {
    if (g_str_equal(keyword, *item)) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean preflight_schema_keywords(JsonNode *schema, const char *path,
                                          char **error) {
  if (!JSON_NODE_HOLDS_OBJECT(schema)) {
    return TRUE;
  }
  static const char *const implemented[] = {"$defs",
                                            "$id",
                                            "$ref",
                                            "$schema",
                                            "additionalProperties",
                                            "allOf",
                                            "anyOf",
                                            "enum",
                                            "items",
                                            "maximum",
                                            "minimum",
                                            "oneOf",
                                            "pattern",
                                            "patternProperties",
                                            "properties",
                                            "required",
                                            "type",
                                            "unevaluatedProperties",
                                            "uniqueItems",
                                            NULL};
  static const char *const recognized_but_unsupported[] = {"$anchor",
                                                           "$comment",
                                                           "$dynamicAnchor",
                                                           "$dynamicRef",
                                                           "$vocabulary",
                                                           "const",
                                                           "contains",
                                                           "contentEncoding",
                                                           "contentMediaType",
                                                           "contentSchema",
                                                           "dependentRequired",
                                                           "dependentSchemas",
                                                           "else",
                                                           "exclusiveMaximum",
                                                           "exclusiveMinimum",
                                                           "format",
                                                           "if",
                                                           "maxContains",
                                                           "maxItems",
                                                           "maxLength",
                                                           "maxProperties",
                                                           "minContains",
                                                           "minItems",
                                                           "minLength",
                                                           "minProperties",
                                                           "multipleOf",
                                                           "not",
                                                           "prefixItems",
                                                           "propertyNames",
                                                           "then",
                                                           "unevaluatedItems",
                                                           "writeOnly",
                                                           "readOnly",
                                                           NULL};
  JsonObject *object = json_node_get_object(schema);
  GList *members = json_object_get_members(object);
  for (GList *member = members; member; member = member->next) {
    const char *name = member->data;
    if (keyword_in(name, recognized_but_unsupported)) {
      char *message = g_strdup_printf(
          "pinned schema uses unimplemented JSON Schema keyword '%s'", name);
      schema_error(error, path, message);
      g_free(message);
      g_list_free(members);
      return FALSE;
    }
    if (!keyword_in(name, implemented)) {
      continue; /* annotations and Compose schema metadata */
    }
    JsonNode *value = json_object_get_member(object, name);
    if (g_str_equal(name, "$defs") || g_str_equal(name, "properties") ||
        g_str_equal(name, "patternProperties")) {
      if (JSON_NODE_HOLDS_OBJECT(value)) {
        JsonObject *children = json_node_get_object(value);
        GList *keys = json_object_get_members(children);
        for (GList *key = keys; key; key = key->next) {
          char *next = child_path(path, key->data);
          gboolean valid = preflight_schema_keywords(
              json_object_get_member(children, key->data), next, error);
          g_free(next);
          if (!valid) {
            g_list_free(keys);
            g_list_free(members);
            return FALSE;
          }
        }
        g_list_free(keys);
      }
    } else if (g_str_equal(name, "allOf") || g_str_equal(name, "anyOf") ||
               g_str_equal(name, "oneOf")) {
      if (JSON_NODE_HOLDS_ARRAY(value)) {
        JsonArray *children = json_node_get_array(value);
        for (guint i = 0; i < json_array_get_length(children); i++) {
          char *next = g_strdup_printf("%s/%s/%u", path, name, i);
          gboolean valid = preflight_schema_keywords(
              json_array_get_element(children, i), next, error);
          g_free(next);
          if (!valid) {
            g_list_free(members);
            return FALSE;
          }
        }
      }
    } else if (g_str_equal(name, "additionalProperties") ||
               g_str_equal(name, "unevaluatedProperties") ||
               g_str_equal(name, "items")) {
      char *next = child_path(path, name);
      gboolean valid = preflight_schema_keywords(value, next, error);
      g_free(next);
      if (!valid) {
        g_list_free(members);
        return FALSE;
      }
    }
  }
  g_list_free(members);
  return TRUE;
}

static gboolean validate_subschema(JsonNode *instance, JsonNode *schema,
                                   JsonNode *root, const char *path,
                                   GHashTable *evaluated, char **error,
                                   guint depth) {
  GHashTable *child_evaluated = NULL;
  gboolean valid = validate_node(instance, schema, root, path, &child_evaluated,
                                 error, depth + 1);
  if (valid) {
    evaluated_merge(evaluated, child_evaluated);
  }
  if (child_evaluated) {
    g_hash_table_destroy(child_evaluated);
  }
  return valid;
}

static gboolean validate_combinator(JsonNode *instance, JsonArray *schemas,
                                    JsonNode *root, const char *path,
                                    GHashTable *evaluated, char **error,
                                    guint depth, const char *kind) {
  guint count = json_array_get_length(schemas);
  guint matched = 0;
  GPtrArray *successful =
      g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);
  for (guint i = 0; i < count; i++) {
    JsonNode *subschema = json_array_get_element(schemas, i);
    GHashTable *annotations = NULL;
    char *branch_error = NULL;
    gboolean valid = validate_node(instance, subschema, root, path,
                                   &annotations, &branch_error, depth + 1);
    g_free(branch_error);
    if (valid) {
      matched++;
      g_ptr_array_add(successful, annotations ? annotations : evaluated_new());
    } else if (annotations) {
      g_hash_table_destroy(annotations);
    }
  }
  gboolean valid = g_str_equal(kind, "allOf")   ? matched == count
                   : g_str_equal(kind, "anyOf") ? matched > 0
                                                : matched == 1;
  if (valid) {
    for (guint i = 0; i < successful->len; i++) {
      evaluated_merge(evaluated, g_ptr_array_index(successful, i));
    }
  } else {
    char *message = g_strdup_printf("does not satisfy %s", kind);
    schema_error(error, path, message);
    g_free(message);
  }
  g_ptr_array_free(successful, TRUE);
  return valid;
}

static gboolean validate_node(JsonNode *instance, JsonNode *schema,
                              JsonNode *root, const char *path,
                              GHashTable **evaluated_out, char **error,
                              guint depth) {
  GHashTable *evaluated = evaluated_new();
  if (evaluated_out) {
    *evaluated_out = evaluated;
  }
  if (depth > 256) {
    schema_error(error, path, "schema reference nesting exceeds 256 levels");
    return FALSE;
  }
  if (JSON_NODE_HOLDS_VALUE(schema) &&
      json_node_get_value_type(schema) == G_TYPE_BOOLEAN) {
    gboolean allowed = json_node_get_boolean(schema);
    if (!allowed) {
      schema_error(error, path, "is rejected by the schema");
    }
    return allowed;
  }
  if (!JSON_NODE_HOLDS_OBJECT(schema)) {
    schema_error(error, path, "schema contains an unsupported construct");
    return FALSE;
  }
  JsonObject *rules = json_node_get_object(schema);
  JsonNode *reference_node = json_object_get_member(rules, "$ref");
  if (reference_node && JSON_NODE_HOLDS_VALUE(reference_node)) {
    const char *reference = json_node_get_string(reference_node);
    JsonNode *target = reference ? schema_ref(root, reference) : NULL;
    if (!target) {
      schema_error(error, path, "schema has an unresolved local reference");
      return FALSE;
    }
    if (!validate_subschema(instance, target, root, path, evaluated, error,
                            depth)) {
      return FALSE;
    }
  }

  JsonNode *type_rule = json_object_get_member(rules, "type");
  if (type_rule) {
    gboolean matches = FALSE;
    if (JSON_NODE_HOLDS_VALUE(type_rule)) {
      const char *type = json_node_get_string(type_rule);
      matches = type && node_is_type(instance, type);
    } else if (JSON_NODE_HOLDS_ARRAY(type_rule)) {
      JsonArray *types = json_node_get_array(type_rule);
      for (guint i = 0; i < json_array_get_length(types); i++) {
        JsonNode *type = json_array_get_element(types, i);
        const char *name =
            JSON_NODE_HOLDS_VALUE(type) ? json_node_get_string(type) : NULL;
        matches |= name && node_is_type(instance, name);
      }
    }
    if (!matches) {
      schema_error(error, path, "has the wrong type");
      return FALSE;
    }
  }

  JsonNode *enum_rule = json_object_get_member(rules, "enum");
  if (enum_rule && JSON_NODE_HOLDS_ARRAY(enum_rule)) {
    gboolean found = FALSE;
    JsonArray *values = json_node_get_array(enum_rule);
    for (guint i = 0; i < json_array_get_length(values); i++) {
      found |= json_node_equal(instance, json_array_get_element(values, i));
    }
    if (!found) {
      schema_error(error, path, "value is not in the allowed enum");
      return FALSE;
    }
  }

  static const char *const combinators[] = {"allOf", "anyOf", "oneOf", NULL};
  for (const char *const *name = combinators; *name; name++) {
    JsonNode *rule = json_object_get_member(rules, *name);
    if (rule && JSON_NODE_HOLDS_ARRAY(rule) &&
        !validate_combinator(instance, json_node_get_array(rule), root, path,
                             evaluated, error, depth, *name)) {
      return FALSE;
    }
  }

  if (JSON_NODE_HOLDS_OBJECT(instance)) {
    JsonObject *object = json_node_get_object(instance);
    JsonNode *properties_node = json_object_get_member(rules, "properties");
    JsonNode *patterns_node =
        json_object_get_member(rules, "patternProperties");
    JsonObject *properties = JSON_NODE_HOLDS_OBJECT(properties_node)
                                 ? json_node_get_object(properties_node)
                                 : NULL;
    JsonObject *patterns = JSON_NODE_HOLDS_OBJECT(patterns_node)
                               ? json_node_get_object(patterns_node)
                               : NULL;
    GList *members = json_object_get_members(object);
    for (GList *member = members; member; member = member->next) {
      const char *key = member->data;
      JsonNode *value = json_object_get_member(object, key);
      gboolean matched = FALSE;
      JsonNode *property_schema =
          properties ? json_object_get_member(properties, key) : NULL;
      if (property_schema) {
        matched = TRUE;
        g_hash_table_add(evaluated, g_strdup(key));
        char *next_path = child_path(path, key);
        gboolean valid = validate_subschema(value, property_schema, root,
                                            next_path, evaluated, error, depth);
        g_free(next_path);
        if (!valid) {
          g_list_free(members);
          return FALSE;
        }
      }
      if (patterns) {
        GList *pattern_names = json_object_get_members(patterns);
        for (GList *pattern = pattern_names; pattern; pattern = pattern->next) {
          const char *expression = pattern->data;
          if (matches_pattern(expression, key)) {
            matched = TRUE;
            g_hash_table_add(evaluated, g_strdup(key));
            char *next_path = child_path(path, key);
            gboolean valid = validate_subschema(
                value, json_object_get_member(patterns, expression), root,
                next_path, evaluated, error, depth);
            g_free(next_path);
            if (!valid) {
              g_list_free(pattern_names);
              g_list_free(members);
              return FALSE;
            }
          }
        }
        g_list_free(pattern_names);
      }
      JsonNode *additional =
          json_object_get_member(rules, "additionalProperties");
      if (!matched && additional) {
        if (JSON_NODE_HOLDS_VALUE(additional) &&
            json_node_get_value_type(additional) == G_TYPE_BOOLEAN &&
            !json_node_get_boolean(additional)) {
          char *message =
              g_strdup_printf("contains unknown property '%s'", key);
          schema_error(error, path, message);
          g_free(message);
          g_list_free(members);
          return FALSE;
        }
        if (JSON_NODE_HOLDS_OBJECT(additional) ||
            JSON_NODE_HOLDS_VALUE(additional)) {
          g_hash_table_add(evaluated, g_strdup(key));
          char *next_path = child_path(path, key);
          gboolean valid = validate_subschema(
              value, additional, root, next_path, evaluated, error, depth);
          g_free(next_path);
          if (!valid) {
            g_list_free(members);
            return FALSE;
          }
        }
      }
    }
    g_list_free(members);

    JsonNode *required_node = json_object_get_member(rules, "required");
    if (required_node && JSON_NODE_HOLDS_ARRAY(required_node)) {
      JsonArray *required = json_node_get_array(required_node);
      for (guint i = 0; i < json_array_get_length(required); i++) {
        JsonNode *required_name = json_array_get_element(required, i);
        const char *key = JSON_NODE_HOLDS_VALUE(required_name)
                              ? json_node_get_string(required_name)
                              : NULL;
        if (key && !json_object_has_member(object, key)) {
          char *message =
              g_strdup_printf("is missing required property '%s'", key);
          schema_error(error, path, message);
          g_free(message);
          return FALSE;
        }
      }
    }

    JsonNode *unevaluated =
        json_object_get_member(rules, "unevaluatedProperties");
    if (unevaluated) {
      members = json_object_get_members(object);
      for (GList *member = members; member; member = member->next) {
        const char *key = member->data;
        if (!g_hash_table_contains(evaluated, key)) {
          if (JSON_NODE_HOLDS_VALUE(unevaluated) &&
              json_node_get_value_type(unevaluated) == G_TYPE_BOOLEAN &&
              !json_node_get_boolean(unevaluated)) {
            char *message =
                g_strdup_printf("contains unsupported property '%s'", key);
            schema_error(error, path, message);
            g_free(message);
            g_list_free(members);
            return FALSE;
          }
          char *next_path = child_path(path, key);
          gboolean valid = validate_subschema(
              json_object_get_member(object, key), unevaluated, root, next_path,
              evaluated, error, depth);
          g_free(next_path);
          if (!valid) {
            g_list_free(members);
            return FALSE;
          }
          g_hash_table_add(evaluated, g_strdup(key));
        }
      }
      g_list_free(members);
    }
  }

  if (JSON_NODE_HOLDS_ARRAY(instance)) {
    JsonArray *array = json_node_get_array(instance);
    JsonNode *items_rule = json_object_get_member(rules, "items");
    if (items_rule) {
      for (guint i = 0; i < json_array_get_length(array); i++) {
        char *next_path = g_strdup_printf("%s/%u", path, i);
        gboolean valid =
            validate_subschema(json_array_get_element(array, i), items_rule,
                               root, next_path, evaluated, error, depth);
        g_free(next_path);
        if (!valid) {
          return FALSE;
        }
      }
    }
    JsonNode *unique = json_object_get_member(rules, "uniqueItems");
    if (unique && JSON_NODE_HOLDS_VALUE(unique) &&
        json_node_get_value_type(unique) == G_TYPE_BOOLEAN &&
        json_node_get_boolean(unique)) {
      for (guint i = 0; i < json_array_get_length(array); i++) {
        for (guint j = i + 1; j < json_array_get_length(array); j++) {
          if (json_node_equal(json_array_get_element(array, i),
                              json_array_get_element(array, j))) {
            schema_error(error, path, "array items must be unique");
            return FALSE;
          }
        }
      }
    }
  }

  if (JSON_NODE_HOLDS_VALUE(instance) &&
      json_node_get_value_type(instance) == G_TYPE_STRING) {
    const char *text = json_node_get_string(instance);
    JsonNode *pattern = json_object_get_member(rules, "pattern");
    if (pattern && JSON_NODE_HOLDS_VALUE(pattern) &&
        !matches_pattern(json_node_get_string(pattern), text)) {
      schema_error(error, path, "string does not match required pattern");
      return FALSE;
    }
  }

  double value;
  if (schema_number(instance, &value)) {
    double bound;
    JsonNode *minimum = json_object_get_member(rules, "minimum");
    if (minimum && schema_number(minimum, &bound) && value < bound) {
      schema_error(error, path, "number is below minimum");
      return FALSE;
    }
    JsonNode *maximum = json_object_get_member(rules, "maximum");
    if (maximum && schema_number(maximum, &bound) && value > bound) {
      schema_error(error, path, "number is above maximum");
      return FALSE;
    }
  }
  return TRUE;
}

gboolean quocker_compose_schema_validate(const char *schema_path,
                                         JsonNode *instance, char **error) {
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;
  if (!json_parser_load_from_file(parser, schema_path, &parse_error)) {
    if (error) {
      *error =
          g_strdup_printf("cannot load Compose schema '%s': %s", schema_path,
                          parse_error ? parse_error->message : "parse error");
    }
    g_clear_error(&parse_error);
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *schema = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  JsonObject *schema_object =
      JSON_NODE_HOLDS_OBJECT(schema) ? json_node_get_object(schema) : NULL;
  JsonNode *dialect =
      schema_object ? json_object_get_member(schema_object, "$schema") : NULL;
  const char *dialect_uri =
      JSON_NODE_HOLDS_VALUE(dialect) ? json_node_get_string(dialect) : NULL;
  gboolean preflight_ok =
      g_strcmp0(dialect_uri, "https://json-schema.org/draft/2020-12/schema") ==
          0 &&
      preflight_schema_keywords(schema, "$schema", error);
  if (!preflight_ok) {
    if (error && !*error) {
      *error = g_strdup("pinned Compose schema must use JSON Schema 2020-12");
    }
    json_node_free(schema);
    return FALSE;
  }
  GHashTable *evaluated = NULL;
  gboolean valid =
      validate_node(instance, schema, schema, "$", &evaluated, error, 0);
  if (evaluated) {
    g_hash_table_destroy(evaluated);
  }
  json_node_free(schema);
  return valid;
}
