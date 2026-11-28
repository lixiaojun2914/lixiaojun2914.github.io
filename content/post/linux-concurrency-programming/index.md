---
title: "Linux 并发编程：进程、线程与同步接口的用法"
slug: linux-concurrency-programming
date: "2026-11-28T18:30:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "fork、pthread、mutex、条件变量、信号量、stdatomic 各自怎么调用，返回值怎么判，常见的错法长什么样。"
categories:
    - Linux 与系统编程
tags:
    - Linux
    - 并发
---

这一篇讲接口：Linux 给 C 程序提供了哪几组创建并发、协调并发的函数，每个函数的参数和返回值是什么意思，正确写法和常见错法分别长什么样。

范围是应用层能直接调用的东西：`fork`/`exec`/`wait`、`pthread_create`/`join`、`pthread_mutex`、读写锁与自旋锁、`pthread_cond`、`sem_t`、C11 的 `stdatomic.h`，最后是进程之间怎样共用一把锁。

这些接口在内核里怎么实现不在这里展开。[《Linux 用户态同步》](/p/linux-userspace-synchronization/)讲 mutex 的三态与 futex 怎样把等待交给内核，需要时去那里看。本篇只关心怎么把它们用对。

程序是 [`ipc-lab.c`](/p/linux-concurrency-programming/ipc-lab.c)，一个文件带八个模式，每个模式对应本篇一节：

```bash
wget -q https://lixiaojun2914.github.io/p/linux-concurrency-programming/ipc-lab.c
gcc -O2 -pthread -Wall -Wextra ipc-lab.c -o ipc-lab
```

运行环境是 Ubuntu 26.04、内核 7.0.0-44、aarch64、2 vCPU。输出保存在同目录的 `ipc-*.txt` 里。

## 一、进程：fork、exec、wait

### 1.1 fork 返回两次

```c
#include <unistd.h>

pid_t fork(void);
```

`fork` 调用一次，返回两次：父进程里返回新子进程的 pid，子进程里返回 0，失败返回 -1 并设置 `errno`。所以判断固定是这三个分支：

```c
pid_t pid = fork();

if (pid < 0) {
        perror("fork");
        return -1;
}
if (pid == 0) {
        /* 只有子进程执行这里 */
        _exit(0);
}
/* 父进程走到这里，pid 是子进程的号 */
```

`./ipc-lab fork 2` 的输出（[原始记录](/p/linux-concurrency-programming/ipc-fork.txt)）：

```
parent pid=81487
spawned child 0 pid=81488
child 0 pid=81488 ppid=81487
child 1 pid=81489 ppid=81487
spawned child 1 pid=81489
reaped pid=81488 status=0
reaped pid=81489 status=0
```

父子的打印交错出现，因为它们是两个独立的执行流，谁先跑由调度器决定，不是代码顺序。

### 1.2 子进程拿到的是副本

`fork` 之后子进程有自己的地址空间，内容是 fork 那一刻的拷贝（底层按页写时复制，语义就是副本）。父进程的全局变量、堆上的数据，子进程改了父进程看不到，反过来也一样。要共享必须显式映射共享内存，见第八节。

继承下来的是：打开的文件描述符、当前工作目录、信号处理设置、用户和组。文件描述符是共享的——父子同时往同一个 fd 写，输出会交错。

### 1.3 一个坑：stdio 缓冲被复制

标准输出接管道或文件时是全缓冲的。fork 之前 `printf` 过但没刷新，那段内容留在缓冲区里，随地址空间一起被复制进子进程。子进程退出时刷新一遍，父进程再刷一遍，同一行就打印两次。

第一次跑这个程序时看到的就是这样：

```
parent pid=81346
parent pid=81346
child 0 pid=81347 ppid=81346
```

修法是 fork 之前 `fflush(stdout)`：

```c
fflush(stdout);
pid = fork();
```

子进程结束时用 `_exit()` 而不是 `exit()`：`_exit` 不刷 stdio 缓冲、不执行 `atexit` 注册的函数，避免把父进程的缓冲再刷一遍。

### 1.4 exec 换掉整个映像

```c
#include <unistd.h>

int execve(const char *path, char *const argv[], char *const envp[]);
```

`execve` 用新程序替换当前进程的代码、数据、堆和栈，pid 不变，成功时不返回，失败返回 -1。它通常和 fork 配对：子进程 fork 出来立刻 exec，父进程继续做自己的事。

