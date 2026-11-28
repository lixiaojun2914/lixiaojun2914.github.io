/*
 * conc-lab.c —— 《Linux 并发编程》实验程序
 *
 *   threads N            创建 N 个线程，打印 tid、栈范围与栈段实际驻留
 *   stack KB             用指定大小的线程栈，触达其中一部分后观察驻留
 *   counter MODE T I     MODE: plain | mutex | seq | relaxed
 *   share PAD T I        PAD: pack（两个计数器相邻）| pad（各占一条 cacheline）
 *   ring MODE N          MODE: lock（mutex+cond）| atomic（SPSC 无锁）
 *   pool TASKS WORKERS   WORKERS=0 表示每个任务新建一个线程
 *   switch KIND N        KIND: ucontext | spin | sem
 *   aff MODE N           MODE: split（两个线程分核）| same（同核）
 *   litmus KIND MODE N   KIND: sb | mp；MODE: relaxed | seq
 *
 * 构建：gcc -O2 -pthread -Wall -Wextra conc-lab.c -o conc-lab
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

static long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* 读 /proc/self/smaps，累加落在 [lo, hi) 内的 Rss（单位 kB） */
static long rss_in_range(unsigned long lo, unsigned long hi)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char line[512];
	unsigned long a = 0, b = 0;
	long total = 0, rss = 0;
	int in = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
			in = (a < hi && b > lo);
			rss = 0;
		} else if (in && sscanf(line, "Rss: %ld kB", &rss) == 1) {
			total += rss;
		}
	}
	fclose(f);
	return total;
}

static void stack_range(void **base, size_t *size)
{
	pthread_attr_t attr;

	pthread_getattr_np(pthread_self(), &attr);
	pthread_attr_getstack(&attr, base, size);
	pthread_attr_destroy(&attr);
}

/* ---------------- threads / stack ---------------- */

struct winfo {
	pthread_t th;
	pid_t tid;
	void *base;
	size_t size;
};

static void *winfo_fn(void *arg)
{
	struct winfo *w = arg;

	w->tid = (pid_t)syscall(SYS_gettid);
	stack_range(&w->base, &w->size);
	return NULL;
}

static void do_threads(int n)
{
	struct winfo *w = calloc(n, sizeof(*w));
	void *base;
	size_t size;
	int i;

	stack_range(&base, &size);
	printf("main tid=%d stack=[%p,%p) size=%zu kB rss=%ld kB\n",
	       (int)syscall(SYS_gettid), base, (char *)base + size,
	       size / 1024, rss_in_range((unsigned long)base, (unsigned long)base + size));

	for (i = 0; i < n; i++)
		pthread_create(&w[i].th, NULL, winfo_fn, &w[i]);
	for (i = 0; i < n; i++) {
		pthread_join(w[i].th, NULL);
		printf("thread %d tid=%d stack=[%p,%p) size=%zu kB rss=%ld kB\n",
		       i, (int)w[i].tid, w[i].base, (char *)w[i].base + w[i].size,
		       w[i].size / 1024,
		       rss_in_range((unsigned long)w[i].base, (unsigned long)w[i].base + w[i].size));
	}
	free(w);
}

static size_t g_touch;
static size_t g_used;
static void *g_sbase;
static size_t g_ssize;
static volatile int g_ready;

static void *stack_fn(void *arg)
{
	volatile char *p = alloca(g_touch);
	void *base;
	size_t size;

	(void)arg;
	stack_range(&base, &size);
	g_sbase = base;
	g_ssize = size;
	p[0] = 1;
	p[g_touch - 1] = 1;
	g_used = (size_t)((char *)g_sbase + g_ssize - (char *)p);
	__atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
	sleep(4);
	return NULL;
}

static void do_stack(int kb)
{
	pthread_t th;
	pthread_attr_t attr;
	size_t top_to_touch = 0;

	pthread_attr_init(&attr);
	{
		int rc = pthread_attr_setstacksize(&attr, (size_t)kb * 1024);

		printf("setstacksize(%d kB) = %d\n", kb, rc);
	}
	g_touch = (size_t)kb * 1024 / 2;
	pthread_create(&th, &attr, stack_fn, NULL);
	while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
		sched_yield();
	printf("requested=%d kB actual=%zu kB used=%zu kB rss=%ld kB\n",
	       kb, g_ssize / 1024, g_used / 1024,
	       rss_in_range((unsigned long)g_sbase, (unsigned long)g_sbase + g_ssize));
	pthread_join(th, NULL);
}

