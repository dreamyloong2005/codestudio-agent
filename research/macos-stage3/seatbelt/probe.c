#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
static void access_file(const char *what, const char *path, int flags) {
  errno = 0;
  int fd = open(path, flags, 0600);
  int error = errno;
  if (fd >= 0) {
    if (flags & O_WRONLY)
      write(fd, "x", 1);
    else {
      char b;
      read(fd, &b, 1);
    }
    close(fd);
  }
  printf("%s ok=%d errno=%d\n", what, fd >= 0, error);
}
extern int probe_dependency(void);
int main(int argc, char **argv) {
  if (argc < 5)
    return 2;
  printf("dependency value=%d\n", probe_dependency());
  access_file("package_read", argv[1], O_RDONLY);
  access_file("package_write", argv[1], O_WRONLY);
  access_file("scratch_write", argv[2], O_WRONLY | O_CREAT);
  access_file("host_read", argv[3], O_RDONLY);
  access_file("host_write", argv[3], O_WRONLY);
  errno = 0;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int serr = errno;
  printf("socket ok=%d errno=%d\n", s >= 0, serr);
  if (s >= 0) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(atoi(argv[4]));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    errno = 0;
    int r = connect(s, (void *)&a, sizeof a);
    printf("network_connect ok=%d errno=%d\n", r == 0, errno);
    close(s);
  }
  errno = 0;
  pid_t c = fork();
  int ferr = errno;
  if (c == 0)
    _exit(0);
  if (c > 0)
    waitpid(c, 0, 0);
  printf("fork ok=%d errno=%d\n", c >= 0, ferr);
  fflush(stdout);
  errno = 0;
  execl("/usr/bin/true", "true", NULL);
  printf("foreign_exec ok=0 errno=%d\n", errno);
  return 0;
}
