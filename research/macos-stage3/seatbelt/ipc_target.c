#include <fcntl.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile sig_atomic_t stopped = 0, hits = 0;
static void terminate(int s) { stopped = 1; }
static void receive(int s) { hits++; }
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  signal(SIGTERM, terminate);
  signal(SIGUSR1, receive);
  signal(SIGALRM, terminate);
  alarm(15);
  int fd = shm_open(argv[2], O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return 3;
  if (ftruncate(fd, 64))
    return 4;
  mach_port_t port;
  kern_return_t r =
      mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
  if (r)
    return 5;
  r = mach_port_insert_right(mach_task_self(), port, port,
                             MACH_MSG_TYPE_MAKE_SEND);
  if (r)
    return 6;
  r = bootstrap_register(bootstrap_port, argv[1], port);
  printf("%d %d\n", getpid(), r);
  fflush(stdout);
  while (!stopped)
    pause();
  bootstrap_register(bootstrap_port, argv[1], MACH_PORT_NULL);
  mach_port_destroy(mach_task_self(), port);
  close(fd);
  shm_unlink(argv[2]);
  return 0;
}