/* ---------------- counter ---------------- */

static long g_plain;
static _Atomic long g_atomic;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static long g_iters;
static int g_mode;			/* 0 plain 1 mutex 2 seq 3 relaxed */

static void *counter_fn(void *arg)
{
	long n = g_iters;
	long i;

	(void)arg;
	switch (g_mode) {
	case 0:
		for (i = 0; i < n; i++)
			g_plain++;
		break;
	case 1:
		for (i = 0; i < n; i++) {
			pthread_mutex_lock(&g_mu);
			g_plain++;
			pthread_mutex_unlock(&g_mu);
		}
		break;
	case 2:
		for (i = 0; i < n; i++)
			atomic_fetch_add_explicit(&g_atomic, 1, memory_order_seq_cst);
		break;
	default:
		for (i = 0; i < n; i++)
			atomic_fetch_add_explicit(&g_atomic, 1, memory_order_relaxed);
		break;
	}
	return NULL;
}

static void do_counter(const char *mode, int nthreads, long iters)
{
	static const char *names[] = { "plain", "mutex", "seq", "relaxed" };
	pthread_t *th = calloc(nthreads, sizeof(*th));
	long t0, dt;
	int i;

	g_iters = iters;
	for (g_mode = 0; g_mode < 4; g_mode++)
		if (!strcmp(mode, names[g_mode]))
			break;
	if (g_mode == 4) {
		printf("unknown mode %s\n", mode);
		exit(1);
	}

	t0 = now_ns();
	for (i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, counter_fn, NULL);
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	dt = now_ns() - t0;

	printf("%s threads=%d iters=%ld total=%ld ns/op=%.2f result=%ld\n",
	       mode, nthreads, iters, dt,
	       (double)dt / ((double)nthreads * (double)iters),
	       g_mode <= 1 ? g_plain : (long)g_atomic);
	free(th);
}

/* ---------------- false sharing ---------------- */

#define PAD 64
struct packed {
	_Atomic long a;
	_Atomic long b;
};
struct padded {
	_Atomic long a;
	char pad[PAD];
	_Atomic long b;
	char pad2[PAD];
};

static struct packed p_pack;
static struct padded p_pad;
static long g_share_iters;
static int g_use_pad;

static void *share_fn(void *arg)
{
	long n = g_share_iters;
	int which = (int)(intptr_t)arg;
	_Atomic long *v = g_use_pad ? (which ? &p_pad.b : &p_pad.a)
				    : (which ? &p_pack.b : &p_pack.a);
	long i;

	for (i = 0; i < n; i++)
		atomic_fetch_add_explicit(v, 1, memory_order_relaxed);
	return NULL;
}

static void do_share(const char *mode, int nthreads, long iters)
{
	pthread_t th[8];
	long t0, dt;
	int i;

	g_use_pad = !strcmp(mode, "pad");
	g_share_iters = iters;
	t0 = now_ns();
	for (i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, share_fn, (void *)(intptr_t)(i & 1));
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	dt = now_ns() - t0;

	printf("%s threads=%d iters=%ld total=%ld ns/op=%.2f\n", mode, nthreads,
	       iters, dt, (double)dt / ((double)nthreads * (double)iters));
}

/* ---------------- SPSC ring ---------------- */

#define RING_CAP 1024
#define RING_MASK (RING_CAP - 1)
static uint64_t rbuf[RING_CAP];
static long rn;

static _Atomic uint32_t r_head;
static _Atomic uint32_t r_tail;
static pthread_mutex_t r_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_notfull = PTHREAD_COND_INITIALIZER;
static pthread_cond_t r_notempty = PTHREAD_COND_INITIALIZER;

static void *ring_prod_lock(void *arg)
{
	uint32_t tail = 0;
	long i;

	(void)arg;
	for (i = 0; i < rn; i++) {
		pthread_mutex_lock(&r_mu);
		while (tail - r_head == RING_CAP)
			pthread_cond_wait(&r_notfull, &r_mu);
		rbuf[tail & RING_MASK] = (uint64_t)i;
		tail++;
		r_tail = tail;
		pthread_cond_signal(&r_notempty);
		pthread_mutex_unlock(&r_mu);
	}
	return NULL;
}

