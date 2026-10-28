#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  pid_t parent = atoi(argv[1]);
  struct proc_bsdinfo before = {0}, after = {0};
  if (parent <= 1 || getppid() != parent ||
      proc_pidinfo(parent, PROC_PIDTBSDINFO, 0, &before, sizeof before) !=
          sizeof before)
    return 3;
  int queue = kqueue();
  if (queue < 0 || fcntl(queue, F_SETFD, FD_CLOEXEC) < 0)
    return 4;
  struct kevent event;
  EV_SET(&event, parent, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT,
         NOTE_EXIT, 0, NULL);
  if (kevent(queue, &event, 1, NULL, 0, NULL) < 0 || getppid() != parent ||
      proc_pidinfo(parent, PROC_PIDTBSDINFO, 0, &after, sizeof after) !=
          sizeof after ||
      before.pbi_start_tvsec != after.pbi_start_tvsec ||
      before.pbi_start_tvusec != after.pbi_start_tvusec)
    return 5;
  pid_t child = fork();
  if (child < 0)
    return 6;
  if (!child) {
    int null = open("/dev/null", O_RDWR);
    for (int i = 0; i < 3; i++)
      dup2(null, i);
    if (null > 2)
      close(null);
    execl("/usr/bin/sandbox-exec", "sandbox-exec", "-f", argv[2], argv[3],
          argv[4], NULL);
    _exit(127);
  }
  printf("%d\n", child);
  fflush(stdout);
  int status = 0;
  int host_exited = 0;
  for (;;) {
    pid_t reaped = waitpid(child, &status, WNOHANG);
    if (reaped == child)
      break;
    if (reaped < 0 && errno != EINTR)
      return 7;
    const struct timespec t = {0, 100000000};
    int count = kevent(queue, NULL, 0, &event, 1, &t);
    if (count < 0 && errno == EINTR)
      continue;
    if (count != 0) {
      host_exited = 1; /* direct, unreaped child PID remains owned */
      if (kill(child, SIGKILL) < 0 && errno != ESRCH)
        return 8;
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
  }
  int fd = open(argv[5], O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return 9;
  dprintf(fd,
          "{\"guardian_pid\":%d,\"child_pid\":%d,\"host_exited\":%s,\"child_"
          "signal\":%d}\n",
          getpid(), child, host_exited ? "true" : "false",
          WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  close(fd);
  close(queue);
  return 0;
}