```c
if (pid == 0) {
        char *argv[] = { "ls", "-l", NULL };
        execvp("ls", argv);
        _exit(127);          /* 只有 exec 失败才会到这里 */
}
```

exec 失败后一定要退出。漏了这行，子进程会继续往下执行父进程的代码，程序里出现两份相同的逻辑。

### 1.5 wait 回收子进程

```c
#include <sys/wait.h>

pid_t wait(int *wstatus);
pid_t waitpid(pid_t pid, int *wstatus, int options);
```

子进程结束后，内核保留它的退出状态直到父进程来取，这段时间它是僵尸进程（`Z` 状态）。`wait` 取任意一个子进程，`waitpid` 可以指定 pid。返回值是回收到的子进程 pid，没有子进程可等时返回 -1。

`wstatus` 不是退出码本身，要用宏解：

```c
int st;
pid_t pid = wait(&st);

if (WIFEXITED(st))
        printf("exit code %d\n", WEXITSTATUS(st));
else if (WIFSIGNALED(st))
        printf("killed by %d\n", WTERMSIG(st));
```

不想阻塞时给 `waitpid` 加 `WNOHANG`。不关心退出状态时可以用 `signal(SIGCHLD, SIG_IGN)` 让内核自动回收，代价是拿不到退出码。

## 二、线程：pthread_create、join、detach

### 2.1 创建

```c
#include <pthread.h>

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg);
```

四个参数：传出线程号、线程属性（传 `NULL` 用默认）、入口函数、传给入口函数的指针。返回 0 表示成功，失败返回错误码——**不是设置 `errno`**，所以不能用 `perror` 打，要用 `strerror(rc)`。

```c
static void *worker(void *arg)
{
        long id = (long)arg;

        printf("thread %ld started\n", id);
        return NULL;               /* 返回值可以被 join 取到 */
}
```

`./ipc-lab threads 3` 的输出（[记录](/p/linux-concurrency-programming/ipc-threads.txt)）：

```
process pid=81349 main tid=81349
thread 0 tid=81350 pid=81349
thread 1 tid=81351 pid=81349
thread 2 tid=81352 pid=81349
```

tid 各不相同，pid 都是同一个：线程是同一进程里的执行流，共享地址空间。

### 2.2 join 与 detach

```c
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
```

`pthread_join` 等线程结束并回收它的资源，`retval` 接住入口函数的返回值。既不 join 也不 detach 的线程结束后，栈和线程描述符不会释放，和僵尸进程类似。

`pthread_detach` 表示"不用等我"，线程结束后资源自动回收。两者只能选一个，detach 之后不能再 join。

```c
pthread_t th;

pthread_create(&th, NULL, worker, (void *)1L);
pthread_join(th, NULL);
```

### 2.3 参数不要传栈上变量的地址

第 4 个参数是 `void *`，最常见的错法是传循环变量地址：

```c
for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, worker, &i);   /* 错 */
```

四个线程拿到同一个地址，等它们真正开始跑，`i` 可能已经是 4，也可能被改了好几次。要么每个线程一份独立数据，要么把整数直接塞进指针：

```c
pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
/* 线程里 */
long id = (long)(intptr_t)arg;
```

传结构体也一样：结构体要在线程结束前一直有效，不能是某个函数里的局部变量。

### 2.4 线程共享什么

![进程与线程各自拥有什么](/p/linux-concurrency-programming/address-space.svg)

线程共享代码段、全局数据、堆和打开的文件描述符，各自只有独立的栈和寄存器。由此有三条推论：

- 一个线程改全局变量，别的线程立刻能看到——这也是必须加锁的原因；
- 一个线程在栈上放的变量，别的线程拿不到，除非把地址传出去；
- 一个线程调用 `exit()`，整个进程结束，所有线程一起退出。线程只想结束自己，用 `return` 或 `pthread_exit()`。

### 2.5 pthread 底下是 clone

`pthread_create` 不是独立的内核接口，它最后调 `clone` 或 `clone3`。`fork` 和 clone 的区别只有一点：fork 全部复制，clone 用 flags 逐项选择共享什么。glibc 的 `fork` 本身就是 `clone(SIGCHLD)`。

