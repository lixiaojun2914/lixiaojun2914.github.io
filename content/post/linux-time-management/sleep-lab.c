#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct timespec read_clock(clockid_t id)
{
    struct timespec t;
    if (clock_gettime(id, &t) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return t;
}

static struct timespec add_ms(struct timespec t, long ms)
{
    t.tv_nsec += ms * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    return t;
}

static double diff_ms(struct timespec end, struct timespec start)
{
    return (end.tv_sec - start.tv_sec) * 1000.0 +
           (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static void sleep_until(struct timespec deadline)
{
    int err;
    do {
        err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                              &deadline, NULL);
    } while (err == EINTR);
    if (err != 0) {
        fprintf(stderr, "clock_nanosleep: %s\n", strerror(err));
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    struct timespec start = read_clock(CLOCK_MONOTONIC);
    struct timespec cpu_start = read_clock(CLOCK_PROCESS_CPUTIME_ID);
    sleep_until(add_ms(start, 100));
    struct timespec end = read_clock(CLOCK_MONOTONIC);
    struct timespec cpu_end = read_clock(CLOCK_PROCESS_CPUTIME_ID);

    printf("sleep target=100.000 ms elapsed=%.3f ms cpu=%.3f ms\n",
           diff_ms(end, start), diff_ms(cpu_end, cpu_start));

    start = read_clock(CLOCK_MONOTONIC);
    for (int i = 1; i <= 3; i++) {
        struct timespec deadline = add_ms(start, i * 20);
        sleep_until(deadline);
        end = read_clock(CLOCK_MONOTONIC);
        printf("period=%d target=%.3f ms elapsed=%.3f ms late=%.3f ms\n",
               i, i * 20.0, diff_ms(end, start), diff_ms(end, deadline));
    }
    return EXIT_SUCCESS;
}
