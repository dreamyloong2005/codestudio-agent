#define _DARWIN_C_SOURCE 1
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  if (argc == 2) {
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_SETSID);
    char *args[] = {argv[0], argv[1], "after", NULL};
    pid_t p;
    int r = posix_spawn(&p, argv[0], NULL, &a, args, environ);
    return r ? 3 : 4;
  }
  int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 5;
  dprintf(fd, "{\"pid\":%d,\"pgid\":%d,\"sid\":%d}\n", getpid(), getpgrp(),
          getsid(0));
  close(fd);
  sleep(10);
  return 0;
}
