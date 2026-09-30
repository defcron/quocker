#include <arpa/inet.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static gboolean run_cli_for_project(const char *cli, const char *directory,
                                    const char *compose_file,
                                    const char *project,
                                    const char *service, const char *command,
                                    const char *extra_arg,
                                    gchar **stdout_text) {
  char *arguments[] = {(char *)cli,          (char *)"--project-directory",
                       (char *)directory,    (char *)"--project-name",
                       (char *)project,      (char *)"-f",
                       (char *)compose_file, (char *)command,
                       (char *)service,      (char *)extra_arg, NULL};
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  gboolean spawned =
      g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                   stdout_text, &stderr_text, &status, &error);
  if (!spawned) {
    g_error("could not execute quocker: %s", error->message);
  }
  gboolean ok = g_spawn_check_wait_status(status, &error);
  if (!ok && error) {
    g_test_message("quocker command failed: %s; stderr: %s", error->message,
                   stderr_text ? stderr_text : "");
  }
  g_clear_error(&error);
  g_free(stderr_text);
  return ok;
}

static gboolean run_cli(const char *cli, const char *directory,
                        const char *compose_file, const char *command,
                        gchar **stdout_text) {
  return run_cli_for_project(cli, directory, compose_file, "lifecycle", "app",
                             command, NULL, stdout_text);
}

static void assert_stop_timeout_short_option(const char *cli,
                                             const char *directory,
                                             const char *compose_file) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"lifecycle",
                       (char *)"-f",
                       (char *)compose_file,
                       (char *)"stop",
                       (char *)"--dry-run",
                       (char *)"-t",
                       (char *)"0",
                       (char *)"app",
                       NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &stdout_text, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(stdout_text, ==, "Would stop service app\n");
  g_assert_cmpstr(stderr_text, ==, "");
  g_free(stdout_text);
  g_free(stderr_text);
}

static gboolean remove_cli_volumes(const char *cli, const char *directory,
                                   const char *compose_file) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"lifecycle",
                       (char *)"-f",
                       (char *)compose_file,
                       (char *)"down",
                       (char *)"--volumes",
                       (char *)"app",
                       NULL};
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  gboolean spawned =
      g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                   &stdout_text, &stderr_text, &status, &error);
  if (!spawned) {
    g_error("could not execute quocker: %s", error->message);
  }
  gboolean ok = g_spawn_check_wait_status(status, &error);
  if (!ok && error) {
    g_test_message("quocker down --volumes failed: %s; stderr: %s",
                   error->message, stderr_text ? stderr_text : "");
  }
  g_clear_error(&error);
  g_free(stdout_text);
  g_free(stderr_text);
  return ok;
}

static void assert_dynamic_port(const char *cli, const char *directory,
                                const char *compose_file) {
  char *arguments[] = {(char *)cli,
                       (char *)"--project-directory",
                       (char *)directory,
                       (char *)"--project-name",
                       (char *)"lifecycle",
                       (char *)"-f",
                       (char *)compose_file,
                       (char *)"port",
                       (char *)"app",
                       (char *)"80",
                       NULL};
  gchar *output = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &output, &stderr_text, &status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(output, ==, "127.0.0.1:41234\n");
  g_free(output);
  g_free(stderr_text);
}

