#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int payload;
static atomic_int ready = ATOMIC_VAR_INIT(0);

static void *writer(void *arg)
{
    (void)arg;
    payload = 42;
    atomic_store_explicit(&ready, 1, memory_order_release);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    int err = pthread_create(&thread, NULL, writer, NULL);
    if (err) {
        fprintf(stderr, "create: %s\n", strerror(err));
        return EXIT_FAILURE;
    }
    while (!atomic_load_explicit(&ready, memory_order_acquire))
        sched_yield();
    printf("ready=1 payload=%d\n", payload);
    err = pthread_join(thread, NULL);
    if (err) {
        fprintf(stderr, "join: %s\n", strerror(err));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
