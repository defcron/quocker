#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void handle_term(int signo) { stopped = 1; }

static void serve_qmp(int client) {
  FILE *stream = fdopen(client, "r+");
  if (!stream) {
    close(client);
    return;
  }
  setvbuf(stream, NULL, _IONBF, 0);
  fputs("{\"QMP\":{\"version\":{\"qemu\":{\"major\":11,\"minor\":1,\"micro\":0},"
        "\"package\":\"fake\"},\"capabilities\":[]}}\r\n",
        stream);
  char line[4096];
  while (!stopped && fgets(line, sizeof(line), stream)) {
    if (strstr(line, "human-monitor-command")) {
      fputs("{\"return\":\"Hub -1 (quocker-net):\\r\\n  "
            "TCP[HOST_FORWARD]   9 127.0.0.1 41234 10.0.2.15 80 0 0"
            "\\r\\n\"}\r\n",
            stream);
    } else if (strstr(line, "qmp_capabilities") || strstr(line, "\"stop\"") ||
        strstr(line, "\"cont\"")) {
      fputs("{\"return\":{}}\r\n", stream);
    } else {
      fputs("{\"error\":{\"class\":\"CommandNotFound\","
            "\"desc\":\"unsupported fake QMP command\"}}\r\n",
            stream);
    }
  }
  fclose(stream);
}

int main(int argc, char **argv) {
  signal(SIGTERM, handle_term);
  const char *pidfile = NULL;
  const char *qmp_option = NULL;
  int daemonize = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-pidfile") == 0 && i + 1 < argc) {
      pidfile = argv[++i];
    } else if (strcmp(argv[i], "-qmp") == 0 && i + 1 < argc) {
      qmp_option = argv[++i];
    } else if (strcmp(argv[i], "-daemonize") == 0) {
      daemonize = 1;
    }
  }
  int qmp_server = -1;
  char *qmp_path = NULL;
  if (qmp_option && strncmp(qmp_option, "unix:", 5) == 0) {
    const char *path_start = qmp_option + 5;
    const char *options = strchr(path_start, ',');
    qmp_path = options ? strndup(path_start, options - path_start)
                       : strdup(path_start);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(qmp_path) >= sizeof(address.sun_path)) {
      free(qmp_path);
      return 4;
    }
    strcpy(address.sun_path, qmp_path);
    qmp_server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (qmp_server < 0) {
      free(qmp_path);
      return 5;
    }
    unlink(qmp_path);
    if (bind(qmp_server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(qmp_server, 4) < 0) {
      close(qmp_server);
      unlink(qmp_path);
      free(qmp_path);
      return 6;
    }
  }
  if (daemonize && pidfile) {
    pid_t child = fork();
    if (child < 0) {
      return 2;
    }
    if (child > 0) {
      if (qmp_server >= 0) {
        close(qmp_server);
      }
      FILE *file = fopen(pidfile, "w");
      if (!file) {
        kill(child, SIGKILL);
        if (qmp_path) {
          unlink(qmp_path);
          free(qmp_path);
        }
        return 3;
      }
      fprintf(file, "%ld\n", (long)child);
      fclose(file);
      return 0;
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }
  while (!stopped) {
    if (qmp_server >= 0) {
      struct pollfd descriptor = {.fd = qmp_server, .events = POLLIN};
      int ready = poll(&descriptor, 1, 100);
      if (ready > 0 && (descriptor.revents & POLLIN)) {
        int client = accept(qmp_server, NULL, NULL);
        if (client >= 0) {
          serve_qmp(client);
        }
      }
    } else {
      usleep(10000);
    }
  }
  if (qmp_server >= 0) {
    close(qmp_server);
  }
  if (qmp_path) {
    unlink(qmp_path);
    free(qmp_path);
  }
  return 0;
}