```c
#define _GNU_SOURCE
#include <sched.h>

int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...);
int clone3(struct clone_args *args, size_t size);
```

`clone3` 把参数收进一个结构体，避开老 `clone` 变参传 pid/tls 指针的写法，glibc 2.34 以后新建线程走的是它。用 `strace` 看 `pthread_create` 到底发了什么（[记录](/p/linux-concurrency-programming/ipc-clone.txt)）：

```
clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM
        |CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID,
        exit_signal=0, stack=0x..., stack_size=0x80ea40, tls=0x...}) = 82365
```

这一串 flags 就是"线程共享什么"的逐项答案：

| flag | 效果 | pthread 是否设置 |
| --- | --- | --- |
| `CLONE_VM` | 共享地址空间 | 是 |
| `CLONE_FS` | 共享工作目录与 umask | 是 |
| `CLONE_FILES` | 共享文件描述符表 | 是 |
| `CLONE_SIGHAND` | 共享信号处理设置 | 是 |
| `CLONE_THREAD` | 放进同一线程组，pid 相同、tid 不同 | 是 |
| `CLONE_SYSVSEM` | 共享 System V 信号量的撤销列表 | 是 |
| `CLONE_SETTLS` | 设置线程本地存储 | 是 |
| `CLONE_PARENT_SETTID` | 把 tid 写回调用者给的地址 | 是 |
| `CLONE_CHILD_CLEARTID` | 线程退出时清零该地址并唤醒等待者 | 是 |

最后一位是 `pthread_join` 的基础：线程退出时内核把 tid 清零，并在那个地址上做一次 futex 唤醒，join 就是睡在这里等它。`exit_signal=0` 表示结束后不给父进程发信号——线程不是独立进程，不该被 `wait` 回收。

`stack_size=0x80ea40` 约 8 MB，是 glibc 的默认线程栈，由它自己 `mmap` 出来的，多出来的一点是保护页。这也说明 `clone` 的栈由调用者提供，不是内核给的。

实际写法上不要手写 clone 起线程：

- 需要自己准备栈，还要自己处理 TLS，`errno` 会失效；
- `malloc`、`printf`、动态链接器都不是为这种半初始化状态准备的；
- 出错时的现象很难查，可能只是偶发的崩溃。

`clone` 真正的用处是加 `CLONE_NEW*` 做命名空间隔离，那是容器的话题。理解它的 flags 的价值在于能解释现象：一个线程越界写会毁掉整个进程，因为 `CLONE_VM`；一个线程 `exit()` 会让全部退出，因为 `CLONE_THREAD`。

## 三、互斥锁：pthread_mutex_t

### 3.1 初始化

按锁的生命周期选两种方式之一：

```c
/* 静态分配：全局变量或 static，用宏 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* 动态分配：堆上或结构体成员，用 init/destroy */
pthread_mutex_t lock;
pthread_mutex_init(&lock, NULL);       /* 第二个参数是属性，NULL 为默认 */
...
pthread_mutex_destroy(&lock);
```

`PTHREAD_MUTEX_INITIALIZER` 只能用于静态分配的锁。堆上或结构体里的锁必须 `init`，用完 `destroy`。

### 3.2 加锁与解锁

```c
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
```

- `lock`：拿不到就睡，直到别人解锁；
- `trylock`：拿不到立刻返回 `EBUSY`，不睡；
- `unlock`：释放，必须由持锁的线程调用。

返回 0 成功，非 0 是错误码。

### 3.3 不加锁会丢多少

两个线程各把同一个变量加一百万次，期望两百万：

```c
for (i = 0; i < 1000000; i++)
        g_count++;                  /* 读、加、写三步，不是原子操作 */
```

`./ipc-lab counter unsafe` 连跑三次（[记录](/p/linux-concurrency-programming/ipc-counter.txt)）：

```
unsafe result=1000000 expected=2000000 LOST
unsafe result=1002209 expected=2000000 LOST
unsafe result=1606440 expected=2000000 LOST
mutex  result=2000000 expected=2000000 ok
```

三次丢的量都不一样，最差丢了一半。`g_count++` 编译出来是"读进寄存器、加一、写回"三条指令，两个线程的这三步可以任意交错，后写的覆盖先写的。

加锁版本：

```c
pthread_mutex_lock(&lock);
g_count++;
pthread_mutex_unlock(&lock);
```

结果稳定是两百万。