static void *ring_cons_lock(void *arg)
{
	uint32_t head = 0;
	long i;

	(void)arg;
	for (i = 0; i < rn; i++) {
		pthread_mutex_lock(&r_mu);
		while (head == r_tail)
			pthread_cond_wait(&r_notempty, &r_mu);
		if (rbuf[head & RING_MASK] != (uint64_t)i) {
			printf("lock: order broken at %ld\n", i);
			exit(2);
		}
		head++;
		r_head = head;
		pthread_cond_signal(&r_notfull);
		pthread_mutex_unlock(&r_mu);
	}
	return NULL;
}

static void *ring_prod_atomic(void *arg)
{
	uint32_t tail = 0, head;
	long i;

	(void)arg;
	for (i = 0; i < rn; i++) {
		for (;;) {
			head = atomic_load_explicit(&r_head, memory_order_acquire);
			if (tail - head < RING_CAP)
				break;
			sched_yield();
		}
		rbuf[tail & RING_MASK] = (uint64_t)i;
		tail++;
		atomic_store_explicit(&r_tail, tail, memory_order_release);
	}
	return NULL;
}

static void *ring_cons_atomic(void *arg)
{
	uint32_t head = 0, tail;
	long i;

	(void)arg;
	for (i = 0; i < rn; i++) {
		for (;;) {
			tail = atomic_load_explicit(&r_tail, memory_order_acquire);
			if (head != tail)
				break;
			sched_yield();
		}
		if (rbuf[head & RING_MASK] != (uint64_t)i) {
			printf("atomic: order broken at %ld\n", i);
			exit(2);
		}
		head++;
		atomic_store_explicit(&r_head, head, memory_order_release);
	}
	return NULL;
}

static void do_ring(const char *mode, long n)
{
	pthread_t p, c;
	long t0, dt;

	rn = n;
	t0 = now_ns();
	if (!strcmp(mode, "lock")) {
		pthread_create(&p, NULL, ring_prod_lock, NULL);
		pthread_create(&c, NULL, ring_cons_lock, NULL);
	} else {
		pthread_create(&p, NULL, ring_prod_atomic, NULL);
		pthread_create(&c, NULL, ring_cons_atomic, NULL);
	}
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	dt = now_ns() - t0;

	printf("%s n=%ld total=%ld ns/msg=%.2f Mmsg/s=%.2f\n", mode, n, dt,
	       (double)dt / (double)n, (double)n / (double)dt * 1000.0);
}

/* ---------------- thread pool ---------------- */

static volatile long work_sink;

static long burn(long x)
{
	double s = 0;
	int i;

	for (i = 0; i < 2000; i++)
		s += (double)(x + i) * 0.5;
	work_sink += (long)s;
	return (long)s;
}

struct tq {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	long next;
	long total;
	long done;
	int stop;
};

static struct tq q;

static void *worker_fn(void *arg)
{
	(void)arg;
	for (;;) {
		long id;

		pthread_mutex_lock(&q.mu);
		while (!q.stop && q.next >= q.total && q.done < q.total)
			pthread_cond_wait(&q.cv, &q.mu);
		if (q.next >= q.total) {
			pthread_mutex_unlock(&q.mu);
			break;
		}
		id = q.next++;
		pthread_mutex_unlock(&q.mu);

		burn(id);

		pthread_mutex_lock(&q.mu);
		q.done++;
		if (q.done == q.total)
			pthread_cond_broadcast(&q.cv);
		pthread_mutex_unlock(&q.mu);
	}
	return NULL;
}

static void *one_task(void *arg)
{
	long id = *(long *)arg;

	burn(id);
	return NULL;
}

static void do_pool(long tasks, int workers)
{
	long t0, dt, i;

	q.total = tasks;
	q.next = 0;
	q.done = 0;
	q.stop = 0;
	t0 = now_ns();
	if (workers == 0) {
		for (i = 0; i < tasks; i++) {
			pthread_t th;
			long id = i;

			pthread_create(&th, NULL, one_task, &id);
			pthread_join(th, NULL);
		}
	} else {
		pthread_t *th = calloc(workers, sizeof(*th));

		for (i = 0; i < workers; i++)
			pthread_create(&th[i], NULL, worker_fn, NULL);
		for (i = 0; i < workers; i++)
			pthread_join(th[i], NULL);
		free(th);
	}
	dt = now_ns() - t0;

	printf("workers=%d tasks=%ld total=%ld ns/task=%.0f\n", workers, tasks, dt,
	       (double)dt / (double)tasks);
}

/* ---------------- context switch ---------------- */


static ucontext_t uc_main, uc_alt;
static long uc_n;

