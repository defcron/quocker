/*
 * Bounded retry policy for OCI registry HTTP requests.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QUOCKER_REGISTRY_HTTP_H
#define QUOCKER_REGISTRY_HTTP_H

#include <curl/curl.h>
#include <glib.h>

#include <time.h>

gboolean quocker_registry_http_status_retryable(long status);
gboolean quocker_registry_curl_error_retryable(CURLcode code);
guint quocker_registry_retry_delay_seconds(const char *retry_after,
                                           guint retry_number,
                                           time_t now);

#endif
