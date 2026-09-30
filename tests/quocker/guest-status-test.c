#include "../../scripts/quocker-guest-status.h"

#include <glib.h>
#include <string.h>

static void test_ready_marker_requires_protocol_line(void) {
  const char *valid = "booting\nQUOCKER_READY pid=42\nother output\n";
  g_assert_true(quocker_guest_status_has_ready(valid, strlen(valid)));

  const char *embedded = "workload says QUOCKER_READY pid=42\n";
  g_assert_false(quocker_guest_status_has_ready(embedded, strlen(embedded)));
  const char *invalid = "QUOCKER_READY pid=0\n";
  g_assert_false(quocker_guest_status_has_ready(invalid, strlen(invalid)));
  const char *signed_pid = "QUOCKER_READY pid=+42\n";
  g_assert_false(
      quocker_guest_status_has_ready(signed_pid, strlen(signed_pid)));
  g_assert_false(quocker_guest_status_has_ready(NULL, 0));
}

static void test_exit_status_parsing(void) {
  GError *error = NULL;
  int status = -1;
  const char *success = "QUOCKER_EXIT status=0\r\n";
  g_assert_true(quocker_guest_status_parse_exit(success, strlen(success),
                                                &status, &error));
  g_assert_no_error(error);
  g_assert_cmpint(status, ==, 0);

  const char *failure = "QUOCKER_EXIT status=0\nQUOCKER_EXIT status=17\n";
  g_assert_true(quocker_guest_status_parse_exit(failure, strlen(failure),
                                                &status, &error));
  g_assert_no_error(error);
  g_assert_cmpint(status, ==, 17);

  const char *invalid = "QUOCKER_EXIT status=256\n";
  g_assert_false(quocker_guest_status_parse_exit(invalid, strlen(invalid),
                                                 &status, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-guest-status-error"),
                 3);
  g_clear_error(&error);

  const char *signed_status = "QUOCKER_EXIT status=+1\n";
  g_assert_false(quocker_guest_status_parse_exit(
      signed_status, strlen(signed_status), &status, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-guest-status-error"),
                 3);
  g_clear_error(&error);

  const char *signaled = "QUOCKER_EXIT signal=9\n";
  g_assert_false(quocker_guest_status_parse_exit(signaled, strlen(signaled),
                                                 &status, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-guest-status-error"),
                 2);
  g_clear_error(&error);
  g_assert_false(quocker_guest_status_parse_exit(NULL, 0, &status, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-guest-status-error"),
                 1);
  g_clear_error(&error);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/guest-status/ready",
                  test_ready_marker_requires_protocol_line);
  g_test_add_func("/quocker/guest-status/exit",
                  test_exit_status_parsing);
  return g_test_run();
}
