/*
 * Quocker - Compose Specification schema validation.
 *
 * Copyright (C) 2026 The Quocker Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 */

#ifndef QUOCKER_COMPOSE_SCHEMA_H
#define QUOCKER_COMPOSE_SCHEMA_H

#include <json-glib/json-glib.h>

gboolean quocker_compose_schema_validate(const char *schema_path,
                                         JsonNode *instance, char **error);

#endif
