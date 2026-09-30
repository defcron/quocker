#include "../../scripts/quocker-volume.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int create_volume_child(const char *directory, const char *name) {
  char *path = NULL;
  GError *error = NULL;
  gboolean created = quocker_volume_disk_prepare(
      directory, name, 64 * 1024 * 1024, &path, &error);
  g_free(path);
  if (created) {
    g_clear_error(&error);
    return 0;
  }
  gboolean quota = error && strstr(error->message, "quota exceeded");
  g_clear_error(&error);
  return quota ? 10 : 11;
}

static gchar *run_volume_command(const char *cli, const char *project,
                                 const char *compose, const char *action,
                                 const char *name, gboolean dry_run,
                                 gboolean expect_success) {
  char *arguments[] = {(char *)cli,       (char *)"--project-directory",
                       (char *)project,   (char *)"--project-name",
                       (char *)"project", (char *)"-f",
                       (char *)compose,   (char *)"volume",
                       (char *)action,    dry_run ? (char *)"--dry-run" : NULL,
                       (char *)name,      NULL};
  if (!dry_run) {
    arguments[9] = arguments[10];
    arguments[10] = NULL;
  }
  gchar *stdout_text = NULL;
  gchar *stderr_text = NULL;
  gint status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &stdout_text, &stderr_text, &status, &error));
  g_assert_no_error(error);
  gboolean succeeded = g_spawn_check_wait_status(status, &error);
  g_assert_cmpint(succeeded, ==, expect_success);
  g_clear_error(&error);
  g_free(stderr_text);
  return stdout_text;
}

