#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double seconds(clockid_t id)
{
    struct timespec t;
    if (clock_gettime(id, &t) == -1) { perror("clock_gettime"); exit(1); }
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "cpu")) {
        double start = seconds(CLOCK_MONOTONIC);
        double cpu_start = seconds(CLOCK_PROCESS_CPUTIME_ID);
        volatile unsigned long work = 0;
        while (seconds(CLOCK_MONOTONIC) - start < 4.0)
            for (int i = 0; i < 10000; i++) work++;
        double wall = seconds(CLOCK_MONOTONIC) - start;
        double cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
        printf("wall=%.3f s cpu=%.3f s ratio=%.3f\n", wall, cpu, cpu / wall);
        return 0;
    }
    if (!strcmp(argv[1], "tasks")) {
        pid_t children[16];
        int count = 0, saved_errno = 0;
        for (; count < 16; count++) {
            pid_t p = fork();
            if (p == -1) { saved_errno = errno; break; }
            if (!p) { for (;;) pause(); }
            children[count] = p;
        }
        printf("parent=1 children=%d fork_error=%s\n", count,
               saved_errno ? strerror(saved_errno) : "none");
        for (int i = 0; i < count; i++) kill(children[i], SIGTERM);
        for (int i = 0; i < count; i++)
            while (waitpid(children[i], NULL, 0) == -1 && errno == EINTR) { }
        return 0;
    }
    if (!strcmp(argv[1], "memory")) {
        size_t n = 96UL * 1024 * 1024;
        volatile unsigned char *p = malloc(n);
        if (!p) { perror("malloc"); return 1; }
        puts("allocated=96 MiB; touching pages");
        for (size_t i = 0; i < n; i += 4096) p[i] = 1;
        puts("all pages touched");
        free((void *)p);
        return 0;
    }
    return 2;
}
