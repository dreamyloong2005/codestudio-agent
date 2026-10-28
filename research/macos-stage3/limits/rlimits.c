// Bounded, unprivileged probes; changes apply only to this fixture process.
#include <errno.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void memory(void) {
    const size_t bytes = 64ULL * 1024 * 1024;
    unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        printf("{\"mmap\":false,\"errno\":%d}\n", errno);
        return;
    }
    for (size_t i = 0; i < bytes; i += 4096)
        p[i] = (unsigned char)((i / 4096) % 251 + 1);
    struct task_vm_info info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                 (task_info_t)&info, &count);
    printf("{\"mmap\":true,\"touched_bytes\":%zu,\"task_info\":%d,"
           "\"resident_bytes\":%llu,\"phys_footprint\":%llu}\n",
           bytes, kr, info.resident_size, info.phys_footprint);
    munmap(p, bytes);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) return 3;
    alarm(4);
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "footprint")) {
        int old = -1;
        kern_return_t kr = task_set_phys_footprint_limit(mach_task_self(), 32, &old);
        printf("{\"api\":\"task_set_phys_footprint_limit\",\"mb\":32,"
               "\"result\":%d,\"old_mb\":%d}\n", kr, old);
        memory();
        return 0;
    }
    int resource;
    if (!strcmp(argv[1], "as")) resource = RLIMIT_AS;
    else if (!strcmp(argv[1], "rss")) resource = RLIMIT_RSS;
    else if (!strcmp(argv[1], "nproc")) resource = RLIMIT_NPROC;
    else if (!strcmp(argv[1], "cpu")) resource = RLIMIT_CPU;
    else return 2;
    struct rlimit limit = {32ULL * 1024 * 1024, 32ULL * 1024 * 1024};
    if (resource == RLIMIT_NPROC) limit = (struct rlimit){2, 2};
    if (resource == RLIMIT_CPU) limit = (struct rlimit){1, 2};
    errno = 0;
    int result = setrlimit(resource, &limit), saved = errno;
    struct rlimit readback = {0, 0};
    int queried = getrlimit(resource, &readback);
    printf("{\"api\":\"setrlimit\",\"mode\":\"%s\",\"resource\":%d,"
           "\"result\":%d,\"errno\":%d,\"queried\":%d,"
           "\"soft\":%llu,\"hard\":%llu}\n",
           argv[1], resource, result, saved, queried,
           readback.rlim_cur, readback.rlim_max);
    if (resource == RLIMIT_NPROC) {
        errno = 0;
        pid_t child = fork();
        saved = errno;
        if (child == 0) _exit(0);
        printf("{\"first_fork\":%s,\"errno\":%d}\n",
               child > 0 ? "true" : "false", saved);
        if (child > 0) while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    } else if (resource == RLIMIT_CPU) {
        volatile uint64_t n = 0;
        for (;;) n = n * 1664525 + 1013904223;
    } else memory();
    return 0;
}
