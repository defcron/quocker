/*
 * Bounded retry policy for OCI registry HTTP requests.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-registry-http.h"

#include <errno.h>

#define MAX_RETRY_DELAY_SECONDS 30

gboolean quocker_registry_http_status_retryable(long status) {
  switch (status) {
  case 408:
  case 425:
  case 429:
  case 500:
  case 502:
  case 503:
  case 504:
    return TRUE;
  default:
    return FALSE;
  }
}

gboolean quocker_registry_curl_error_retryable(CURLcode code) {
  switch (code) {
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_CONNECT:
  case CURLE_OPERATION_TIMEDOUT:
  case CURLE_SEND_ERROR:
  case CURLE_RECV_ERROR:
  case CURLE_GOT_NOTHING:
  case CURLE_PARTIAL_FILE:
    return TRUE;
  default:
    return FALSE;
  }
}

guint quocker_registry_retry_delay_seconds(const char *retry_after,
                                           guint retry_number,
                                           time_t now) {
  if (retry_after && *retry_after) {
    char *end = NULL;
    errno = 0;
    guint64 seconds = g_ascii_strtoull(retry_after, &end, 10);
    if (end != retry_after && end && *end == '\0' && errno != ERANGE) {
      return MIN(seconds, MAX_RETRY_DELAY_SECONDS);
    }
    time_t date = curl_getdate(retry_after, NULL);
    if (date >= 0) {
      if (date <= now) {
        return 0;
      }
      return MIN((guint64)(date - now), MAX_RETRY_DELAY_SECONDS);
    }
  }

  guint shift = MIN(retry_number, 5);
  guint delay = 1U << shift;
  return MIN(delay, MAX_RETRY_DELAY_SECONDS);
}