static void uc_entry(void)
{
	long i;

	for (i = 0; i < uc_n; i++) {
		swapcontext(&uc_alt, &uc_main);
	}
}

static void do_switch_ucontext(long n)
{
	static char stk[64 * 1024];

	uc_n = n;
	getcontext(&uc_alt);
	uc_alt.uc_stack.ss_sp = stk;
	uc_alt.uc_stack.ss_size = sizeof(stk);
	uc_alt.uc_link = NULL;
	makecontext(&uc_alt, uc_entry, 0);

	long t0 = now_ns();
	for (long i = 0; i < n; i++)
		swapcontext(&uc_main, &uc_alt);
	long dt = now_ns() - t0;

	printf("ucontext switches=%ld total=%ld ns/switch=%.1f\n", n * 2, dt,
	       (double)dt / (double)(n * 2));
}

static _Atomic int sw_a, sw_b;
static sem_t sem_a, sem_b;
static int sw_mode;			/* 0 spin 1 sem */
static long sw_n;

static void *sw_fn_b(void *arg)
{
	long i;

	(void)arg;
	for (i = 0; i < sw_n; i++) {
		if (sw_mode == 0) {
			while (!atomic_load_explicit(&sw_a, memory_order_acquire))
				sched_yield();
			atomic_store_explicit(&sw_a, 0, memory_order_release);
			atomic_store_explicit(&sw_b, 1, memory_order_release);
		} else {
			sem_wait(&sem_a);
			sem_post(&sem_b);
		}
	}
	return NULL;
}

static void do_switch_pthread(const char *mode, long n)
{
	pthread_t b;
	long t0, dt, i;

	sw_mode = !strcmp(mode, "sem");
	sw_n = n;
	sem_init(&sem_a, 0, 0);
	sem_init(&sem_b, 0, 0);
	pthread_create(&b, NULL, sw_fn_b, NULL);

	t0 = now_ns();
	for (i = 0; i < n; i++) {
		if (sw_mode == 0) {
			atomic_store_explicit(&sw_a, 1, memory_order_release);
			while (!atomic_load_explicit(&sw_b, memory_order_acquire))
				sched_yield();
			atomic_store_explicit(&sw_b, 0, memory_order_release);
		} else {
			sem_post(&sem_a);
			sem_wait(&sem_b);
		}
	}
	dt = now_ns() - t0;
	pthread_join(b, NULL);

	printf("%s rounds=%ld total=%ld ns/round=%.1f\n", mode, n, dt,
	       (double)dt / (double)n);
}

/* ---------------- litmus: store buffer / publish ---------------- */

static void pin(int cpu);

/* SB：两个核各自写自己的变量，再读对方的变量。双方都读到 0，
 * 说明两个写都还没被对方看到。 */
static pthread_barrier_t sb_bar;
static _Atomic int sb_x __attribute__((aligned(64)));
static _Atomic int sb_y __attribute__((aligned(64)));
static int sb_mode;			/* 0 relaxed 1 seq */
static long sb_n, sb_hits, sb_r1, sb_r2;

static void *sb_fn(void *arg)
{
	int p0 = (int)(intptr_t)arg;
	long i;

	pin(p0);
	for (i = 0; i < sb_n; i++) {
		atomic_store_explicit(&sb_x, 0, memory_order_relaxed);
		atomic_store_explicit(&sb_y, 0, memory_order_relaxed);
		pthread_barrier_wait(&sb_bar);
		if (p0) {
			atomic_store_explicit(&sb_x, 1, sb_mode ? memory_order_seq_cst : memory_order_relaxed);
			sb_r1 = atomic_load_explicit(&sb_y, sb_mode ? memory_order_seq_cst : memory_order_relaxed);
		} else {
			atomic_store_explicit(&sb_y, 1, sb_mode ? memory_order_seq_cst : memory_order_relaxed);
			sb_r2 = atomic_load_explicit(&sb_x, sb_mode ? memory_order_seq_cst : memory_order_relaxed);
		}
		pthread_barrier_wait(&sb_bar);
		if (p0 && sb_r1 == 0 && sb_r2 == 0)
			sb_hits++;
		pthread_barrier_wait(&sb_bar);
	}
	return NULL;
}

/* PUB：生产者先写 data 再写 flag，两者是同一个序号。
 * 消费者看到 flag=i 之后读 data，data != i 就说明它读到了半条消息。 */
struct pub_slot {
	_Atomic long v;
	char pad[56];
};
static struct pub_slot pub_data, pub_flag;
static long pub_n, pub_hits;

