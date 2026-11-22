#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int full, value, done;
static int total;

static void check(int err, const char *what)
{
    if (err) {
        fprintf(stderr, "%s: %s\n", what, strerror(err));
        exit(EXIT_FAILURE);
    }
}

static void *consumer(void *arg)
{
    (void)arg;
    check(pthread_mutex_lock(&lock), "lock");
    for (;;) {
        while (!full && !done)
            check(pthread_cond_wait(&changed, &lock), "wait");
        if (!full && done)
            break;
        total += value;
        printf("consume=%d\n", value);
        full = 0;
        check(pthread_cond_signal(&changed), "signal");
    }
    check(pthread_mutex_unlock(&lock), "unlock");
    return NULL;
}

int main(void)
{
    pthread_t thread;
    check(pthread_create(&thread, NULL, consumer, NULL), "create");
    for (int i = 1; i <= 3; i++) {
        check(pthread_mutex_lock(&lock), "lock");
        while (full)
            check(pthread_cond_wait(&changed, &lock), "wait");
        value = i;
        full = 1;
        check(pthread_cond_signal(&changed), "signal");
        check(pthread_mutex_unlock(&lock), "unlock");
    }
    check(pthread_mutex_lock(&lock), "lock");
    done = 1;
    check(pthread_cond_signal(&changed), "signal");
    check(pthread_mutex_unlock(&lock), "unlock");
    check(pthread_join(thread, NULL), "join");
    printf("total=%d\n", total);
    check(pthread_cond_destroy(&changed), "destroy cond");
    check(pthread_mutex_destroy(&lock), "destroy mutex");
    return EXIT_SUCCESS;
}
