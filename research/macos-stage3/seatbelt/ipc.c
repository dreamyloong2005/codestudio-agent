#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  int pid = atoi(argv[1]);
  errno = 0;
  int r = kill(pid, 0);
  printf("signal_probe result=%d errno=%d\n", r, errno);
  errno = 0;
  r = kill(pid, SIGUSR1);
  printf("signal_delivery result=%d errno=%d\n", r, errno);
  mach_port_t port = MACH_PORT_NULL;
  int m = task_name_for_pid(mach_task_self(), pid, &port);
  printf("task_name_for_pid result=%d port=%u\n", m, port);
  if (port)
    mach_port_deallocate(mach_task_self(), port);
  port = MACH_PORT_NULL;
  m = task_for_pid(mach_task_self(), pid, &port);
  printf("task_for_pid result=%d port=%u\n", m, port);
  if (port)
    mach_port_deallocate(mach_task_self(), port);
  port = MACH_PORT_NULL;
  m = bootstrap_look_up(bootstrap_port, argv[3], &port);
  printf("mach_lookup result=%d port=%u\n", m, port);
  if (port)
    mach_port_deallocate(mach_task_self(), port);
  errno = 0;
  int fd = shm_open(argv[2], O_RDONLY, 0600);
  printf("shared_memory result=%d errno=%d\n", fd >= 0, errno);
  if (fd >= 0)
    close(fd);
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {0};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, argv[4], sizeof(a.sun_path) - 1);
  errno = 0;
  r = connect(sock, (void *)&a, sizeof a);
  printf("unix_connect result=%d errno=%d\n", r, errno);
  close(sock);
  return 0;
}
