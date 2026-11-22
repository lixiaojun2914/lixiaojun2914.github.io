#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct result { int nice_value; double cpu_ms; };
static void fail(const char *what) { perror(what); exit(EXIT_FAILURE); }
static double milliseconds(struct timespec t)
{
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static struct timespec now(clockid_t id)
{
    struct timespec t;
    if (clock_gettime(id, &t) == -1) fail("clock_gettime");
    return t;
}
static void transfer(int fd, void *buf, size_t size, int writing)
{
    char *p = buf;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n == -1 && errno == EINTR) continue;
        if (n <= 0) { fprintf(stderr, "pipe transfer failed\n"); exit(EXIT_FAILURE); }
        p += n; size -= (size_t)n;
    }
}

int main(void)
{
    cpu_set_t allowed, one;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == -1) fail("affinity");
    int cpu;
    for (cpu = 0; cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed); cpu++) { }
    if (cpu == CPU_SETSIZE) return EXIT_FAILURE;
    CPU_ZERO(&one); CPU_SET(cpu, &one);
    struct timespec *deadline = mmap(NULL, sizeof(*deadline),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (deadline == MAP_FAILED) fail("mmap");
    int ready[2], start[2], output[2];
    if (pipe(ready) || pipe(start) || pipe(output)) fail("pipe");
    pid_t children[2];
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        if (children[i] == -1) fail("fork");
        if (children[i] == 0) {
            close(ready[0]); close(start[1]); close(output[0]);
            if (sched_setaffinity(0, sizeof(one), &one) == -1) fail("pin");
            int target_nice = i * 5;
            if (setpriority(PRIO_PROCESS, 0, target_nice) == -1) fail("nice");
            char token = 'r';
            transfer(ready[1], &token, 1, 1);
            transfer(start[0], &token, 1, 0);
            struct timespec before = now(CLOCK_PROCESS_CPUTIME_ID);
            volatile unsigned long work = 0;
            while (milliseconds(now(CLOCK_MONOTONIC)) < milliseconds(*deadline))
                for (int j = 0; j < 4096; j++) work++;
            struct timespec after = now(CLOCK_PROCESS_CPUTIME_ID);
            struct result r = { target_nice,
                milliseconds(after) - milliseconds(before) };
            transfer(output[1], &r, sizeof(r), 1);
            _exit(0);
        }
    }
    close(ready[1]); close(start[0]); close(output[1]);
    char tokens[2];
    transfer(ready[0], tokens, 2, 0);
    *deadline = now(CLOCK_MONOTONIC);
    deadline->tv_sec += 2;
    transfer(start[1], "ss", 2, 1);
    struct result rows[2], r;
    for (int i = 0; i < 2; i++) {
        transfer(output[0], &r, sizeof(r), 0);
        rows[r.nice_value == 0 ? 0 : 1] = r;
    }
    for (int i = 0; i < 2; i++) {
        int status;
        if (waitpid(children[i], &status, 0) == -1 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) return EXIT_FAILURE;
    }
    printf("cpu=%d wall_target=2000 ms\n", cpu);
    for (int i = 0; i < 2; i++)
        printf("nice=%d cpu=%.3f ms\n", rows[i].nice_value, rows[i].cpu_ms);
    printf("cpu_ratio=%.3f\n", rows[0].cpu_ms / rows[1].cpu_ms);
    close(ready[0]); close(start[1]); close(output[0]);
    munmap(deadline, sizeof(*deadline));
    return EXIT_SUCCESS;
}
