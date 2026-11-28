/*
 * ipc-lab.c —— 《Linux 并发编程》实验程序
 *
 *   fork N        创建 N 个子进程，打印各自的 pid 与父 pid，父进程回收
 *   threads N     创建 N 个线程，打印 tid，主线程 join
 *   counter MODE  MODE: unsafe | mutex，两个线程各加 1000000 次
 *   cond          一个有界队列，一个生产者、一个消费者，容量 4，共 20 条
 *   sem           3 个许可，6 个线程各占用 200 ms，统计同时在临界区的峰值
 *   pshm          共享内存加进程共享信号量，父子进程各加 100000 次
 *   atomic MODE   MODE: relaxed | seq，两个线程各做 1000000 次原子加
 *   pub           发布 1000 条消息，消费者逐条校验内容
 *
 * 构建：gcc -O2 -pthread -Wall -Wextra ipc-lab.c -o ipc-lab
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int gettid_(void)
{
	return (int)syscall(SYS_gettid);
}

/* ---------------- fork ---------------- */

static void do_fork(int n)
{
	int i;

	printf("parent pid=%d\n", (int)getpid());
	for (i = 0; i < n; i++) {
		pid_t pid;

		fflush(stdout);
		pid = fork();

		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (pid == 0) {
			printf("child %d pid=%d ppid=%d\n", i, (int)getpid(), (int)getppid());
			fflush(stdout);
			_exit(0);
		}
		printf("spawned child %d pid=%d\n", i, (int)pid);
	}
	for (i = 0; i < n; i++) {
		int st;
		pid_t pid = wait(&st);

		printf("reaped pid=%d status=%d\n", (int)pid, WEXITSTATUS(st));
	}
}

/* ---------------- threads ---------------- */

struct targ {
	int idx;
	int tid;
	int pid;
};

static void *thread_fn(void *arg)
{
	struct targ *t = arg;

	t->tid = gettid_();
	t->pid = (int)getpid();
	return NULL;
}

static void do_threads(int n)
{
	struct targ *t = calloc(n, sizeof(*t));
	pthread_t *th = calloc(n, sizeof(*th));
	int i;

	printf("process pid=%d main tid=%d\n", (int)getpid(), gettid_());
	for (i = 0; i < n; i++) {
		t[i].idx = i;
		pthread_create(&th[i], NULL, thread_fn, &t[i]);
	}
	for (i = 0; i < n; i++) {
		pthread_join(th[i], NULL);
		printf("thread %d tid=%d pid=%d\n", i, t[i].tid, t[i].pid);
	}
	free(t);
	free(th);
}

/* ---------------- counter ---------------- */

static volatile long g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_use_mutex;
static const long ITERS = 1000000;

static void *counter_fn(void *arg)
{
	long i;

	(void)arg;
	for (i = 0; i < ITERS; i++) {
		if (g_use_mutex) {
			pthread_mutex_lock(&g_lock);
			g_count++;
			pthread_mutex_unlock(&g_lock);
		} else {
			g_count++;
		}
	}
	return NULL;
}

static void do_counter(const char *mode)
{
	pthread_t a, b;

	g_use_mutex = !strcmp(mode, "mutex");
	pthread_create(&a, NULL, counter_fn, NULL);
	pthread_create(&b, NULL, counter_fn, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	printf("%s result=%ld expected=%ld %s\n", mode, g_count, ITERS * 2,
	       g_count == ITERS * 2 ? "ok" : "LOST");
}

/* ---------------- condition variable ---------------- */

#define QCAP 4
struct queue {
	int buf[QCAP];
	int head, tail, count;
	pthread_mutex_t mu;
	pthread_cond_t not_full;
	pthread_cond_t not_empty;
};

static struct queue q;

static void q_init(struct queue *q)
{
	q->head = q->tail = q->count = 0;
	pthread_mutex_init(&q->mu, NULL);
	pthread_cond_init(&q->not_full, NULL);
	pthread_cond_init(&q->not_empty, NULL);
}

static void q_put(struct queue *q, int v)
{
	pthread_mutex_lock(&q->mu);
	while (q->count == QCAP)
		pthread_cond_wait(&q->not_full, &q->mu);
	q->buf[q->tail] = v;
	q->tail = (q->tail + 1) % QCAP;
	q->count++;
	printf("put %2d  count=%d\n", v, q->count);
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mu);
}

static int q_get(struct queue *q)
{
	int v;

	pthread_mutex_lock(&q->mu);
	while (q->count == 0)
		pthread_cond_wait(&q->not_empty, &q->mu);
	v = q->buf[q->head];
	q->head = (q->head + 1) % QCAP;
	q->count--;
	printf("get %2d  count=%d\n", v, q->count);
	pthread_cond_signal(&q->not_full);
	pthread_mutex_unlock(&q->mu);
	return v;
}

static void *producer(void *arg)
{
	int i;

	(void)arg;
	for (i = 1; i <= 20; i++)
		q_put(&q, i);
	return NULL;
}

static void *consumer(void *arg)
{
	int i, v, sum = 0;

	(void)arg;
	for (i = 0; i < 20; i++) {
		v = q_get(&q);
		sum += v;
	}
	printf("consumer sum=%d\n", sum);
	return NULL;
}

static void do_cond(void)
{
	pthread_t p, c;

	q_init(&q);
	pthread_create(&p, NULL, producer, NULL);
	pthread_create(&c, NULL, consumer, NULL);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
}

/* ---------------- semaphore ---------------- */