### 3.4 三条纪律

- **谁加谁解**，并且每条返回路径都要解。函数中途 `return` 最容易漏，可以只在末尾返回，或者用 `goto out` 统一收尾。
- **临界区要短**。锁里不做 I/O、不 `sleep`、不等另一个线程。持锁越久，别人等得越久。
- **锁的顺序要固定**。两把锁 A、B，一个线程先 A 后 B，另一个先 B 后 A，就可能互等到死锁。全程序统一一个顺序，比如按编号或按地址。

## 四、读写锁与自旋锁

### 4.1 读写锁：读多写少

```c
#include <pthread.h>

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);
```

读锁可以多个线程同时持有，写锁独占。适合配置表、路由表这类读远多于写的数据。

要注意写者饥饿：读请求一直不断时，写者可能长时间拿不到锁。glibc 的默认实现会让等待中的写者优先，但这是实现行为，不能当契约依赖。

### 4.2 自旋锁：极短临界区

```c
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);
```

拿不到锁就原地循环，不睡眠。只适合临界区只有几条指令、且确定不会阻塞的场景；临界区里做任何可能阻塞的事（I/O、`malloc`、`printf`）都是在白烧 CPU。

用户态程序里它的用处不多，普通 mutex 的快路径已经足够快。

### 4.3 怎么选

| 场景 | 选择 |
| --- | --- |
| 普通互斥 | `pthread_mutex_t` |
| 读远多于写 | `pthread_rwlock_t` |
| 临界区几条指令，确定不阻塞 | `pthread_spinlock_t` |
| 跨进程互斥 | mutex 设 `PTHREAD_PROCESS_SHARED`，见第八节 |

## 五、条件变量：pthread_cond_t

### 5.1 三个接口，必须配一把锁

```c
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
```

- `wait`：原子地释放 mutex 并睡眠，被唤醒后重新拿到 mutex 才返回；
- `signal`：唤醒一个等待者；
- `broadcast`：唤醒全部。

`wait` 的第二个参数是保护"条件"的那把锁，不能换成别的锁，也不能传 `NULL`。条件变量第一次用之前要 `pthread_cond_init`，静态的可以用 `PTHREAD_COND_INITIALIZER`。

### 5.2 判断必须用 while

```c
pthread_mutex_lock(&mu);
while (count == 0)                       /* 必须是 while，不是 if */
        pthread_cond_wait(&not_empty, &mu);
v = buf[head];
pthread_mutex_unlock(&mu);
```

两个原因：

1. 从被唤醒到真正拿到锁之间有一段时间，条件可能被别人改回去了；
2. 条件变量允许虚假唤醒，没有 signal 也可能返回。

用 `if` 只查一次，就可能在没数据时往下走。

### 5.3 生产者消费者

![条件变量在有界队列上的等待与唤醒](/p/linux-concurrency-programming/prodcons.svg)

一个有界队列，容量 4，生产者放 20 条，消费者取 20 条：

```c
#define QCAP 4

static void q_put(struct queue *q, int v)
{
        pthread_mutex_lock(&q->mu);
        while (q->count == QCAP)                    /* 满了等 */
                pthread_cond_wait(&q->not_full, &q->mu);
        q->buf[q->tail] = v;
        q->tail = (q->tail + 1) % QCAP;
        q->count++;
        pthread_cond_signal(&q->not_empty);         /* 叫醒消费者 */
        pthread_mutex_unlock(&q->mu);
}

static int q_get(struct queue *q)
{
        int v;

        pthread_mutex_lock(&q->mu);
        while (q->count == 0)                       /* 空了等 */
                pthread_cond_wait(&q->not_empty, &q->mu);
        v = q->buf[q->head];
        q->head = (q->head + 1) % QCAP;
        q->count--;
        pthread_cond_signal(&q->not_full);          /* 叫醒生产者 */
        pthread_mutex_unlock(&q->mu);
        return v;
}
```

每个 `wait` 前面都有一把锁保护 `count`，队列的头尾下标也在这把锁里，所以队列本身不需要再多的同步。

`./ipc-lab cond` 的开头几行（[完整记录](/p/linux-concurrency-programming/ipc-cond.txt)）：

```
put  1  count=1
put  2  count=2
put  3  count=3
put  4  count=4
get  1  count=3
get  2  count=2
...
get 20  count=0
consumer sum=210
```

