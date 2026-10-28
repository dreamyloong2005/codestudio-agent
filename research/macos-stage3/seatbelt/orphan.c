#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  pid_t child = fork();
  if (child < 0)
    return 3;
  if (child == 0) {
    if (setsid() < 0)
      _exit(4);
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
      _exit(5);
    dprintf(fd, "%d\n", getpid());
    close(fd);
    sleep(3);
    fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
      _exit(6);
    write(fd, "survived host exit\n", 19);
    close(fd);
    _exit(0);
  }
  int status;
  waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 7;
}
