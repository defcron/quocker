#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char server[512];
  if (!fgets(server, sizeof(server), stdin) ||
      strcmp(server, "https://private.example\n") != 0) {
    return 2;
  }
  const char *mode = getenv("QUOCKER_TEST_HELPER_MODE");
  if (mode && strcmp(mode, "missing") == 0) {
    return 1;
  }
  if (mode && strcmp(mode, "large") == 0) {
    for (unsigned i = 0; i < 1024; i++) {
      fputs("................................................................\n", stdout);
    }
    return 0;
  }
  if (mode && strcmp(mode, "invalid") == 0) {
    fputs("{}", stdout);
    return 0;
  }
  fputs("{\"ServerURL\":\"https://private.example\","
        "\"Username\":\"helper-user\",\"Secret\":\"helper-secret\"}",
        stdout);
  return 0;
}
