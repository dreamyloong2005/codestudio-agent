// Private APIs are research subjects here, not product dependencies.
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Signatures verified against Apple libproc and kern_memorystatus sources.
extern int proc_setcpu_percentage(pid_t, int, int);
extern int proc_clear_cpulimits(pid_t);
extern int proc_setthread_cpupercent(uint8_t, uint32_t);
extern int memorystatus_control(uint32_t, pid_t, uint32_t, void *, size_t);

static double now(clockid_t which) {
    struct timespec t;
    clock_gettime(which, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *burn(void *unused) {
    (void)unused;
    double start = now(CLOCK_MONOTONIC), cpu = now(CLOCK_PROCESS_CPUTIME_ID);
    volatile unsigned n = 0;
    while (now(CLOCK_MONOTONIC) - start < 1.1)
        for (int i = 0; i < 1000; i++) n = n * 1664525 + 1013904223;
    printf("{\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f}\n",
           now(CLOCK_MONOTONIC) - start, now(CLOCK_PROCESS_CPUTIME_ID) - cpu);
    return NULL;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    const struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core) != 0) return 3;
    alarm(4);
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "memory")) {
        struct task_vm_info info = {0};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                     (task_info_t)&info, &count);
        printf("{\"task_info\":%d,\"virtual_size\":%llu,\"resident_size\":%llu,"
               "\"phys_footprint\":%llu}\n",
               kr, info.virtual_size, info.resident_size, info.phys_footprint);
        for (unsigned long long bytes = 64ULL << 20; bytes <= 4ULL << 30; bytes *= 8) {
            struct rlimit r = {bytes, bytes};
            errno = 0;
            int rc = setrlimit(RLIMIT_AS, &r), saved = errno;
            printf("{\"limit_bytes\":%llu,\"result\":%d,\"errno\":%d}\n",
                   bytes, rc, saved);
        }
        // MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT = 6; flags contain megabytes.
        errno = 0;
        int rc = memorystatus_control(6, getpid(), 32, NULL, 0), saved = errno;
        printf("{\"api\":\"memorystatus_control_SET_JETSAM_TASK_LIMIT\","
               "\"result\":%d,\"errno\":%d}\n", rc, saved);
        return 0;
    }
    if (!strcmp(argv[1], "thread") || !strcmp(argv[1], "newthread")) {
        errno = 0;
        int rc = proc_setthread_cpupercent(10, 100), saved = errno;
        printf("{\"api\":\"proc_setthread_cpupercent\",\"percent\":10,"
               "\"refill_ms\":100,\"result\":%d,\"errno\":%d}\n", rc, saved);
        if (!strcmp(argv[1], "newthread")) {
            pthread_t t;
            rc = pthread_create(&t, NULL, burn, NULL);
            printf("{\"new_thread\":%d}\n", rc);
            if (!rc) pthread_join(t, NULL);
        } else burn(NULL);
        return 0;
    }
    if (strcmp(argv[1], "process") && strcmp(argv[1], "clear") &&
        strcmp(argv[1], "fork")) return 2;
    // PROC_POLICY_RUSAGE_ACTION_THROTTLE = 1.
    errno = 0;
    int rc = proc_setcpu_percentage(getpid(), 1, 20), saved = errno;
    printf("{\"api\":\"proc_setcpu_percentage\",\"percent\":20,"
           "\"result\":%d,\"errno\":%d}\n", rc, saved);
    if (!strcmp(argv[1], "clear")) {
        errno = 0;
        rc = proc_clear_cpulimits(getpid());
        saved = errno;
        printf("{\"api\":\"proc_clear_cpulimits\",\"result\":%d,\"errno\":%d}\n",
               rc, saved);
    }
    if (!strcmp(argv[1], "fork")) {
        errno = 0;
        pid_t p = fork();
        saved = errno;
        if (!p) { alarm(4); burn(NULL); _exit(0); }
        printf("{\"fork_succeeded\":%s,\"errno\":%d}\n", p > 0 ? "true" : "false", saved);
        if (p > 0) while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
        else return 3;
    } else burn(NULL);
    return 0;
}