count 到 4 就不再增加，说明生产者确实停在 `not_full` 上。消费者取走一条、count 降到 3，生产者才被唤醒继续放。最后 `sum=210` 是 1 到 20 的和，说明 20 条一条没漏、顺序也对。

### 5.4 signal 还是 broadcast

- 只有一个等待者，或每次只需要一个人干活：用 `signal`；
- 等待者等的条件不同，或条件变化后所有人都该重查：用 `broadcast`。

队列这种"放一条就够一个人取"的场景用 `signal`。线程池退出、状态切换这种"所有人都要重新判断"的场景用 `broadcast`，并且每个线程醒来都要用 while 重查——广播出去只代表"可能有变化"，不代表对它成立。

## 六、信号量：sem_t

### 6.1 四个接口

```c
#include <semaphore.h>

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_destroy(sem_t *sem);
```

`pshared` 为 0 时用于同一进程内的线程，非 0 时用于进程之间（此时信号量本身必须放在共享内存里）。`value` 是初始可用数量。

- `sem_wait`：数量大于 0 就减一并返回，等于 0 就睡；
- `sem_post`：数量加一，并唤醒一个等待者。

返回 0 成功，-1 失败并设置 `errno`。

### 6.2 限制并发数

信号量最直接的用法是"同时最多几个"。3 个许可、6 个线程各占用 200 ms：

```c
sem_init(&permits, 0, 3);

sem_wait(&permits);
/* 这里最多 3 个线程同时执行 */
usleep(200000);
sem_post(&permits);
```

`./ipc-lab sem` 的输出（[记录](/p/linux-concurrency-programming/ipc-sem.txt)）：

```
worker 0 enter  inside=1 at 0 ms
worker 1 enter  inside=2 at 0 ms
worker 4 enter  inside=3 at 0 ms
worker 0 leave  inside=2 at 201 ms
worker 5 enter  inside=3 at 201 ms
worker 1 leave  inside=2 at 201 ms
worker 2 enter  inside=3 at 201 ms
worker 4 leave  inside=2 at 201 ms
worker 3 enter  inside=3 at 201 ms
...
peak_inside=3 elapsed=401 ms
```

同时在里边的峰值是 3，总耗时约 400 ms：6 个任务、每个 200 ms、3 路并发，正好两轮。第 2、3、5 号线程一开始就等在 `sem_wait` 上，前面有人 `post` 才被放进来。

这里统计 `inside` 的计数器自己用了 mutex，它和信号量是两件事：信号量管"能不能进"，mutex 管"这个统计别算错"。信号量不保护数据，只发放许可。

### 6.3 与条件变量怎么分工

- 条件变量要配一把 mutex，还要自己维护"条件"这个变量，适合"等某个状态成立"；
- 信号量自带计数，适合"资源还剩几个"，不需要额外的状态变量。

队列也能用两个信号量写：一个记空槽位，一个记已放入条数。但头尾下标仍要一把锁保护，两种写法差别不大，选自己看得懂的那种。

## 七、原子操作：stdatomic.h

### 7.1 原子类型与加法

```c
#include <stdatomic.h>

_Atomic long counter;            /* 等价写法：atomic_long counter; */

long v = atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
```

`atomic_fetch_add` 把加一做成一条不可分割的指令，返回加之前的值。不需要锁，也不会丢更新：

```
atomic relaxed result=2000000 expected=2000000 ok
atomic seq     result=2000000 expected=2000000 ok
```

常用的一组是 `atomic_load`、`atomic_store`、`atomic_fetch_add`、`atomic_exchange`、`atomic_compare_exchange_strong`。带 `_explicit` 后缀的版本多一个内存序参数，不带的等价于 `memory_order_seq_cst`。

原子类型不能随便当普通变量用：`_Atomic long` 的大小和对齐可能与 `long` 不同，不能对它的成员取地址传给需要非原子指针的函数，也不要 `memcpy` 进出。

### 7.2 六种内存序