static void *pub_prod(void *arg)
{
	long i;

	(void)arg;
	pin(0);
	for (i = 1; i <= pub_n; i++) {
		atomic_store_explicit(&pub_data.v, i,
				      sb_mode ? memory_order_seq_cst : memory_order_relaxed);
		atomic_store_explicit(&pub_flag.v, i,
				      sb_mode ? memory_order_seq_cst : memory_order_relaxed);
	}
	return NULL;
}

static void *pub_cons(void *arg)
{
	(void)arg;
	pin(1);
	for (;;) {
		long f = atomic_load_explicit(&pub_flag.v,
					      sb_mode ? memory_order_seq_cst : memory_order_relaxed);
		long d;

		if (!f)
			continue;
		d = atomic_load_explicit(&pub_data.v,
					 sb_mode ? memory_order_seq_cst : memory_order_relaxed);
		if (d < f)
			pub_hits++;
		if (f == pub_n)
			break;
	}
	return NULL;
}

static void do_litmus(const char *kind, const char *mode, long n)
{
	pthread_t a, b;

	sb_mode = !strcmp(mode, "seq");
	sb_n = n;
	sb_hits = 0;
	pub_n = n;
	pub_hits = 0;

	if (!strcmp(kind, "pub")) {
		pthread_create(&a, NULL, pub_prod, NULL);
		pthread_create(&b, NULL, pub_cons, NULL);
		pthread_join(a, NULL);
		pthread_join(b, NULL);
		printf("pub %s rounds=%ld torn=%ld\n", mode, n, pub_hits);
		return;
	}

	pthread_barrier_init(&sb_bar, NULL, 2);
	pthread_create(&a, NULL, sb_fn, (void *)(intptr_t)1);
	pthread_create(&b, NULL, sb_fn, (void *)(intptr_t)0);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	pthread_barrier_destroy(&sb_bar);
	printf("sb %s rounds=%ld both_zero=%ld\n", mode, n, sb_hits);
}

/* ---------------- affinity ---------------- */

static void pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *pin_prod(void *arg)
{
	int cpu = (int)(intptr_t)arg;

	if (cpu >= 0)
		pin(cpu);
	return ring_prod_atomic(NULL);
}

static void *pin_cons(void *arg)
{
	int cpu = (int)(intptr_t)arg;

	if (cpu >= 0)
		pin(cpu);
	return ring_cons_atomic(NULL);
}

static void do_aff(const char *mode, long n)
{
	pthread_t p, c;
	int pc, cc;
	long t0, dt;

	rn = n;
	if (!strcmp(mode, "split")) {
		pc = 0;
		cc = 1;
	} else {
		pc = 0;
		cc = 0;
	}
	t0 = now_ns();
	pthread_create(&p, NULL, pin_prod, (void *)(intptr_t)pc);
	pthread_create(&c, NULL, pin_cons, (void *)(intptr_t)cc);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	dt = now_ns() - t0;

	printf("%s n=%ld total=%ld ns/msg=%.2f\n", mode, n, dt, (double)dt / (double)n);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s threads|stack|counter|share|ring|pool|switch|aff ...\n", argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "threads") && argc == 3)
		do_threads(atoi(argv[2]));
	else if (!strcmp(argv[1], "stack") && argc == 3)
		do_stack(atoi(argv[2]));
	else if (!strcmp(argv[1], "counter") && argc == 5)
		do_counter(argv[2], atoi(argv[3]), atol(argv[4]));
	else if (!strcmp(argv[1], "share") && argc == 5)
		do_share(argv[2], atoi(argv[3]), atol(argv[4]));
	else if (!strcmp(argv[1], "ring") && argc == 4)
		do_ring(argv[2], atol(argv[3]));
	else if (!strcmp(argv[1], "pool") && argc == 4)
		do_pool(atol(argv[2]), atoi(argv[3]));
	else if (!strcmp(argv[1], "switch") && argc == 4) {
		if (!strcmp(argv[2], "ucontext"))
			do_switch_ucontext(atol(argv[3]));
		else
			do_switch_pthread(argv[2], atol(argv[3]));
	} else if (!strcmp(argv[1], "litmus") && argc == 5) {
		do_litmus(argv[2], argv[3], atol(argv[4]));
	} else if (!strcmp(argv[1], "aff") && argc == 4) {
		do_aff(argv[2], atol(argv[3]));
	} else {
		fprintf(stderr, "bad argument\n");
		return 1;
	}
	return 0;
}
