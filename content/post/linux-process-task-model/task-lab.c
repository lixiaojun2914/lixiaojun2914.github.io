#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int shared_value = 42;
static int channel[2];

static void check(int err, const char *what)
{
    if (err) {
        fprintf(stderr, "%s: %s\n", what, strerror(err));
        exit(EXIT_FAILURE);
    }
}

static void *worker(void *arg)
{
    int local = 2;
    char token;
    (void)arg;
    check(pthread_setname_np(pthread_self(), "worker-demo"), "setname");
    printf("worker pid=%ld tid=%ld shared=%p local=%p\n",
           (long)getpid(), syscall(SYS_gettid),
           (void *)&shared_value, (void *)&local);
    ssize_t n;
    do {
        n = read(channel[0], &token, 1);
    } while (n == -1 && errno == EINTR);
    if (n == -1) {
        perror("read");
        exit(EXIT_FAILURE);
    }
    return NULL;
}

int main(void)
{
    pthread_t thread;
    sigset_t signals;
    int local = 1, received;
    setbuf(stdout, NULL);
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    check(pthread_sigmask(SIG_BLOCK, &signals, NULL), "sigmask");
    if (pipe(channel) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    printf("main   pid=%ld tid=%ld shared=%p local=%p\n",
           (long)getpid(), syscall(SYS_gettid),
           (void *)&shared_value, (void *)&local);
    check(pthread_create(&thread, NULL, worker, NULL), "create");
    check(sigwait(&signals, &received), "sigwait");
    if (write(channel[1], "x", 1) != 1) {
        perror("write");
        return EXIT_FAILURE;
    }
    check(pthread_join(thread, NULL), "join");
    close(channel[0]);
    close(channel[1]);
    return EXIT_SUCCESS;
}
