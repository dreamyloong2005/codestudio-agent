#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
  if (argc > 1) {
    printf("spawned pid=%d pgid=%d sid=%d\n", getpid(), getpgrp(), getsid(0));
    return 0;
  }
  setbuf(stdout, NULL);
  printf("initial pid=%d pgid=%d sid=%d\n", getpid(), getpgrp(), getsid(0));
  pid_t child = fork();
  if (child == 0) {
    errno = 0;
    int a = setsid();
    printf("child setsid=%d errno=%d pid=%d pgid=%d sid=%d\n", a, errno,
           getpid(), getpgrp(), getsid(0));
    errno = 0;
    int b = setpgid(0, 0);
    printf("child setpgid=%d errno=%d\n", b, errno);
    _exit(0);
  }
  if (child > 0)
    waitpid(child, NULL, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  char *args[] = {argv[0], "spawned", NULL};
  pid_t spawned;
  int r = posix_spawn(&spawned, argv[0], NULL, &attr, args, environ);
  printf("spawn newgroup result=%d\n", r);
  if (!r)
    waitpid(spawned, NULL, 0);
  posix_spawnattr_destroy(&attr);
  return 0;
}
