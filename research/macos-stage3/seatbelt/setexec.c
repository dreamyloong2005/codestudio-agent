#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  if (argc > 1 && strcmp(argv[1], "after") == 0) {
    printf("after_setexec pid=%d pgid=%d sid=%d\n", getpid(), getpgrp(),
           getsid(0));
    return 0;
  }
  printf("before_setexec pid=%d pgid=%d sid=%d\n", getpid(), getpgrp(),
         getsid(0));
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(
      &attr, POSIX_SPAWN_SETEXEC |
                 ((argc > 1 && strcmp(argv[1], "group") == 0)
                      ? POSIX_SPAWN_SETPGROUP
                      : ((argc > 1 && strcmp(argv[1], "session") == 0)
                             ? POSIX_SPAWN_SETSID
                             : 0)));
  posix_spawnattr_setpgroup(&attr, 0);
  char *args[] = {argv[0], "after", NULL};
  pid_t spawned;
  int r = posix_spawn(&spawned, argv[0], NULL, &attr, args, environ);
  printf("setexec result=%d\n", r);
  return 0;
}
