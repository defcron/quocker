#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc >= 4 && strcmp(argv[1], "info") == 0) {
    const char *path = argv[argc - 1];
    char *sidecar = NULL;
    if (asprintf(&sidecar, "%s.base", path) < 0) {
      return 2;
    }
    FILE *file = fopen(sidecar, "r");
    free(sidecar);
    if (file) {
      char backing[4096];
      if (!fgets(backing, sizeof(backing), file)) {
        fclose(file);
        return 3;
      }
      fclose(file);
      backing[strcspn(backing, "\r\n")] = '\0';
      printf("{\"format\":\"qcow2\",\"backing-filename\":\"%s\"}\n",
             backing);
    } else {
      puts("{\"format\":\"raw\"}");
    }
    return 0;
  }
  if (argc >= 2 && strcmp(argv[1], "create") == 0) {
    const char *base = NULL;
    const char *output = argv[argc - 1];
    for (int i = 1; i + 1 < argc; i++) {
      if (strcmp(argv[i], "-b") == 0) {
        base = argv[i + 1];
      }
    }
    FILE *disk = fopen(output, "w");
    if (!disk) {
      return 4;
    }
    fclose(disk);
    if (base) {
      char *sidecar = NULL;
      if (asprintf(&sidecar, "%s.base", output) < 0) {
        return 5;
      }
      FILE *file = fopen(sidecar, "w");
      free(sidecar);
      if (!file) {
        return 6;
      }
      fprintf(file, "%s\n", base);
      fclose(file);
    }
    return 0;
  }
  return 1;
}