static sem_t g_permits;
static double g_t0;
static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inside, g_peak;

static void *sem_worker(void *arg)
{
	int id = (int)(intptr_t)arg;

	sem_wait(&g_permits);
	pthread_mutex_lock(&g_stat_lock);
	g_inside++;
	if (g_inside > g_peak)
		g_peak = g_inside;
	printf("worker %d enter  inside=%d at %.0f ms\n", id, g_inside, now_ms() - g_t0);
	pthread_mutex_unlock(&g_stat_lock);

	usleep(200000);

	pthread_mutex_lock(&g_stat_lock);
	g_inside--;
	printf("worker %d leave  inside=%d at %.0f ms\n", id, g_inside, now_ms() - g_t0);
	pthread_mutex_unlock(&g_stat_lock);
	sem_post(&g_permits);
	return NULL;
}

static void do_sem(void)
{
	pthread_t th[6];
	int i;

	g_t0 = now_ms();
	sem_init(&g_permits, 0, 3);
	for (i = 0; i < 6; i++)
		pthread_create(&th[i], NULL, sem_worker, (void *)(intptr_t)i);
	for (i = 0; i < 6; i++)
		pthread_join(th[i], NULL);
	sem_destroy(&g_permits);
	printf("peak_inside=%d elapsed=%.0f ms\n", g_peak, now_ms() - g_t0);
}

/* ---------------- shared memory + process-shared semaphore ---------------- */

struct shared {
	sem_t lock;
	long counter;
};

static void bump(struct shared *s, long n)
{
	long i;

	for (i = 0; i < n; i++) {
		sem_wait(&s->lock);
		s->counter++;
		sem_post(&s->lock);
	}
}

static void do_pshm(void)
{
	struct shared *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	pid_t pid;

	sem_init(&s->lock, 1, 1);		/* pshared = 1 */
	s->counter = 0;

	pid = fork();
	if (pid == 0) {
		bump(s, 100000);
		_exit(0);
	}
	bump(s, 100000);
	waitpid(pid, NULL, 0);

	printf("counter=%ld expected=200000 %s\n", s->counter,
	       s->counter == 200000 ? "ok" : "LOST");
	sem_destroy(&s->lock);
	munmap(s, sizeof(*s));
}

/* ---------------- atomic ---------------- */

static _Atomic long g_acount;
static int g_aseq;

static void *atomic_fn(void *arg)
{
	long i;

	(void)arg;
	for (i = 0; i < ITERS; i++) {
		if (g_aseq)
			atomic_fetch_add_explicit(&g_acount, 1, memory_order_seq_cst);
		else
			atomic_fetch_add_explicit(&g_acount, 1, memory_order_relaxed);
	}
	return NULL;
}

static void do_atomic(const char *mode)
{
	pthread_t a, b;

	g_aseq = !strcmp(mode, "seq");
	pthread_create(&a, NULL, atomic_fn, NULL);
	pthread_create(&b, NULL, atomic_fn, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	printf("atomic %s result=%ld expected=%ld %s\n", mode,
	       (long)g_acount, ITERS * 2,
	       (long)g_acount == ITERS * 2 ? "ok" : "LOST");
}

/* ---------------- publish ---------------- */

#define PCAP 1024

struct pubmsg {
	long payload[8];
};

static _Atomic long g_seq;
static struct pubmsg g_ring[PCAP];	/* 1000 条，不超过容量，不会被覆盖 */
static long g_bad;

static void *pub_producer(void *arg)
{
	long i, j;

	(void)arg;
	for (i = 1; i <= 1000; i++) {
		for (j = 0; j < 8; j++)
			g_ring[(i - 1) & (PCAP - 1)].payload[j] = i * 10 + j;
		/* 数据写完，再用 release 发布序号 */
		atomic_store_explicit(&g_seq, i, memory_order_release);
	}
	return NULL;
}

static void *pub_consumer(void *arg)
{
	long i, j, seq;

	(void)arg;
	for (i = 1; i <= 1000; i++) {
		/* acquire 读到序号之后，才能读数据 */
		do {
			seq = atomic_load_explicit(&g_seq, memory_order_acquire);
		} while (seq < i);
		for (j = 0; j < 8; j++)
			if (g_ring[(i - 1) & (PCAP - 1)].payload[j] != i * 10 + j)
				g_bad++;
	}
	return NULL;
}

static void do_pub(void)
{
	pthread_t p, c;

	pthread_create(&p, NULL, pub_producer, NULL);
	pthread_create(&c, NULL, pub_consumer, NULL);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	printf("messages=1000 bad=%ld %s\n", g_bad, g_bad == 0 ? "ok" : "TORN");
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s fork|threads|counter|cond|sem|pshm ...\n", argv[0]);
		return 1;
	}
	if (!strcmp(argv[1], "fork") && argc == 3)
		do_fork(atoi(argv[2]));
	else if (!strcmp(argv[1], "threads") && argc == 3)
		do_threads(atoi(argv[2]));
	else if (!strcmp(argv[1], "counter") && argc == 3)
		do_counter(argv[2]);
	else if (!strcmp(argv[1], "cond"))
		do_cond();
	else if (!strcmp(argv[1], "sem"))
		do_sem();
	else if (!strcmp(argv[1], "pshm"))
		do_pshm();
	else if (!strcmp(argv[1], "atomic") && argc == 3)
		do_atomic(argv[2]);
	else if (!strcmp(argv[1], "pub"))
		do_pub();
	else {
		fprintf(stderr, "bad argument\n");
		return 1;
	}
	return 0;
}