static void test_stop_preserves_vm_state(void) {
  const char *cli = g_getenv("QUOCKER_CLI");
  const char *fake_qemu = g_getenv("QUOCKER_FAKE_QEMU");
  const char *fake_qemu_img = g_getenv("QUOCKER_FAKE_QEMU_IMG");
  g_assert_nonnull(cli);
  g_assert_nonnull(fake_qemu);
  g_assert_nonnull(fake_qemu_img);
  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-lifecycle-XXXXXX", &error);
  g_assert_no_error(error);
  char *compose = g_build_filename(directory, "compose.yaml", NULL);
  g_assert_true(
      g_file_set_contents(compose,
                          "services:\n  app:\n    image: ./disk.qcow2\n    "
                          "ports:\n      - \"127.0.0.1::80\"\n",
                          -1, &error));
  g_assert_no_error(error);
  char *state_directory =
      g_build_filename(directory, ".quocker", "lifecycle", NULL);
  g_assert_cmpint(g_mkdir_with_parents(state_directory, 0700), ==, 0);
  g_setenv("QUOCKER_QEMU", fake_qemu, TRUE);
  g_setenv("QUOCKER_QEMU_IMG", fake_qemu_img, TRUE);
  char *base_disk = g_build_filename(directory, "disk.qcow2", NULL);
  char *overlay_disk = g_build_filename(state_directory, "app.qcow2", NULL);
  char *overlay_sidecar = g_strconcat(overlay_disk, ".base", NULL);
  g_assert_true(g_file_set_contents(base_disk, "base", -1, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_set_contents(overlay_disk, "overlay", -1, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_set_contents(overlay_sidecar, base_disk, -1, &error));
  g_assert_no_error(error);

  char *fake_argv[] = {(char *)fake_qemu, (char *)"-name",
                       (char *)"lifecycle-app", NULL};
  GPid child = 0;
  g_assert_true(g_spawn_async(NULL, fake_argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                              NULL, NULL, &child, &error));
  g_assert_no_error(error);
  g_usleep(50000);
  char *state_path = g_build_filename(state_directory, "app.state", NULL);
  char *pidfile = g_build_filename(state_directory, "app.pid", NULL);
  char *qmp_socket = g_build_filename(state_directory, "app.qmp", NULL);
  char *stale_state =
      g_build_filename(state_directory, ".app.state.tmp.crash01", NULL);
  g_assert_true(g_file_set_contents(stale_state, "partial", -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(stale_state, 0600), ==, 0);
  char *state = g_strdup_printf("%d\nlifecycle-app\napp.qcow2\n", (int)child);
  g_assert_true(g_file_set_contents(state_path, state, -1, &error));
  g_assert_no_error(error);
  char *pid_contents = g_strdup_printf("%d\n", (int)child);
  g_assert_true(g_file_set_contents(pidfile, pid_contents, -1, &error));
  g_assert_no_error(error);
  g_free(pid_contents);
  g_free(state);

  gchar *output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "rm", &output));
  g_free(output);
  output = NULL;
  g_assert_true(g_file_test(state_path, G_FILE_TEST_IS_REGULAR));
  g_assert_false(run_cli_for_project(cli, directory, compose, "lifecycle",
                                     "app", "rm", "--volumes", &output));
  g_free(output);
  output = NULL;
  g_assert_true(g_file_test(state_path, G_FILE_TEST_IS_REGULAR));
  g_assert_true(run_cli(cli, directory, compose, "stop", &output));
  g_assert_nonnull(strstr(output, "app: stopped"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "ps", &output));
  g_assert_cmpstr(output, ==, "NAME\tSTATE\tPID\tDISK\n");
  g_free(output);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "lifecycle",
                                    "app", "ps", "--all", &output));
  g_assert_nonnull(strstr(output, "lifecycle-app\tstopped"));
  g_free(output);
  g_assert_false(g_file_test(stale_state, G_FILE_TEST_EXISTS));
  int child_status = 0;
  g_assert_cmpint(waitpid(child, &child_status, 0), ==, child);
  g_assert_true(WIFEXITED(child_status));
  g_assert_true(g_file_test(state_path, G_FILE_TEST_IS_REGULAR));
  g_assert_false(g_file_test(pidfile, G_FILE_TEST_EXISTS));
  g_spawn_close_pid(child);

  char *saved_state_path = g_build_filename(state_directory, "app.state.saved",
                                             NULL);
  char *external_state = g_build_filename(directory, "external.state", NULL);
  g_assert_cmpint(g_rename(state_path, saved_state_path), ==, 0);
  g_assert_true(g_file_set_contents(external_state, "123\n", -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(symlink(external_state, state_path), ==, 0);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  g_assert_cmpint(g_unlink(state_path), ==, 0);
  g_assert_cmpint(g_rename(saved_state_path, state_path), ==, 0);
  g_unlink(external_state);
  g_free(saved_state_path);
  g_free(external_state);

  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "start", &output));
  g_assert_nonnull(strstr(output, "app: started"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "ps", &output));
  g_assert_nonnull(strstr(output, "lifecycle-app\trunning"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "lifecycle",
                                    "-q", "ps", "app", &output));
  g_assert_cmpstr(output, ==, "lifecycle-app\n");
  g_free(output);
  struct stat state_stat;
  g_assert_cmpint(g_stat(state_path, &state_stat), ==, 0);
  g_assert_cmpint(state_stat.st_mode & 0777, ==, 0600);
  assert_dynamic_port(cli, directory, compose);
  gchar *started_state = NULL;
  g_assert_true(g_file_get_contents(state_path, &started_state, NULL, &error));
  g_assert_no_error(error);
  gchar **state_lines = g_strsplit(started_state, "\n", 0);
  g_assert_nonnull(state_lines[3]);
  g_assert_cmpuint(g_ascii_strtoull(state_lines[3], NULL, 10), >, 0);
  char *mismatched_state = g_strdup_printf("%s\n%s\n%s\n1\n", state_lines[0],
                                           state_lines[1], state_lines[2]);
  g_assert_true(g_file_set_contents(state_path, mismatched_state, -1, &error));
  g_assert_no_error(error);
  g_free(mismatched_state);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "lifecycle",
                                    "app", "ps", "--all", &output));
  g_assert_nonnull(strstr(output, "lifecycle-app\tstopped"));
  g_free(output);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "stop", &output));
  g_free(output);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "wait", &output));
  g_free(output);
  g_assert_true(g_file_test(qmp_socket, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_set_contents(state_path, started_state, -1, &error));
  g_assert_no_error(error);
  pid_t crashed_pid = (pid_t)g_ascii_strtoll(state_lines[0], NULL, 10);
  g_assert_cmpint(kill(crashed_pid, SIGKILL), ==, 0);
  g_usleep(100000);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "lifecycle",
                                    "app", "ps", "--all", &output));
  g_assert_nonnull(strstr(output, "lifecycle-app\tstopped"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "start", &output));
  g_assert_nonnull(strstr(output, "app: started"));
  g_free(output);
  g_strfreev(state_lines);
  g_free(started_state);
  char *wait_arguments[] = {(char *)cli,
                            (char *)"--project-directory",
                            directory,
                            (char *)"--project-name",
                            (char *)"lifecycle",
                            (char *)"-f",
                            compose,
                            (char *)"wait",
                            (char *)"app",
                            NULL};
  GPid waiter = 0;
  g_assert_true(g_spawn_async(NULL, wait_arguments, NULL,
                              G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &waiter,
                              &error));
  g_assert_no_error(error);
  g_usleep(100000);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "stop", &output));
  g_free(output);
  gint waiter_status = 0;
  pid_t waiter_result = 0;
  for (guint i = 0; i < 30 && waiter_result == 0; i++) {
    waiter_result = waitpid(waiter, &waiter_status, WNOHANG);
    if (waiter_result == 0) {
      g_usleep(100000);
    }
  }
  if (waiter_result == 0) {
    kill(waiter, SIGKILL);
    waitpid(waiter, &waiter_status, 0);
  }
  g_assert_cmpint(waiter_result, ==, waiter);
  g_assert_true(WIFEXITED(waiter_status));
  g_assert_cmpint(WEXITSTATUS(waiter_status), ==, 0);
  g_spawn_close_pid(waiter);
  char *guest_log = g_build_filename(state_directory, "app.log", NULL);
  g_assert_true(g_file_set_contents(guest_log,
                                    "QUOCKER_EXIT status=23\n", -1, &error));
  g_assert_no_error(error);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "wait", &output));
  g_assert_cmpstr(output, ==, "app: exit code 23\n");
  g_free(output);
  g_assert_true(g_file_set_contents(guest_log, "QUOCKER_EXIT signal=9\n", -1,
                                    &error));
  g_assert_no_error(error);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "wait", &output));
  g_assert_cmpstr(output, ==, "app: exit code 137\n");
  g_free(output);
  g_free(guest_log);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "start", &output));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "pause", &output));
  g_assert_nonnull(strstr(output, "app: paused"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "unpause", &output));
  g_assert_nonnull(strstr(output, "app: unpaused"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "kill", &output));
  g_assert_nonnull(strstr(output, "sent Killed"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "start", &output));
  g_assert_nonnull(strstr(output, "app: started"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "stop", &output));
  g_assert_nonnull(strstr(output, "app: stopped"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "rm", &output));
  g_free(output);
  g_assert_false(g_file_test(state_path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(pidfile, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(overlay_disk, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(overlay_sidecar, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(base_disk, G_FILE_TEST_IS_REGULAR));

  char *fake_argv_path = g_build_filename(directory, "fake-qemu-argv", NULL);
  g_setenv("QUOCKER_FAKE_QEMU_ARGV_FILE", fake_argv_path, TRUE);
  g_assert_true(g_file_set_contents(
      compose,
      "services:\n  app:\n    image: ./disk.qcow2\n    "
      "network_mode: host\n",
      -1, &error));
  g_assert_no_error(error);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  g_assert_true(g_file_set_contents(
      compose,
      "services:\n  app:\n    image: ./disk.qcow2\n    "
      "network_mode: none\n    ports: [\"80:80\"]\n",
      -1, &error));
  g_assert_no_error(error);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  g_assert_true(g_file_set_contents(
      compose,
      "services:\n  app:\n    image: ./disk.qcow2\n    network_mode: none\n",
      -1, &error));
  g_assert_no_error(error);
  g_setenv("QUOCKER_FAKE_QEMU_IGNORE_TERM", "1", TRUE);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "up", &output));
  g_assert_nonnull(strstr(output, "app: started"));
  g_free(output);
  gchar *fake_argv_contents = NULL;
  g_assert_true(g_file_get_contents(fake_argv_path, &fake_argv_contents, NULL,
                                    &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(fake_argv_contents, "\n-nic\nnone\n"));
  g_free(fake_argv_contents);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "lifecycle",
                                    "app", "stop", "--timeout=0", &output));
  g_assert_nonnull(strstr(output, "app: stopped"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  char *rm_volumes_directory =
      g_build_filename(state_directory, "volumes", NULL);
  char *named_volume_disk =
      g_build_filename(rm_volumes_directory, "named.ext4", NULL);
  g_assert_cmpint(g_mkdir(rm_volumes_directory, 0700), ==, 0);
  g_assert_true(g_file_set_contents(named_volume_disk, "keep", -1, &error));
  g_assert_no_error(error);
  g_unsetenv("QUOCKER_FAKE_QEMU_IGNORE_TERM");
  output = NULL;
  char *rm_stop_arguments[] = {
      (char *)cli, (char *)"--project-directory", directory,
      (char *)"--project-name", (char *)"lifecycle", (char *)"-f",
      compose, (char *)"rm", (char *)"--stop", (char *)"--timeout=0",
      (char *)"app", NULL};
  gchar *rm_stderr = NULL;
  gint rm_status = 0;
  GError *rm_error = NULL;
  g_assert_true(g_spawn_sync(NULL, rm_stop_arguments, NULL, G_SPAWN_DEFAULT,
                             NULL, NULL, &output, &rm_stderr, &rm_status,
                             &rm_error));
  g_assert_no_error(rm_error);
  g_assert_true(g_spawn_check_wait_status(rm_status, &rm_error));
  g_assert_no_error(rm_error);
  g_assert_nonnull(strstr(output, "app: stopped"));
  g_assert_false(g_file_test(state_path, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(named_volume_disk, G_FILE_TEST_IS_REGULAR));
  g_free(output);
  g_free(rm_stderr);
  g_assert_cmpint(g_unlink(named_volume_disk), ==, 0);
  g_assert_cmpint(g_rmdir(rm_volumes_directory), ==, 0);
  g_free(named_volume_disk);
  g_free(rm_volumes_directory);
  assert_stop_timeout_short_option(cli, directory, compose);
  g_unsetenv("QUOCKER_FAKE_QEMU_ARGV_FILE");

  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "wait-ready",
                                    "app", "up", "--wait", &output));
  g_assert_nonnull(strstr(output, "[wait-ready] app: ready"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose, "wait-ready",
                                    "app", "down", NULL, &output));
  g_free(output);
  char *wait_ready_directory =
      g_build_filename(directory, ".quocker", "wait-ready", NULL);
  const char *wait_ready_files[] = {"app.log", "app.pid", "app.state",
                                    "app.qcow2", "app.qcow2.base",
                                    ".lifecycle.lock"};
  for (guint i = 0; i < G_N_ELEMENTS(wait_ready_files); i++) {
    char *path = g_build_filename(wait_ready_directory, wait_ready_files[i],
                                  NULL);
    g_unlink(path);
    g_free(path);
  }
  g_assert_cmpint(g_rmdir(wait_ready_directory), ==, 0);
  g_free(wait_ready_directory);

  const char *completed_compose =
      "services:\n"
      "  app:\n"
      "    image: ./disk.qcow2\n"
      "    depends_on:\n"
      "      job:\n"
      "        condition: service_completed_successfully\n"
      "  job:\n"
      "    image: ./disk.qcow2\n";
  g_assert_true(g_file_set_contents(compose, completed_compose, -1, &error));
  g_assert_no_error(error);
  g_setenv("QUOCKER_FAKE_QEMU_EXIT_SERVICE", "job", TRUE);
  g_setenv("QUOCKER_FAKE_QEMU_EXIT_STATUS", "9", TRUE);
  output = NULL;
  g_assert_false(run_cli_for_project(cli, directory, compose,
                                    "completed-failure", "app", "up",
                                    NULL,
                                    &output));
  g_assert_nonnull(strstr(output, "[completed-failure] job: started"));
  g_assert_null(strstr(output, "[completed-failure] app: started"));
  g_free(output);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose,
                                    "completed-failure", "app", "down",
                                    NULL,
                                    &output));
  g_free(output);

  g_setenv("QUOCKER_FAKE_QEMU_EXIT_STATUS", "0", TRUE);
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose,
                                    "completed-success", "app", "up",
                                    NULL,
                                    &output));
  g_assert_nonnull(strstr(output, "[completed-success] job: started"));
  g_assert_nonnull(strstr(output, "[completed-success] app: started"));
  g_free(output);
  g_unsetenv("QUOCKER_FAKE_QEMU_EXIT_SERVICE");
  g_unsetenv("QUOCKER_FAKE_QEMU_EXIT_STATUS");
  output = NULL;
  g_assert_true(run_cli_for_project(cli, directory, compose,
                                    "completed-success", "app", "down",
                                    NULL,
                                    &output));
  g_free(output);
  const char *completed_projects[] = {"completed-failure", "completed-success"};
  const char *completed_files[] = {
      "app.log", "job.log", "app.qcow2", "app.qcow2.base", "job.qcow2",
      "job.qcow2.base", ".lifecycle.lock"};
  for (guint i = 0; i < G_N_ELEMENTS(completed_projects); i++) {
    char *project_directory =
        g_build_filename(directory, ".quocker", completed_projects[i], NULL);
    for (guint j = 0; j < G_N_ELEMENTS(completed_files); j++) {
      char *path = g_build_filename(project_directory, completed_files[j],
                                    NULL);
      g_unlink(path);
      g_free(path);
    }
    g_assert_cmpint(g_rmdir(project_directory), ==, 0);
    g_free(project_directory);
  }

  char *volumes_directory = g_build_filename(state_directory, "volumes", NULL);
  char *volume_disk =
      g_build_filename(volumes_directory, "persistent.ext4", NULL);
  g_assert_cmpint(g_mkdir(volumes_directory, 0700), ==, 0);
  int volume_fd = open(volume_disk, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  g_assert_cmpint(volume_fd, >=, 0);
  g_assert_cmpint(ftruncate(volume_fd, 64 * 1024 * 1024), ==, 0);
  g_assert_cmpint(close(volume_fd), ==, 0);
  const char *mkfs_arguments[] = {"mke2fs", "-q", "-t",        "ext4", "-F",
                                  "-m",     "0",  volume_disk, NULL};
  gint mkfs_status = 0;
  g_assert_true(g_spawn_sync(NULL, (char **)mkfs_arguments, NULL,
                             G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                             &mkfs_status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(mkfs_status, &error));
  g_assert_no_error(error);
  g_assert_true(remove_cli_volumes(cli, directory, compose));
  g_assert_false(g_file_test(volume_disk, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(volumes_directory, G_FILE_TEST_IS_DIR));
  char *volume_lock =
      g_build_filename(volumes_directory, ".quocker.lock", NULL);
  g_assert_cmpint(g_unlink(volume_lock), ==, 0);
  g_assert_cmpint(g_rmdir(volumes_directory), ==, 0);

  g_assert_false(g_file_test(overlay_sidecar, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(overlay_disk, G_FILE_TEST_EXISTS));
  char *app_log = g_build_filename(state_directory, "app.log", NULL);
  g_assert_cmpint(g_unlink(app_log), ==, 0);
  g_free(app_log);
  g_assert_cmpint(g_unlink(base_disk), ==, 0);
  g_assert_cmpint(g_unlink(fake_argv_path), ==, 0);
  g_assert_cmpint(g_unlink(compose), ==, 0);
  char *lifecycle_lock =
      g_build_filename(state_directory, ".lifecycle.lock", NULL);
  g_assert_cmpint(g_unlink(lifecycle_lock), ==, 0);
  g_assert_cmpint(g_rmdir(state_directory), ==, 0);
  char *quocker_directory = g_build_filename(directory, ".quocker", NULL);
  g_assert_cmpint(g_rmdir(quocker_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(lifecycle_lock);
  g_free(fake_argv_path);
  g_free(quocker_directory);
  g_free(volume_lock);
  g_free(volume_disk);
  g_free(volumes_directory);
  g_free(state_path);
  g_free(pidfile);
  g_free(qmp_socket);
  g_free(stale_state);
  g_free(overlay_sidecar);
  g_free(overlay_disk);
  g_free(base_disk);
  g_free(state_directory);
  g_free(compose);
  g_free(directory);
}

static void test_up_rejects_host_port_used_by_other_process(void) {
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(fd, >=, 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                                .sin_port = 0};
  g_assert_cmpint(bind(fd, (struct sockaddr *)&address, sizeof(address)), ==,
                  0);
  socklen_t address_length = sizeof(address);
  g_assert_cmpint(getsockname(fd, (struct sockaddr *)&address, &address_length),
                  ==, 0);

  GError *error = NULL;
  char *directory = g_dir_make_tmp("quocker-port-collision-XXXXXX", &error);
  g_assert_no_error(error);
  char *compose = g_build_filename(directory, "compose.yaml", NULL);
  char *contents = g_strdup_printf(
      "services:\n  app:\n    image: ./disk.qcow2\n    ports:\n"
      "      - \"127.0.0.1:%u:80\"\n",
      ntohs(address.sin_port));
  g_assert_true(g_file_set_contents(compose, contents, -1, &error));
  g_assert_no_error(error);
  g_free(contents);
  gchar *output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  close(fd);

  int udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(udp_fd, >=, 0);
  address.sin_port = 0;
  g_assert_cmpint(bind(udp_fd, (struct sockaddr *)&address, sizeof(address)),
                  ==, 0);
  address_length = sizeof(address);
  g_assert_cmpint(getsockname(udp_fd, (struct sockaddr *)&address,
                              &address_length),
                  ==, 0);
  contents = g_strdup_printf(
      "services:\n  app:\n    image: ./disk.qcow2\n    ports:\n"
      "      - \"127.0.0.1:%u:53/udp\"\n",
      ntohs(address.sin_port));
  g_assert_true(g_file_set_contents(compose, contents, -1, &error));
  g_assert_no_error(error);
  g_free(contents);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "up", &output));
  g_free(output);
  close(udp_fd);

  g_unlink(compose);
  g_rmdir(directory);
  g_free(compose);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/lifecycle/stop-preserves-state",
                  test_stop_preserves_vm_state);
  g_test_add_func("/quocker/lifecycle/host-port-collision",
                  test_up_rejects_host_port_used_by_other_process);
  return g_test_run();
}
