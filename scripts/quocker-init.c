/*
 * Quocker guest init: mount an OCI-derived ext4 root and run its workload.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG_PATH "/quocker/config"
#define ROOT_DEVICE "/dev/vda"
#define NEW_ROOT "/newroot"
#define OLD_ROOT "/newroot/.quocker-initramfs"
#define CONFIG_MAX_BYTES (16u * 1024u * 1024u)
#define CONFIG_MAX_ITEMS 65536u
#define CONFIG_MAX_STRING (64u * 1024u)

typedef struct GuestConfig {
  char **argv;
  uint32_t argc;
  char **environment;
  uint32_t environment_count;
  char **mount_targets;
  uint32_t *mount_read_only;
  uint32_t mount_count;
  char *working_directory;
  char *user;
} GuestConfig;

static volatile sig_atomic_t pending_signal;
static pid_t workload_process_group = -1;

static void guest_signal(int signo) {
  if (signo == SIGCHLD) {
    return;
  }
  pending_signal = signo;
  if (workload_process_group > 0) {
    kill(-workload_process_group, signo);
  }
}

static void guest_message(const char *message) {
  dprintf(STDERR_FILENO, "quocker-init: %s: %s\n", message, strerror(errno));
}

static void poweroff(void) {
  sync();
  reboot(RB_POWER_OFF);
  for (;;) {
    pause();
  }
}

static void fail_guest(const char *message) {
  guest_message(message);
  poweroff();
}

static uint32_t read_be32(const unsigned char *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int read_exact(int fd, void *buffer, size_t size) {
  unsigned char *out = buffer;
  while (size) {
    ssize_t count = read(fd, out, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return -1;
    }
    out += count;
    size -= (size_t)count;
  }
  return 0;
}

static int read_config_string(int fd, uint32_t length, char **value_out) {
  if (length > CONFIG_MAX_STRING) {
    errno = E2BIG;
    return -1;
  }
  char *value = malloc((size_t)length + 1);
  if (!value) {
    return -1;
  }
  if (read_exact(fd, value, length) < 0 || memchr(value, '\0', length)) {
    free(value);
    errno = EINVAL;
    return -1;
  }
  value[length] = '\0';
  *value_out = value;
  return 0;
}

static int read_config_item(int fd, char **value_out) {
  unsigned char encoded_length[4];
  if (read_exact(fd, encoded_length, sizeof(encoded_length)) < 0) {
    return -1;
  }
  return read_config_string(fd, read_be32(encoded_length), value_out);
}

static int valid_mount_target(const char *target) {
  if (!target || target[0] != '/' || target[1] == '\0' ||
      strlen(target) > CONFIG_MAX_STRING) {
    return 0;
  }
  const char *component = target + 1;
  for (const char *cursor = component;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      size_t length = (size_t)(cursor - component);
      if (!length || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.')) {
        return 0;
      }
      if (!*cursor) {
        return 1;
      }
      component = cursor + 1;
    }
  }
}

static int valid_environment_assignment(const char *assignment) {
  const char *equals = strchr(assignment, '=');
  if (!equals || equals == assignment ||
      !(('A' <= assignment[0] && assignment[0] <= 'Z') ||
        ('a' <= assignment[0] && assignment[0] <= 'z') ||
        assignment[0] == '_')) {
    return 0;
  }
  for (const char *p = assignment + 1; p < equals; p++) {
    if (!(('A' <= *p && *p <= 'Z') || ('a' <= *p && *p <= 'z') ||
          ('0' <= *p && *p <= '9') || *p == '_')) {
      return 0;
    }
  }
  return 1;
}

static int load_config(const char *path, GuestConfig *config) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return -1;
  }
  struct stat st;
  unsigned char header[28];
  int result = -1;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 24 ||
      st.st_size > CONFIG_MAX_BYTES || read_exact(fd, header, 8) < 0 ||
      memcmp(header, "QCFG", 4) != 0) {
    errno = EINVAL;
    goto done;
  }
  uint32_t version = read_be32(header + 4);
  size_t remaining_header = version == 1 ? 16 : version == 2 ? 20 : 0;
  if (!remaining_header || read_exact(fd, header + 8, remaining_header) < 0) {
    errno = EINVAL;
    goto done;
  }
  config->argc = read_be32(header + 8);
  config->environment_count = read_be32(header + 12);
  config->mount_count = version == 2 ? read_be32(header + 16) : 0;
  uint32_t working_directory_size =
      read_be32(header + (version == 2 ? 20 : 16));
  uint32_t user_size = read_be32(header + (version == 2 ? 24 : 20));
  if (!config->argc || config->argc > CONFIG_MAX_ITEMS ||
      config->environment_count > CONFIG_MAX_ITEMS ||
      config->mount_count > 25 || working_directory_size > CONFIG_MAX_STRING ||
      user_size > CONFIG_MAX_STRING) {
    errno = E2BIG;
    goto done;
  }
  config->argv = calloc((size_t)config->argc + 1, sizeof(char *));
  config->environment =
      calloc((size_t)config->environment_count + 1, sizeof(char *));
  config->mount_targets =
      calloc((size_t)config->mount_count + 1, sizeof(char *));
  config->mount_read_only =
      calloc((size_t)config->mount_count + 1, sizeof(uint32_t));
  if (!config->argv || !config->environment || !config->mount_targets ||
      !config->mount_read_only) {
    goto done;
  }
  for (uint32_t i = 0; i < config->argc; i++) {
    if (read_config_item(fd, &config->argv[i]) < 0) {
      goto done;
    }
  }
  for (uint32_t i = 0; i < config->environment_count; i++) {
    if (read_config_item(fd, &config->environment[i]) < 0 ||
        !valid_environment_assignment(config->environment[i])) {
      errno = EINVAL;
      goto done;
    }
  }
  for (uint32_t i = 0; i < config->mount_count; i++) {
    unsigned char flags[4];
    if (read_config_item(fd, &config->mount_targets[i]) < 0 ||
        !valid_mount_target(config->mount_targets[i]) ||
        read_exact(fd, flags, sizeof(flags)) < 0) {
      errno = EINVAL;
      goto done;
    }
    config->mount_read_only[i] = read_be32(flags);
    if (config->mount_read_only[i] > 1) {
      errno = EINVAL;
      goto done;
    }
  }
  if (working_directory_size &&
      read_config_string(fd, working_directory_size,
                         &config->working_directory) < 0) {
    goto done;
  }
  if (user_size && read_config_string(fd, user_size, &config->user) < 0) {
    goto done;
  }
  unsigned char extra;
  ssize_t extra_count;
  do {
    extra_count = read(fd, &extra, 1);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    errno = EINVAL;
    goto done;
  }
  result = 0;
done:
  close(fd);
  return result;
}

static int ensure_directory(const char *path) {
  struct stat st;
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
  }
  return -1;
}

static int ensure_mount_directory(const char *target, char *full_path,
                                  size_t full_path_size) {
  char relative[PATH_MAX];
  if (!valid_mount_target(target) || strlen(target + 1) >= sizeof(relative) ||
      snprintf(full_path, full_path_size, "%s%s", NEW_ROOT, target) >=
          (int)full_path_size) {
    errno = EINVAL;
    return -1;
  }
  memcpy(relative, target + 1, strlen(target + 1) + 1);
  int directory =
      open(NEW_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) {
    return -1;
  }
  char *save = NULL;
  for (char *component = strtok_r(relative, "/", &save); component;
       component = strtok_r(NULL, "/", &save)) {
    if (mkdirat(directory, component, 0755) < 0 && errno != EEXIST) {
      close(directory);
      return -1;
    }
    int child = openat(directory, component,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) {
      close(directory);
      return -1;
    }
    close(directory);
    directory = child;
  }
  close(directory);
  return 0;
}

static int mount_guest_volumes(const GuestConfig *config) {
  for (uint32_t i = 0; i < config->mount_count; i++) {
    char device[32];
    char target[PATH_MAX];
    struct stat st;
    if (snprintf(device, sizeof(device), "/dev/vd%c", 'b' + i) >=
            (int)sizeof(device) ||
        ensure_mount_directory(config->mount_targets[i], target,
                               sizeof(target)) < 0) {
      return -1;
    }
    int found = 0;
    for (int retry = 0; retry < 50; retry++) {
      if (stat(device, &st) == 0 && S_ISBLK(st.st_mode)) {
        found = 1;
        break;
      }
      usleep(100000);
    }
    if (!found) {
      errno = ENODEV;
      return -1;
    }
    unsigned long flags = MS_RELATIME;
    if (config->mount_read_only[i]) {
      flags |= MS_RDONLY;
    }
    if (mount(device, target, "ext4", flags, NULL) < 0) {
      return -1;
    }
  }
  return 0;
}

static int prepare_root_device(void) {
  if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL) == 0) {
    return 0;
  }
  if (errno != ENODEV && errno != EINVAL) {
    return -1;
  }
  struct stat st;
  if (lstat(ROOT_DEVICE, &st) == 0) {
    return S_ISBLK(st.st_mode) ? 0 : -1;
  }
  return mknod(ROOT_DEVICE, S_IFBLK | 0600, makedev(252, 0));
}

static int mount_guest_root(const GuestConfig *config) {
  if (ensure_directory(NEW_ROOT) < 0 || prepare_root_device() < 0 ||
      mount(ROOT_DEVICE, NEW_ROOT, "ext4", MS_RELATIME, NULL) < 0) {
    return -1;
  }
  const char *mountpoints[] = {"proc", "sys", "dev", "run", NULL};
  for (const char **name = mountpoints; *name; name++) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", NEW_ROOT, *name) >=
            (int)sizeof(path) ||
        ensure_directory(path) < 0) {
      return -1;
    }
  }
  if (ensure_directory(OLD_ROOT) < 0 ||
      mount("proc", NEW_ROOT "/proc", "proc", MS_NOSUID | MS_NOEXEC, NULL) <
          0 ||
      mount("sysfs", NEW_ROOT "/sys", "sysfs", MS_NOSUID | MS_NOEXEC, NULL) <
          0 ||
      mount("devtmpfs", NEW_ROOT "/dev", "devtmpfs", MS_NOSUID, NULL) < 0) {
    return -1;
  }
  const char *device_mountpoints[] = {"shm", "pts", "mqueue", NULL};
  for (const char **name = device_mountpoints; *name; name++) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/dev/%s", NEW_ROOT, *name) >=
            (int)sizeof(path) ||
        ensure_directory(path) < 0) {
      return -1;
    }
  }
  if (mount("tmpfs", NEW_ROOT "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV,
            "mode=1777") < 0 ||
      mount("devpts", NEW_ROOT "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
            "newinstance,ptmxmode=0666,mode=0620") < 0 ||
      mount("mqueue", NEW_ROOT "/dev/mqueue", "mqueue",
            MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
    return -1;
  }
  if (mount_guest_volumes(config) < 0) {
    return -1;
  }
  if (syscall(SYS_pivot_root, NEW_ROOT, OLD_ROOT) < 0 || chdir("/") < 0 ||
      umount2("/.quocker-initramfs", MNT_DETACH) < 0 ||
      rmdir("/.quocker-initramfs") < 0) {
    return -1;
  }
  if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") < 0) {
    return -1;
  }
  return 0;
}

static int parse_id(const char *text, uid_t *id_out) {
  if (!text || !*text) {
    return -1;
  }
  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !end || *end || value > (unsigned long)(uid_t)-1) {
    return -1;
  }
  *id_out = (uid_t)value;
  return 0;
}

static int lookup_passwd(const char *name, uid_t *uid_out, gid_t *gid_out) {
  FILE *file = fopen("/etc/passwd", "re");
  if (!file) {
    return -1;
  }
  char *line = NULL;
  size_t capacity = 0;
  int found = -1;
  while (getline(&line, &capacity, file) >= 0) {
    char *save = NULL;
    char *field_name = strtok_r(line, ":", &save);
    (void)strtok_r(NULL, ":", &save);
    char *uid_text = strtok_r(NULL, ":\n", &save);
    char *gid_text = strtok_r(NULL, ":\n", &save);
    uid_t uid;
    uid_t gid;
    if (field_name && uid_text && gid_text && strcmp(field_name, name) == 0 &&
        parse_id(uid_text, &uid) == 0 && parse_id(gid_text, &gid) == 0) {
      *uid_out = uid;
      *gid_out = gid;
      found = 0;
      break;
    }
  }
  free(line);
  fclose(file);
  return found;
}

static int lookup_group(const char *name, gid_t *gid_out) {
  FILE *file = fopen("/etc/group", "re");
  if (!file) {
    return -1;
  }
  char *line = NULL;
  size_t capacity = 0;
  int found = -1;
  while (getline(&line, &capacity, file) >= 0) {
    char *save = NULL;
    char *field_name = strtok_r(line, ":", &save);
    (void)strtok_r(NULL, ":", &save);
    char *gid_text = strtok_r(NULL, ":\n", &save);
    gid_t gid;
    if (field_name && gid_text && strcmp(field_name, name) == 0 &&
        parse_id(gid_text, &gid) == 0) {
      *gid_out = gid;
      found = 0;
      break;
    }
  }
  free(line);
  fclose(file);
  return found;
}

static int apply_user(const char *user_text) {
  if (!user_text || !*user_text) {
    return 0;
  }
  char *user = strdup(user_text);
  if (!user) {
    return -1;
  }
  char *group_text = strchr(user, ':');
  if (group_text) {
    *group_text++ = '\0';
  }
  uid_t uid;
  gid_t gid = 0;
  const int numeric_user = parse_id(user, &uid) == 0;
  if (!numeric_user && lookup_passwd(user, &uid, &gid) < 0) {
    free(user);
    errno = ENOENT;
    return -1;
  }
  if (group_text && *group_text) {
    if (parse_id(group_text, &gid) < 0 && lookup_group(group_text, &gid) < 0) {
      free(user);
      errno = ENOENT;
      return -1;
    }
  }
  int result = setgroups(0, NULL);
  if (result == 0) {
    result = setgid(gid);
  }
  if (result == 0) {
    result = setuid(uid);
  }
  free(user);
  return result;
}

static int run_workload(const GuestConfig *config) {
  if (clearenv() < 0) {
    return -1;
  }
  for (uint32_t i = 0; i < config->environment_count; i++) {
    char *assignment = strdup(config->environment[i]);
    if (!assignment) {
      return -1;
    }
    char *equals = strchr(assignment, '=');
    *equals = '\0';
    int result = setenv(assignment, equals + 1, 1);
    free(assignment);
    if (result < 0) {
      return -1;
    }
  }
  if (config->working_directory && chdir(config->working_directory) < 0) {
    return -1;
  }
  if (apply_user(config->user) < 0) {
    return -1;
  }
  execvpe(config->argv[0], config->argv, environ);
  return -1;
}

static void supervise(const GuestConfig *config) {
  sigset_t signals;
  sigset_t previous_mask;
  sigemptyset(&signals);
  sigaddset(&signals, SIGCHLD);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGHUP);
  sigaddset(&signals, SIGQUIT);
  if (sigprocmask(SIG_BLOCK, &signals, &previous_mask) < 0) {
    fail_guest("could not block signals");
  }
  struct sigaction action = {.sa_handler = guest_signal};
  sigemptyset(&action.sa_mask);
  const int watched_signals[] = {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGQUIT};
  for (size_t i = 0; i < sizeof(watched_signals) / sizeof(watched_signals[0]);
       i++) {
    if (sigaction(watched_signals[i], &action, NULL) < 0) {
      fail_guest("could not install signal handler");
    }
  }
  int exec_status_pipe[2];
  if (pipe2(exec_status_pipe, O_CLOEXEC) < 0) {
    fail_guest("could not create workload exec-status pipe");
  }
  pid_t child = fork();
  if (child < 0) {
    close(exec_status_pipe[0]);
    close(exec_status_pipe[1]);
    fail_guest("could not fork workload");
  }
  if (child == 0) {
    close(exec_status_pipe[0]);
    sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    setpgid(0, 0);
    run_workload(config);
    int exec_error = errno ? errno : EIO;
    const unsigned char *error_bytes = (const unsigned char *)&exec_error;
    size_t error_length = sizeof(exec_error);
    while (error_length > 0) {
      ssize_t written =
          write(exec_status_pipe[1], error_bytes, error_length);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        break;
      }
      error_bytes += written;
      error_length -= (size_t)written;
    }
    guest_message("could not exec image command");
    _exit(127);
  }
  close(exec_status_pipe[1]);
  workload_process_group = child;
  setpgid(child, child);
  int exec_error = 0;
  size_t error_length = 0;
  while (error_length < sizeof(exec_error)) {
    ssize_t count = read(exec_status_pipe[0],
                         (unsigned char *)&exec_error + error_length,
                         sizeof(exec_error) - error_length);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    error_length += (size_t)count;
  }
  close(exec_status_pipe[0]);
  if (error_length == 0) {
    dprintf(STDERR_FILENO, "QUOCKER_READY pid=%ld\n", (long)child);
  } else if (error_length != sizeof(exec_error)) {
    exec_error = EIO;
  }
  int workload_status = 0;
  int workload_done = 0;
  for (;;) {
    pid_t finished;
    int status;
    while ((finished = waitpid(-1, &status, WNOHANG)) > 0) {
      if (finished == child) {
        workload_status = status;
        workload_done = 1;
      }
    }
    if (workload_done) {
      break;
    }
    sigsuspend(&previous_mask);
  }
  kill(-child, SIGTERM);
  usleep(100000);
  kill(-child, SIGKILL);
  while (waitpid(-1, NULL, WNOHANG) > 0) {
  }
  if (WIFEXITED(workload_status)) {
    dprintf(STDERR_FILENO, "QUOCKER_EXIT status=%d\n",
            WEXITSTATUS(workload_status));
  } else if (WIFSIGNALED(workload_status)) {
    dprintf(STDERR_FILENO, "QUOCKER_EXIT signal=%d\n",
            WTERMSIG(workload_status));
  }
  poweroff();
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--validate-config") == 0) {
    GuestConfig checked = {0};
    if (load_config(argv[2], &checked) < 0) {
      dprintf(STDERR_FILENO, "quocker-init: invalid config: %s\n",
              strerror(errno));
      return 1;
    }
    dprintf(STDOUT_FILENO,
            "valid QCFG v2: %u argv, %u environment values, %u volumes\n",
            checked.argc, checked.environment_count, checked.mount_count);
    return 0;
  }
  if (getpid() != 1) {
    dprintf(STDERR_FILENO, "quocker-init: must run as guest PID 1\n");
    return 125;
  }
  GuestConfig config = {0};
  if (load_config(CONFIG_PATH, &config) < 0) {
    fail_guest("invalid or missing guest configuration");
  }
  if (mount_guest_root(&config) < 0) {
    fail_guest("could not mount and enter the guest root filesystem");
  }
  supervise(&config);
}
