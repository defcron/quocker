#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static gboolean run_cli(const char *cli, const char *directory,
                        const char *compose_file, const char *command,
                        gchar **stdout_text) {
  char *arguments[] = {(char *)cli,          (char *)"--project-directory",
                       (char *)directory,    (char *)"--project-name",
                       (char *)"lifecycle",  (char *)"-f",
                       (char *)compose_file, (char *)command,
                       (char *)"app",        NULL};
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
  char *state = g_strdup_printf("%d\nlifecycle-app\napp.qcow2\n", (int)child);
  g_assert_true(g_file_set_contents(state_path, state, -1, &error));
  g_assert_no_error(error);
  char *pid_contents = g_strdup_printf("%d\n", (int)child);
  g_assert_true(g_file_set_contents(pidfile, pid_contents, -1, &error));
  g_assert_no_error(error);
  g_free(pid_contents);
  g_free(state);

  gchar *output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "stop", &output));
  g_assert_nonnull(strstr(output, "app: stopped"));
  g_free(output);
  int child_status = 0;
  g_assert_cmpint(waitpid(child, &child_status, 0), ==, child);
  g_assert_true(WIFEXITED(child_status));
  g_assert_true(g_file_test(state_path, G_FILE_TEST_IS_REGULAR));
  g_assert_false(g_file_test(pidfile, G_FILE_TEST_EXISTS));
  g_spawn_close_pid(child);

  output = NULL;
  g_assert_true(run_cli(cli, directory, compose, "start", &output));
  g_assert_nonnull(strstr(output, "app: started"));
  g_free(output);
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
  g_assert_true(run_cli(cli, directory, compose, "ps", &output));
  g_assert_nonnull(strstr(output, "lifecycle-app\tstopped"));
  g_free(output);
  output = NULL;
  g_assert_false(run_cli(cli, directory, compose, "stop", &output));
  g_free(output);
  g_assert_true(g_file_test(qmp_socket, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_set_contents(state_path, started_state, -1, &error));
  g_assert_no_error(error);
  g_strfreev(state_lines);
  g_free(started_state);
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
  g_assert_true(run_cli(cli, directory, compose, "down", &output));
  g_free(output);
  g_assert_false(g_file_test(state_path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(pidfile, G_FILE_TEST_EXISTS));

  char *volumes_directory = g_build_filename(state_directory, "volumes", NULL);
  char *volume_disk =
      g_build_filename(volumes_directory, "persistent.ext4", NULL);
  g_assert_cmpint(g_mkdir(volumes_directory, 0700), ==, 0);
  g_assert_true(g_file_set_contents(volume_disk, "disk", -1, &error));
  g_assert_no_error(error);
  g_assert_true(remove_cli_volumes(cli, directory, compose));
  g_assert_false(g_file_test(volume_disk, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(volumes_directory, G_FILE_TEST_EXISTS));

  g_assert_false(g_file_test(overlay_sidecar, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(overlay_disk, G_FILE_TEST_EXISTS));
  g_assert_cmpint(g_unlink(base_disk), ==, 0);
  g_assert_cmpint(g_unlink(compose), ==, 0);
  g_assert_cmpint(g_rmdir(state_directory), ==, 0);
  char *quocker_directory = g_build_filename(directory, ".quocker", NULL);
  g_assert_cmpint(g_rmdir(quocker_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(quocker_directory);
  g_free(volume_disk);
  g_free(volumes_directory);
  g_free(state_path);
  g_free(pidfile);
  g_free(qmp_socket);
  g_free(overlay_sidecar);
  g_free(overlay_disk);
  g_free(base_disk);
  g_free(state_directory);
  g_free(compose);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/lifecycle/stop-preserves-state",
                  test_stop_preserves_vm_state);
  return g_test_run();
}
