#include "../../scripts/quocker-registry-http.h"

#include <glib.h>

static void test_retryable_statuses(void) {
  const long retryable[] = {408, 425, 429, 500, 502, 503, 504};
  for (guint i = 0; i < G_N_ELEMENTS(retryable); i++) {
    g_assert_true(quocker_registry_http_status_retryable(retryable[i]));
  }
  g_assert_false(quocker_registry_http_status_retryable(200));
  g_assert_false(quocker_registry_http_status_retryable(401));
  g_assert_false(quocker_registry_http_status_retryable(404));
}

static void test_transient_transport_errors(void) {
  g_assert_true(quocker_registry_curl_error_retryable(CURLE_COULDNT_CONNECT));
  g_assert_true(quocker_registry_curl_error_retryable(CURLE_OPERATION_TIMEDOUT));
  g_assert_true(quocker_registry_curl_error_retryable(CURLE_PARTIAL_FILE));
  g_assert_false(quocker_registry_curl_error_retryable(CURLE_PEER_FAILED_VERIFICATION));
  g_assert_false(quocker_registry_curl_error_retryable(CURLE_URL_MALFORMAT));
}

static void test_bounded_backoff(void) {
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(NULL, 0, 0), ==, 1);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(NULL, 1, 0), ==, 2);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(NULL, 3, 0), ==, 8);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(NULL, 10, 0), ==, 30);
}

static void test_retry_after_delta_and_date(void) {
  g_assert_cmpuint(quocker_registry_retry_delay_seconds("12", 0, 0), ==, 12);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds("900", 0, 0), ==, 30);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(
                       "Thu, 01 Jan 1970 00:00:10 GMT", 0, 0), ==, 10);
  g_assert_cmpuint(quocker_registry_retry_delay_seconds(
                       "Thu, 01 Jan 1970 00:00:10 GMT", 0, 20), ==, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/registry-http/status", test_retryable_statuses);
  g_test_add_func("/quocker/registry-http/transport",
                  test_transient_transport_errors);
  g_test_add_func("/quocker/registry-http/backoff", test_bounded_backoff);
  g_test_add_func("/quocker/registry-http/retry-after",
                  test_retry_after_delta_and_date);
  return g_test_run();
}
