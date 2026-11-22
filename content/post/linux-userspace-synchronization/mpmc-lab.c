#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY 3
#define PRODUCERS 2
#define CONSUMERS 2
#define PER_PRODUCER 3
#define TOTAL (PRODUCERS * PER_PRODUCER)

static int queue[CAPACITY];
static size_t head, tail;
static sem_t empty_slots, available_items;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int produced[TOTAL], consumed[TOTAL];
static int produced_count, consumed_count, sum, duplicates;
static int consumer_count[CONSUMERS];
static unsigned int seen;

static void check(int err, const char *what)
{
    if (err) {
        fprintf(stderr, "%s: %s\n", what, strerror(err));
        exit(EXIT_FAILURE);
    }
}

static void take_token(sem_t *sem)
{
    int ret;
    do { ret = sem_wait(sem); } while (ret == -1 && errno == EINTR);
    if (ret == -1) { perror("sem_wait"); exit(EXIT_FAILURE); }
}

static void give_token(sem_t *sem)
{
    if (sem_post(sem) == -1) { perror("sem_post"); exit(EXIT_FAILURE); }
}

static void put(int value)
{
    take_token(&empty_slots);
    check(pthread_mutex_lock(&queue_lock), "lock");
    queue[tail] = value;
    tail = (tail + 1) % CAPACITY;
    if (value != 0)
        produced[produced_count++] = value;
    check(pthread_mutex_unlock(&queue_lock), "unlock");
    give_token(&available_items);
}

static int get(int consumer_id)
{
    take_token(&available_items);
    check(pthread_mutex_lock(&queue_lock), "lock");
    int value = queue[head];
    head = (head + 1) % CAPACITY;
    if (value != 0) {
        if (value < 1 || value > TOTAL || consumed_count >= TOTAL) {
            fprintf(stderr, "invalid queue value\n");
            exit(EXIT_FAILURE);
        }
        unsigned int bit = 1u << (value - 1);
        if (seen & bit) duplicates++;
        seen |= bit;
        consumed[consumed_count++] = value;
        consumer_count[consumer_id]++;
        sum += value;
    }
    check(pthread_mutex_unlock(&queue_lock), "unlock");
    give_token(&empty_slots);
    return value;
}

static void *producer(void *arg)
{
    int id = *(int *)arg;
    for (int i = 1; i <= PER_PRODUCER; i++)
        put(id * PER_PRODUCER + i);
    return NULL;
}

static void *consumer(void *arg)
{
    int id = *(int *)arg;
    while (get(id) != 0) { }
    return NULL;
}

int main(void)
{
    if (sem_init(&empty_slots, 0, CAPACITY) == -1 ||
        sem_init(&available_items, 0, 0) == -1) {
        perror("sem_init");
        return EXIT_FAILURE;
    }
    pthread_t prod[PRODUCERS], cons[CONSUMERS];
    int prod_ids[PRODUCERS], cons_ids[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        cons_ids[i] = i;
        check(pthread_create(&cons[i], NULL, consumer, &cons_ids[i]), "create");
    }
    for (int i = 0; i < PRODUCERS; i++) {
        prod_ids[i] = i;
        check(pthread_create(&prod[i], NULL, producer, &prod_ids[i]), "create");
    }
    for (int i = 0; i < PRODUCERS; i++)
        check(pthread_join(prod[i], NULL), "join producer");
    for (int i = 0; i < CONSUMERS; i++) put(0);
    for (int i = 0; i < CONSUMERS; i++)
        check(pthread_join(cons[i], NULL), "join consumer");

    printf("producers=%d consumers=%d capacity=%d\n", PRODUCERS, CONSUMERS, CAPACITY);
    printf("enqueue:");
    for (int i = 0; i < produced_count; i++) printf(" %d", produced[i]);
    printf("\ndequeue:");
    for (int i = 0; i < consumed_count; i++) printf(" %d", consumed[i]);
    printf("\nproduced=%d consumed=%d sum=%d duplicates=%d\n",
           produced_count, consumed_count, sum, duplicates);
    printf("consumer0=%d consumer1=%d\n", consumer_count[0], consumer_count[1]);
    int good = produced_count == TOTAL && consumed_count == TOTAL &&
               sum == TOTAL * (TOTAL + 1) / 2 && duplicates == 0 &&
               seen == (1u << TOTAL) - 1 &&
               memcmp(produced, consumed, sizeof(produced)) == 0;
    printf("verified=%s\n", good ? "yes" : "no");
    if (sem_destroy(&empty_slots) == -1 || sem_destroy(&available_items) == -1) {
        perror("sem_destroy");
        return EXIT_FAILURE;
    }
    check(pthread_mutex_destroy(&queue_lock), "destroy");
    return good ? EXIT_SUCCESS : EXIT_FAILURE;
}