| 内存序 | 含义 | 典型用途 |
| --- | --- | --- |
| `memory_order_relaxed` | 只保证这一条操作原子，不约束前后读写顺序 | 统计计数、序号递增 |
| `memory_order_acquire` | 这条读之后的读写不能挪到它前面 | 读标志、取锁 |
| `memory_order_release` | 这条写之前的读写不能挪到它后面 | 写标志、放锁 |
| `memory_order_acq_rel` | 同时具备两者，用于读改写 | 引用计数减一 |
| `memory_order_seq_cst` | 所有线程看到同一个全局顺序 | 两个变量之间要统一顺序的协议 |
| `memory_order_consume` | 只约束数据依赖，实践中按 acquire 处理 | 几乎不用 |

记法是：`relaxed` 只管自己，`acquire`/`release` 管一边，`seq_cst` 管全局。默认（不带 `_explicit`）是 `seq_cst`，最保险也最贵；能说清为什么用弱序再用弱的。

### 7.3 发布一条消息

一个线程写数据、另一个线程等数据就绪，这是 release/acquire 最典型的用法：

```c
/* 生产者 */
for (j = 0; j < 8; j++)
        ring[i & (PCAP - 1)].payload[j] = i * 10 + j;
atomic_store_explicit(&seq, i, memory_order_release);    /* 先写数据，再发布序号 */

/* 消费者 */
do {
        s = atomic_load_explicit(&seq, memory_order_acquire);   /* 先取序号 */
} while (s < i);
/* 这里读到的 payload 一定是第 i 条写完的内容 */
```

写侧只用 `release`，读侧只用 `acquire`，数据本身不需要是原子类型。`./ipc-lab pub` 发 1000 条、逐条校验 8 个字段（[记录](/p/linux-concurrency-programming/ipc-pub.txt)）：

```
messages=1000 bad=0 ok
```

两块容易写错的地方：

- 数据写完却用 `relaxed` 发布序号：CPU 可以让序号先被看到，消费者拿到半条消息；
- 消费者用 `relaxed` 读序号：编译器和 CPU 可以把数据读提到序号判断之前，读到旧值。

还有一层不属于内存序：缓冲区如果只有一格，生产者放下一条时会覆盖上一条内容，消费者即使拿到正确的序号，读到的也可能是后一条。程序里用 1024 格的环，1000 条不重叠，才保证每条都能校验。单格复用要靠消费者及时跟上，或者开够格数。

### 7.4 什么时候用原子代替锁

- 单个计数、单个标志、单个序号：原子足够，比锁快也不会阻塞；
- 一组数据要一起变：仍然用锁。多个原子变量拼在一起不构成一个原子操作；
- 需要"等某个条件成立"：用条件变量。原子不能让你睡。

## 八、进程之间共享

### 8.1 共享内存

```c
#include <sys/mman.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

父子之间最省事的是匿名共享映射：

```c
struct shared *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
if (s == MAP_FAILED) {
        perror("mmap");
        return -1;
}
```

`MAP_SHARED` 是关键：fork 之后父子看到同一块物理页，一方写另一方立刻能看到。换成 `MAP_PRIVATE` 就退回成副本。

无关进程之间用 `shm_open` 拿一个名字，再 `mmap` 上去，原理相同。

### 8.2 进程共享的锁

共享内存里的 mutex 默认只在本进程的线程间有效。要跨进程，初始化时就得声明：

```c
pthread_mutexattr_t attr;