int main(void) {
  GError *error = NULL;
  g_setenv("QUOCKER_VOLUME_QUOTA", "128MiB", TRUE);
  char *directory = g_dir_make_tmp("quocker-volume-test-XXXXXX", &error);
  g_assert_no_error(error);
  char *first = NULL;
  g_assert_true(quocker_volume_disk_prepare(directory, "project:data",
                                            64 * 1024 * 1024, &first, &error));
  g_assert_no_error(error);
  struct stat st;
  g_assert_cmpint(g_lstat(first, &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));
  g_assert_cmpint(st.st_size, ==, 64 * 1024 * 1024);
  GPtrArray *volumes = NULL;
  g_assert_true(quocker_volume_list(directory, &volumes, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(volumes->len, ==, 1);
  QuockerVolumeInfo *info = g_ptr_array_index(volumes, 0);
  g_assert_cmpstr(info->logical_name, ==, "project:data");
  g_assert_cmpuint(info->size_bytes, ==, 64 * 1024 * 1024);
  g_ptr_array_free(volumes, TRUE);
  guint64 virtual_bytes = 0;
  guint64 allocated_bytes = 0;
  guint64 quota_bytes = 0;
  g_assert_true(quocker_volume_project_usage(
      directory, &virtual_bytes, &allocated_bytes, &quota_bytes, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(virtual_bytes, ==, 64 * 1024 * 1024);
  g_assert_cmpuint(allocated_bytes, >, 0);
  g_assert_cmpuint(quota_bytes, ==, 128 * 1024 * 1024);
  char *over_quota = NULL;
  g_assert_false(quocker_volume_disk_prepare(
      directory, "project:extra", 128 * 1024 * 1024, &over_quota, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_assert_nonnull(strstr(error->message, "quota exceeded"));
  g_clear_error(&error);
  g_setenv("QUOCKER_VOLUME_QUOTA", " -1GiB", TRUE);
  g_assert_false(quocker_volume_project_usage(
      directory, &virtual_bytes, &allocated_bytes, &quota_bytes, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_clear_error(&error);
  g_setenv("QUOCKER_VOLUME_QUOTA", "128MiB", TRUE);

  pid_t children[2];
  const char *parallel_names[] = {"project:parallel-a", "project:parallel-b"};
  for (guint i = 0; i < G_N_ELEMENTS(children); i++) {
    children[i] = fork();
    g_assert_cmpint(children[i], >=, 0);
    if (children[i] == 0) {
      _exit(create_volume_child(directory, parallel_names[i]));
    }
  }
  guint successes = 0;
  guint quota_rejections = 0;
  for (guint i = 0; i < G_N_ELEMENTS(children); i++) {
    int child_status = 0;
    g_assert_cmpint(waitpid(children[i], &child_status, 0), ==, children[i]);
    g_assert_true(WIFEXITED(child_status));
    if (WEXITSTATUS(child_status) == 0) {
      successes++;
    } else if (WEXITSTATUS(child_status) == 10) {
      quota_rejections++;
    } else {
      g_assert_not_reached();
    }
  }
  g_assert_cmpuint(successes, ==, 1);
  g_assert_cmpuint(quota_rejections, ==, 1);
  g_assert_true(quocker_volume_project_usage(
      directory, &virtual_bytes, &allocated_bytes, &quota_bytes, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(virtual_bytes, ==, 128 * 1024 * 1024);
  char *second = NULL;
  g_assert_true(quocker_volume_disk_prepare(
      directory, "project:data", 128 * 1024 * 1024, &second, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(first, ==, second);
  g_assert_cmpint(g_lstat(second, &st), ==, 0);
  g_assert_cmpint(st.st_size, ==, 64 * 1024 * 1024);

#ifdef F_OFD_SETLK
  int in_use_fd = open(first, O_RDONLY | O_CLOEXEC);
  g_assert_cmpint(in_use_fd, >=, 0);
  struct flock qemu_permission_lock = {
      .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 1};
  g_assert_cmpint(fcntl(in_use_fd, F_OFD_SETLK, &qemu_permission_lock), ==, 0);
  g_assert_false(quocker_volume_remove(directory, "project:data", &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_assert_nonnull(strstr(error->message, "in use"));
  g_clear_error(&error);
  g_assert_false(quocker_volume_remove_all(directory, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_assert_nonnull(strstr(error->message, "in use"));
  g_clear_error(&error);
  g_assert_true(quocker_volume_project_usage(
      directory, &virtual_bytes, &allocated_bytes, &quota_bytes, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(virtual_bytes, ==, 128 * 1024 * 1024);
  qemu_permission_lock.l_type = F_UNLCK;
  g_assert_cmpint(fcntl(in_use_fd, F_OFD_SETLK, &qemu_permission_lock), ==, 0);
  g_assert_cmpint(close(in_use_fd), ==, 0);
#endif

  char *invalid = NULL;
  g_assert_false(
      quocker_volume_disk_prepare(directory, "bad", 1024, &invalid, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-volume-error"), 1);
  g_clear_error(&error);

  char *volume_directory = g_build_filename(directory, "volumes", NULL);
  g_assert_true(quocker_volume_remove_all(directory, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(first, G_FILE_TEST_EXISTS));
  char *lock_path = g_build_filename(volume_directory, ".quocker.lock", NULL);
  g_assert_cmpint(g_unlink(lock_path), ==, 0);
  g_assert_cmpint(g_rmdir(volume_directory), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(volume_directory);
  g_free(lock_path);
  g_free(second);
  g_free(first);
  g_free(directory);

  const char *cli = g_getenv("QUOCKER_CLI");
  if (cli && *cli) {
    char *project = g_dir_make_tmp("quocker-volume-cli-XXXXXX", &error);
    g_assert_no_error(error);
    char *compose = g_build_filename(project, "compose.yaml", NULL);
    g_assert_true(g_file_set_contents(
        compose, "services:\n  app:\n    image: ./ignored.qcow2\n", -1,
        &error));
    g_assert_no_error(error);
    char *quocker_state =
        g_build_filename(project, ".quocker", "project", NULL);
    g_assert_cmpint(g_mkdir_with_parents(quocker_state, 0700), ==, 0);
    char *disk = NULL;
    g_assert_true(quocker_volume_disk_prepare(quocker_state, "project:data",
                                              64 * 1024 * 1024, &disk, &error));
    g_assert_no_error(error);
    char *arguments[] = {(char *)cli,       (char *)"--project-directory",
                         project,           (char *)"--project-name",
                         (char *)"project", (char *)"-f",
                         compose,           (char *)"volume",
                         (char *)"ls",      NULL};
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint status = 0;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL,
                               NULL, &stdout_text, &stderr_text, &status,
                               &error));
    g_assert_no_error(error);
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(stdout_text, "project:data"));
    g_assert_nonnull(strstr(stdout_text, "64"));
    g_assert_nonnull(strstr(stdout_text, "USED"));
    g_free(stdout_text);
    char *df_arguments[] = {(char *)cli,       (char *)"--project-directory",
                            project,           (char *)"--project-name",
                            (char *)"project", (char *)"-f",
                            compose,           (char *)"volume",
                            (char *)"df",      NULL};
    g_assert_true(g_spawn_sync(NULL, df_arguments, NULL, G_SPAWN_DEFAULT, NULL,
                               NULL, &stdout_text, &stderr_text, &status,
                               &error));
    g_assert_no_error(error);
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(stdout_text, "VIRTUAL\tUSED\tQUOTA"));
    g_assert_nonnull(strstr(stdout_text, "128"));
    g_free(stdout_text);
    g_free(stderr_text);
    char *metadata = g_strconcat(disk, ".name", NULL);
    char *volume_dir = g_path_get_dirname(disk);
    stdout_text = run_volume_command(cli, project, compose, "inspect",
                                     "project:data", FALSE, TRUE);
    g_assert_nonnull(strstr(stdout_text, "Name: project:data"));
    g_assert_nonnull(strstr(stdout_text, "Size: 64"));
    g_free(stdout_text);
    stdout_text = run_volume_command(cli, project, compose, "rm",
                                     "project:data", TRUE, TRUE);
    g_assert_nonnull(strstr(stdout_text, "Would remove volume project:data"));
    g_free(stdout_text);
    g_assert_true(g_file_test(disk, G_FILE_TEST_IS_REGULAR));
    char *pid_path = g_build_filename(quocker_state, "app.state", NULL);
    char *pid_text = g_strdup_printf("%d\n", (int)getpid());
    g_assert_true(g_file_set_contents(pid_path, pid_text, -1, &error));
    g_assert_no_error(error);
    stdout_text = run_volume_command(cli, project, compose, "rm",
                                     "project:data", FALSE, FALSE);
    g_free(stdout_text);
    g_assert_true(g_file_test(disk, G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(pid_path), ==, 0);
    g_free(pid_text);
    g_free(pid_path);
    stdout_text = run_volume_command(cli, project, compose, "rm",
                                     "project:data", FALSE, TRUE);
    g_assert_nonnull(strstr(stdout_text, "Removed volume project:data"));
    g_free(stdout_text);
    g_assert_false(g_file_test(disk, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(metadata, G_FILE_TEST_EXISTS));
    char *cli_lock_path = g_build_filename(volume_dir, ".quocker.lock", NULL);
    g_assert_cmpint(g_unlink(cli_lock_path), ==, 0);
    g_assert_cmpint(g_rmdir(volume_dir), ==, 0);
    char *lifecycle_lock =
        g_build_filename(quocker_state, ".lifecycle.lock", NULL);
    g_assert_cmpint(g_unlink(lifecycle_lock), ==, 0);
    g_assert_cmpint(g_rmdir(quocker_state), ==, 0);
    char *metadata_root = g_build_filename(project, ".quocker", NULL);
    g_assert_cmpint(g_rmdir(metadata_root), ==, 0);
    g_assert_cmpint(g_unlink(compose), ==, 0);
    g_assert_cmpint(g_rmdir(project), ==, 0);
    g_free(metadata_root);
    g_free(lifecycle_lock);
    g_free(volume_dir);
    g_free(cli_lock_path);
    g_free(metadata);
    g_free(disk);
    g_free(quocker_state);
    g_free(compose);
    g_free(project);
  }
  return 0;
}
