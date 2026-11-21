#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct clock_entry {
    const char *name;
    clockid_t id;
};

int main(void)
{
    const struct clock_entry clocks[] = {
        { "REALTIME", CLOCK_REALTIME },
        { "MONOTONIC", CLOCK_MONOTONIC },
        { "BOOTTIME", CLOCK_BOOTTIME },
        { "MONOTONIC_RAW", CLOCK_MONOTONIC_RAW },
        { "PROCESS_CPU", CLOCK_PROCESS_CPUTIME_ID },
        { "THREAD_CPU", CLOCK_THREAD_CPUTIME_ID }
    };

    for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        struct timespec now, res;
        if (clock_gettime(clocks[i].id, &now) == -1 ||
            clock_getres(clocks[i].id, &res) == -1) {
            perror(clocks[i].name);
            return EXIT_FAILURE;
        }
        printf("%-13s now=%lld.%09ld res=%lld.%09ld\n",
               clocks[i].name, (long long)now.tv_sec, now.tv_nsec,
               (long long)res.tv_sec, res.tv_nsec);
    }
    return EXIT_SUCCESS;
}