pthread_mutexattr_init(&attr);
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutex_init(&s->lock, &attr);
pthread_mutexattr_destroy(&attr);
```

信号量更简单，`sem_init` 第二个参数传 1 就是进程共享：

```c
sem_init(&s->lock, 1, 1);
```

`./ipc-lab pshm` 里父子各加十万次，用共享内存加进程共享信号量（[记录](/p/linux-concurrency-programming/ipc-pshm.txt)）：

```
counter=200000 expected=200000 ok
```

两个地方都省了就会出错：锁只在自己进程里有效，对方根本不认，结果是丢更新，而且不会有任何报错。

### 8.3 fork 时带着锁会怎样

`fork` 只复制调用 `fork` 的那个线程，其他线程在子进程里不存在。如果 fork 的瞬间别的线程正持有一把锁，子进程里这把锁是"已加锁"状态，而持锁的线程没了——子进程再去 `lock` 就永远等下去。

所以多线程程序里 fork 要谨慎：

- fork 之前停掉其他线程，或者确保没有线程持锁；
- 子进程里只调 async-signal-safe 的函数，然后尽快 `exec`；
- 确实要在子进程里继续用锁，就在 fork 之后重新 `pthread_mutex_init`，覆盖掉继承来的状态。

## 九、按场景选接口

| 要解决的问题 | 用什么 | 关键点 |
| --- | --- | --- |
| 起一个新程序 | `fork` + `exec` | 子进程 exec 失败要 `_exit` |
| 等子进程结束 | `wait` / `waitpid` | 不回收会留僵尸 |
| 同进程内并行 | `pthread_create` | 参数不能传栈变量地址 |
| 保护一段共享数据 | `pthread_mutex_t` | 谁加谁解，锁序固定 |
| 读多写少 | `pthread_rwlock_t` | 注意写者饥饿 |
| 等某个条件成立 | `pthread_cond_t` | while 重查，配同一把锁 |
| 限制并发数量 | `sem_t` | 跨进程要放共享内存 |
| 单个计数或标志 | `stdatomic.h` | 一组数据仍要锁 |
| 发布一条消息 | release 写 + acquire 读 | 数据本身不必是原子类型 |
| 进程之间共享数据 | `mmap(MAP_SHARED)` | 锁要设 `PTHREAD_PROCESS_SHARED` |

## 十、命令与接口速查

| 目的 | 接口或命令 |
| --- | --- |
| 创建进程 | `pid_t fork(void)`，子进程返回 0，父进程返回子 pid |
| 替换映像 | `int execve(const char *path, char *const argv[], char *const envp[])`，成功不返回 |
| 回收子进程 | `pid_t wait(int *wstatus)`、`pid_t waitpid(pid_t pid, int *wstatus, int options)` |
| 创建线程 | `int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)` |
| 等待线程 | `int pthread_join(pthread_t t, void **retval)` |
| 分离线程 | `int pthread_detach(pthread_t t)` |
| 静态初始化锁 | `pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER` |
| 动态初始化锁 | `pthread_mutex_init(&m, NULL)`、`pthread_mutex_destroy(&m)` |
| 加锁解锁 | `pthread_mutex_lock`、`pthread_mutex_trylock`、`pthread_mutex_unlock` |
| 读写锁 | `pthread_rwlock_rdlock`、`pthread_rwlock_wrlock`、`pthread_rwlock_unlock` |
| 自旋锁 | `pthread_spin_lock`、`pthread_spin_unlock` |
| 条件变量等待 | `int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)` |
| 唤醒 | `pthread_cond_signal`、`pthread_cond_broadcast` |
| 信号量 | `sem_init(&s, pshared, value)`、`sem_wait`、`sem_post`、`sem_destroy` |
| 原子加 | `atomic_fetch_add_explicit(&v, 1, memory_order_relaxed)` |
| 发布与读取 | `atomic_store_explicit(&f, v, memory_order_release)`、`atomic_load_explicit(&f, memory_order_acquire)` |
| 共享内存 | `mmap(NULL, len, PROT_READ\|PROT_WRITE, MAP_SHARED\|MAP_ANONYMOUS, -1, 0)` |
| 进程共享锁属性 | `pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)` |
| 看线程创建的真实调用 | `strace -f -e trace=clone,clone3 ./ipc-lab threads 2` |
| 看线程 | `ps -eLf`、`top -H -p <pid>` |
| 看锁与唤醒 | `strace -f -e trace=futex ./ipc-lab cond` |
| 看进程状态 | `ps -eo pid,ppid,stat,comm` |

## 十一、名词解释

- **僵尸进程**：已结束但父进程还没 `wait` 的进程，只留退出状态。
- **写时复制**：`fork` 后父子共享物理页，任一方写时才复制一份。
- **tid**：线程号，同一进程的线程 pid 相同、tid 不同。
- **临界区**：被锁保护、同一时刻只允许一个线程执行的那段代码。
- **死锁**：两个以上线程各持一把锁、又都在等对方释放，谁也无法继续。
- **虚假唤醒**：条件变量没有 signal 也可能返回，所以判断条件要用 while。
- **内存序**：编译器和 CPU 对读写重排的约束程度，`relaxed` 最弱，`seq_cst` 最强。
- **acquire / release**：一对单向约束。release 保证它之前的操作不会被观察到晚于它；acquire 保证它之后的操作不会被观察到早于它。
- **`PTHREAD_PROCESS_SHARED`**：mutex 或条件变量的属性，设了才能跨进程用，对象本身要放在共享内存里。
