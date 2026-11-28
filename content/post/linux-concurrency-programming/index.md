---
title: "Linux 并发编程：线程、原子与交接的代价"
slug: linux-concurrency-programming
date: "2026-11-28T20:00:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "同一个计数器有三种写法，发布一条消息可以只多一条指令；差别最后落在 cacheline 归谁、栈有多大、一次唤醒要走多远。"
categories:
    - Linux 与系统编程
tags:
    - Linux
    - 并发
---

两个线程都要改同一个计数，能不能直接 `x++`？加一把锁贵多少？把两个变量分开放，为什么能快四倍？一个生产者、一个消费者，能不能不用锁？

这一篇用同一台机器上的一组小程序回答这些问题：线程与栈、原子操作与内存顺序、伪共享、单生产者单消费者环、绑核、线程池、协程，每一节都给出改变一个条件前后的差别，并解释差别落在哪个对象上。

同步原语的实现不在本篇展开。[《Linux 用户态同步》](/p/linux-userspace-synchronization/)讲 mutex 的三态与 futex 怎样把等待交给内核，[《Linux 内核同步》](/p/linux-kernel-synchronization/)讲内核里自旋锁、等待队列和内存屏障怎么选。本篇只把那些结论当作已知，关心应用层怎么写、代价多大。

实验机是 Ubuntu 26.04、内核 7.0.0-44、aarch64、2 vCPU、3892 MB，编译器 gcc 15.2，统一 `-O2`。ARM64 是弱内存序机器，第三节的指令差异与它有关。绝对耗时受两核限制，正文只用比值和量级，原始输出在同目录的记录文件里。

程序是本篇的 [`conc-lab.c`](/p/linux-concurrency-programming/conc-lab.c)，一个文件带八种模式：

```bash
wget -q https://lixiaojun2914.github.io/p/linux-concurrency-programming/conc-lab.c
gcc -O2 -pthread -Wall -Wextra conc-lab.c -o conc-lab
```

## 一、一个计数器，三种做法

### 1.1 不加同步时，编译器把循环合并了

先看最直觉的写法：一个全局 `long`，两个线程各加五百万次。

```bash
./conc-lab counter plain 2 5000000
```

```
plain threads=2 iters=5000000 total=108750 ns/op=0.01 result=10000000
```

结果正好是一千万，看起来没问题；耗时折算下来每次自增 0.01 ns，比一个时钟周期还短，这就不可能了。反汇编这个分支：

```
ldr  x0, [x19, #32]
add  x0, x0, x20
str  x0, [x19, #32]
```

五百万次循环只剩下一次加法：`x20` 是循环次数，编译器把整个循环归纳成了 `g_plain += 5000000`。在 C11 的内存模型里，两个线程不加同步地访问同一个非原子对象是数据竞争，行为未定义，编译器完全有权假定"没有别的线程在看这个变量"。所以这里的"结果正确"只是这次运行碰巧，换一个优化级别、换一个编译器版本，结果和耗时都会变。

这条反例的价值不在计数本身，而在于：**任何绕过同步的共享访问，都无法用"跑出来对"来验证**。

### 1.2 一把锁

```bash
./conc-lab counter mutex 2 5000000
```

```
mutex threads=2 iters=5000000 ns/op=16.24 result=10000000
```

两个线程争同一把锁，一次 `lock` 加一次 `unlock` 合计十几纳秒。这个数字说明大部分时间没有进内核：glibc 的 mutex 在无人持有时只做一次用户态原子交换，真正的等待与唤醒才走 futex。争用出现时，等的那一方会先在用户态自旋一小段，仍然不划算才睡。

### 1.3 原子加减

```bash
./conc-lab counter seq 2 5000000
./conc-lab counter relaxed 2 5000000
```

```
seq     threads=2 iters=5000000 ns/op=2.95 result=10000000
relaxed threads=2 iters=5000000 ns/op=2.92 result=10000000
```

原子加减比加锁快五倍多，而 `seq_cst` 与 `relaxed` 在这里几乎一样。看汇编就明白：

```
bl  __aarch64_ldadd8_relax     ← relaxed
bl  __aarch64_ldadd8_acq_rel   ← seq_cst
```

两条路径都是 ARMv8 的 LSE 原子指令，一条指令完成"读—改—写"，由硬件保证该 cacheline 的独占。两者的差别只在指令内部是否附加屏障，而在这个基准里，真正的瓶颈是两核争同一条 cacheline 的独占权，屏障成本被摊薄到看不出来。

所以"`seq_cst` 一定更慢"不是规律。争用同一行时，独占才是主成本；屏障的代价要在没有争用的场景里单独看，第三节会看到它体现在指令选择上，而不是这个计数器上。

## 二、线程的栈

### 2.1 8 MB 是地址空间，不是内存

```bash
./conc-lab threads 8
```

```
main    stack=[0xffffd7edc000,0xffffd86dc000) size=8192 kB rss=8 kB
thread 0 stack=[0xe413744e0000,0xe41374ce0000) size=8192 kB rss=52 kB
thread 1 stack=[0xe41373cd0000,0xe413744d0000) size=8192 kB rss=64 kB
...
```

每个线程的栈都是 8 MB 的地址区间，实际驻留只有几十 KB：刚创建的线程只碰过栈顶附近几页，其余部分还是未分配的匿名映射，第一次写入才触发缺页。主线程的栈在 `[stack]` 段里，驻留 8 kB，其余页没有被触碰。

相邻线程的栈顶相差 `0x810000`，即 8 MB 加 64 KB：那 64 KB 是 guard 区，栈溢出时先撞到它，进程收到 `SIGSEGV`，而不是悄悄踩坏相邻线程的数据。

### 2.2 栈大小有下限

把栈改小，先看系统接受什么：

```bash
./conc-lab stack 64
./conc-lab stack 128
./conc-lab stack 1024
```

```
setstacksize(64 kB) = 22
requested=64 kB actual=8192 kB used=37 kB rss=12 kB
setstacksize(128 kB) = 0
requested=128 kB actual=128 kB used=69 kB rss=12 kB
setstacksize(1024 kB) = 0
requested=1024 kB actual=1024 kB used=517 kB rss=40 kB
```

64 kB 被拒绝，返回 22 即 `EINVAL`；这台机器上最小 128 kB。被拒之后 `pthread_create` 仍用默认值，所以 `actual` 还是 8192——这也是一个容易忽略的点：`pthread_attr_setstacksize` 的返回值必须检查，否则会以为设置生效了。

1 MB 栈里用掉 512 KB，`used` 是 517 kB，但驻留只有 40 kB：程序只写了首尾两个字节，中间 128 个页一个都没碰。栈的内存是按页懒分配的，改小栈省下的是地址空间和 guard 页，不是立刻省下物理内存。

线程数上千时，这两者的差别就明显了：一万个线程按默认栈要 80 GB 地址空间，虽然驻留不多，但页表、`vm.max_map_count` 和地址空间碎片都会先出问题。

## 三、原子操作与内存顺序

### 3.1 单条读写生成什么指令

把六种写法各编成一个函数，看 gcc 在 aarch64 上生成什么：

| 操作 | C11 写法 | 生成的指令 |
| --- | --- | --- |
| 写 | `memory_order_relaxed` | `str` |
| 写 | `memory_order_release` | `stlr` |
| 写 | `memory_order_seq_cst` | `stlr` |
| 读 | `memory_order_relaxed` | `ldr` |
| 读 | `memory_order_acquire` | `ldar` |
| 读 | `memory_order_seq_cst` | `ldar` |

`stlr` 是带 release 语义的存储：它之前的所有写，不会被观察到晚于这一条。`ldar` 是带 acquire 语义的加载：它之后的所有读，不会被观察到早于这一条。`str` 与 `ldr` 没有这两层约束。

单条操作的 `seq_cst` 与 `release`、`acquire` 生成同一条指令，因为一条指令没有"前后的伙伴"可以重排。真正的区别出现在成对操作之间。

### 3.2 发布一条消息：两条写的顺序

生产者要把一份数据交给消费者，最自然的写法是两个变量：`data` 放内容，`flag` 放"已就绪"。三种内存序生成的代码如下：

```
pub_relaxed:   str  x2, [x0]      ← data
               str  x2, [x1]      ← flag

pub_release:   str  x2, [x0]      ← data
               stlr x2, [x1]      ← flag

pub_seq:       stlr x2, [x0]
               stlr x2, [x1]
```

第一种写法里两条都是普通写。ARM64 允许它们以任意顺序被其他核看到，于是消费者可能先看到 `flag` 变成 1，再读 `data` 还是旧值——它拿到了一条"半成品消息"。

第二种写法只把第二条改成 `stlr`：`data` 的普通写被挡在 `stlr` 之前，不可能晚于 `flag` 被看到。这就是 release 的全部含义，一条指令，不需要把 `data` 也做成原子写。

第三种写法两条都是 `stlr`，语义更强，代价是每条写都带约束。

对应的读侧也要配对：

```c
/* 生产者 */
atomic_store_explicit(&data, v, memory_order_relaxed);
atomic_store_explicit(&flag, 1, memory_order_release);

/* 消费者 */
while (!atomic_load_explicit(&flag, memory_order_acquire))
        ;
v = atomic_load_explicit(&data, memory_order_relaxed);
```

读侧的 `acquire` 不能省。少了它，编译器和 CPU 都可以把 `data` 的读提前到 `flag` 的读之前，于是循环还没结束，`v` 就已经是旧值装进寄存器了。release 管"先写数据再举手"，acquire 管"看到举手再读数据"，两者是一对。

### 3.3 什么时候非 `seq_cst` 不可

acquire/release 只约束"这一对"的先后，不提供所有线程看到的统一顺序。一旦协议要求两个变量之间的顺序对所有线程一致，就必须用 `seq_cst`。典型的是双标志互斥：

```c
/* 线程 0 */                      /* 线程 1 */
flag0 = 1;                        flag1 = 1;
if (flag1 == 0) enter();          if (flag0 == 0) enter();
```

如果两条写都用 release、两条读都用 acquire，两个线程可以同时进入临界区：各自看到对方的写还没到，自己的写已经生效。这个协议要求"两个写有一个全局顺序"，只有 `seq_cst` 给得出。

单条计数器不属于这一类。它只需要"加一不丢"，不需要和别的变量排先后，`relaxed` 就够；需要读到一个准确快照时，用 `seq_cst` 或加锁。

### 3.4 乱序的可见性：这条边界要说清

按 3.2 的 relaxed 写法跑两百万轮，统计消费者读到"`flag` 已置位但 `data` 还是旧值"的次数：

```bash
./conc-lab litmus pub relaxed 2000000
./conc-lab litmus pub seq 2000000
```

```
pub relaxed rounds=2000000 torn=0
pub seq     rounds=2000000 torn=0
```

两百万轮一次都没命中。这不代表 relaxed 是安全的，只说明这台机器上两个核之间 store 传播的时间差小于一次读的间隔，窗口太窄，用计数抓不到。硬件 litmus 测试里这类违例常在百万分之一到十亿分之一量级，需要专门的对齐与放大手段才能稳定复现。

所以这一节的结论来自指令和语义，不来自计数：`str` 没有顺序约束，`stlr` 有，这是编译器输出可以直接核对的事实；违例发生时的表现则是"消费者拿到半条消息"，在真实系统里表现为偶发的数据错乱，而不是每次必现。

## 四、伪共享

### 4.1 两个计数器相邻

两个线程各自只写自己的计数器，逻辑上毫无关系。把它们放在一个结构体里相邻，与各占一条 cacheline，差别如下：

```bash
./conc-lab share pack 2 20000000
./conc-lab share pad  2 20000000
```

```
pack threads=2 iters=20000000 ns/op=3.09
pad  threads=2 iters=20000000 ns/op=0.80
```

相邻时每次写 3.1 ns，填充后 0.8 ns，差约四倍。

![两个计数器放在一条 cacheline 与各占一条](/p/linux-concurrency-programming/cacheline.svg)

机制在图里：两个变量落在同一条 64 B 的行上，核 0 写 `a` 要先取得该行的独占权，核 1 手里的副本随之失效；核 1 再写 `b` 又要把行抢回去。两个线程没有共享任何逻辑数据，却在硬件层反复争夺同一条 cacheline，这叫伪共享（false sharing）。

ARM64 的一级数据 cache 行也是 64 B，用 `getconf LEVEL1_DCACHE_LINESIZE` 可以确认。

### 4.2 怎么判断，代价是什么

判断依据是地址距离，不是字段类型：两个会被不同线程写的变量，地址差小于 64 B 就落在同一行。C11 的写法是给结构体成员加对齐：

```c
struct padded {
        _Atomic long a;
        char pad[64];        /* 让 b 落到下一条行 */
        _Atomic long b;
        char pad2[64];
};
```

代价是内存。填充只该用在热点路径上的少数变量；一个每秒被写百万次的结构体值得，一个每次请求只改一次的结构体不值得。

数组里也有同样的问题：`struct item arr[N]`，每个线程处理不同的元素，元素大小小于 64 B 时相邻元素仍然共享行。这时要么让 `item` 按 64 B 对齐并补齐，要么改成每个线程一份局部计数、最后合并。

## 五、单生产者单消费者环

### 5.1 谁写哪个指针

一个固定容量的环形数组，一个生产者放、一个消费者取，可以做成完全无锁。关键是两个指针各只有一个写者：

- `tail` 只由生产者写，消费者只读；
- `head` 只由消费者写，生产者只读。

![单生产者单消费者环的指针归属](/p/linux-concurrency-programming/spsc.svg)

生产者把 `tail` 存在本地变量里，只在推进时写回；消费者对 `head` 同样处理。这样每次放入/取出只碰一次对方的指针，而不是每次都读自己的：

生产者放入一条：

```c
for (;;) {
        head = atomic_load_explicit(&r_head, memory_order_acquire);
        if (tail - head < RING_CAP)
                break;
        sched_yield();
}
rbuf[tail & RING_MASK] = v;
atomic_store_explicit(&r_tail, tail + 1, memory_order_release);
```

消费者取出：

```c
for (;;) {
        t = atomic_load_explicit(&r_tail, memory_order_acquire);
        if (head != t)
                break;
        sched_yield();
}
v = rbuf[head & RING_MASK];
atomic_store_explicit(&r_head, head + 1, memory_order_release);
```

`head` 和 `tail` 是无符号计数，一直递增、允许回绕，所以判断满不需要额外字段：`tail - head == RING_CAP` 就是满，`head == tail` 就是空。下标用 `& (RING_CAP - 1)` 取模，容量取 2 的幂。

数据写入与指针推进之间是 3.2 的发布模式：先写 `rbuf`，再用 release 推进 `tail`；消费者用 acquire 读 `tail`，再读 `rbuf`。少了这一对，消费者可能读到还没写完的槽位。

### 5.2 与加锁队列的对照

```bash
./conc-lab ring lock   2000000
./conc-lab ring atomic 2000000
```

```
lock   n=2000000 ns/msg=100.20 Mmsg/s=9.98
atomic n=2000000 ns/msg=43.87 Mmsg/s=22.79
```

加锁版本每条消息约 100 ns，无锁版本 44 ns。加锁版本每条要做两次加锁、两次解锁、两次条件判断；无锁版本只有两次原子写和两次原子读。

### 5.3 无锁不等于无等待

队列满时生产者仍在循环，空时消费者仍在循环，只是等待方式从"睡在条件变量上"变成"自旋加让出"。`sched_yield()` 在满负载下会让出 CPU 又立刻被调度回来，实际效果取决于调度器；更常见的做法是短暂自旋后转入阻塞，或者干脆把容量设得足够大，让满/空不常发生。

还有一个前提：只有单生产者单消费者才这么便宜。多生产者要么用 CAS 抢 `tail`，要么按生产者分片再合并；CAS 版本的成本会接近加锁版本，因为争用的还是同一条 cacheline。

## 六、绑核与交接成本

### 6.1 分核与同核

同一对生产/消费线程，一个绑 CPU 0、一个绑 CPU 1，与两个都绑 CPU 0：

```bash
./conc-lab aff split 2000000
./conc-lab aff same  2000000
```

```
split n=2000000 ns/msg=28.96
same  n=2000000 ns/msg=1.28
```

分核每条消息约 29 ns，同核 1.3 ns，差二十倍。原因不是"同核更快"，而是这次交接的载体是一行 cacheline：分核时 `tail` 的每次推进都要把行从生产者核迁到消费者核，同核时两个线程轮流用同一份 L1 副本，交接几乎免费。

代价是并行度。同核时生产和消费只能轮流跑，在 2 vCPU 上这个微基准的瓶颈恰好就是交接本身，所以同核赢；一旦每一侧都有实际计算量，分核才能让两段计算重叠。

### 6.2 什么时候该绑

判断依据是"这份数据被几个核碰"：

- 交接频繁、每次只有几十字节，且两侧计算都轻——同核或同一 CCX 内更划算；
- 两侧都有重计算，交接只占一小部分——分核，接受每次几十纳秒的迁移成本；
- 不确定时，先不绑，用 `perf stat` 看 `cache-misses` 与 `context-switches`，再决定。

绑核还会和调度器的负载均衡冲突：绑死之后，核上出现更紧急的任务也无法迁移。生产环境里通常只给关键线程绑核，并留出至少一个核给其他工作。

## 七、线程池

### 7.1 每任务一个线程

四千个短任务，每个新建一个线程、跑完 join：

```bash
./conc-lab pool 4000 0
```

```
workers=0 tasks=4000 total=134145347 ns/task=33536
```

每个任务 33.5 µs。任务本身只有两千次浮点运算，绝大部分时间花在 `pthread_create` 与 `pthread_join` 上：分配栈、clone、等子线程退出、回收栈。

### 7.2 固定 worker

同一批任务交给固定数量的 worker，从队列里取：

```bash
./conc-lab pool 4000 2
./conc-lab pool 4000 4
```

```
workers=2 tasks=4000 ns/task=820
workers=4 tasks=4000 ns/task=820
```

每任务 0.82 µs，比按需创建快约四十倍。4 个 worker 与 2 个完全相同，因为机器只有 2 个 vCPU，多出来的两个只是多两次上下文切换。

线程池不是免费的：本例的任务队列本身是一把锁加热条件变量，任务很短时，取任务的争用会变成新的瓶颈。任务粒度越小、worker 越多，越需要考虑分片队列或无锁队列。经验上，单任务的计算量至少要达到几微秒，池的收益才稳定。

## 八、协程与切换成本

### 8.1 ucontext 换栈

`ucontext` 提供用户态的上下文切换：保存当前寄存器与栈指针，换到另一块栈继续执行，不经过调度器。

```bash
./conc-lab switch ucontext 200000
```

```
ucontext switches=400000 ns/switch=193.2
```

每次切换约 193 ns。这个数字比想象的高，原因可以用 strace 看到：

```
% time     seconds  usecs/call     calls    errors syscall
100.00    0.138211           3     40001           rt_sigprocmask
```

两万次切换对应四万次 `rt_sigprocmask`：glibc 的 `swapcontext` 每次都要保存和恢复信号掩码，各一次系统调用。只保存 callee-saved 寄存器与栈指针的汇编切换通常只要几十纳秒，省掉的就是这部分。

### 8.2 三种交接的量级

![三种切换方式的单次成本](/p/linux-concurrency-programming/switch-cost.svg)

```bash
./conc-lab switch ucontext 200000
./conc-lab switch spin     200000
./conc-lab switch sem      200000
```

```
ucontext switches=400000 ns/switch=193.2
spin     rounds=200000 ns/round=188.0
sem      rounds=200000 ns/round=20024.7
```

三者不是同一层的操作：

- **193 ns**：同线程内换栈，寄存器与栈指针换掉，仍在同一个核上，不经过调度器；
- **188 ns**：两个核各转各的，靠一行 store 通知对方，成本是 cacheline 跨核传播；
- **20 µs**：一方睡进 futex，另一方唤醒它，等调度器把睡的一方选上来，成本包含两次上下文切换与调度延迟。

从忙等到睡眠唤醒差一百倍，这个差距决定了"什么时候该自旋"：预期等待短于一次唤醒的成本，就自旋；否则睡。

### 8.3 协程省什么，不省什么

协程省下的是内核调度与线程栈：一个线程 8 MB 地址空间，一个协程栈可以只有几十 KB，切换不需要陷入内核。它不省的是数据交接——两个协程如果跑在不同线程上，传递数据仍然要付 6.1 的 cacheline 迁移成本；协程遇到阻塞 I/O 也仍然要把整个线程让出去，除非底下换成 io_uring 之类的异步接口。

所以"协程比线程快"只在切换频率高、且切换本来就发生在同一线程内时成立。把它当成"把回调写成顺序代码"的工具，比当成性能手段更准确。

## 九、按问题选工具

| 问题 | 选择 | 主要代价 | 注意 |
| --- | --- | --- | --- |
| 单个数计数 | `atomic_fetch_add`，`relaxed` 或 `seq_cst` | 几纳秒，争同一行时更高 | 不要用普通变量；编译器会合并循环 |
| 一段不变量 | mutex | 每次十几纳秒，争用时睡眠 | 临界区要短，锁的粒度按数据划分 |
| 等一个条件 | 条件变量 | 唤醒约二十微秒 | 谓词必须用 while 重查 |
| 两份数据同时更新 | mutex 或 `seq_cst` | 与上面同量级 | 不要用 acquire/release 拼双标志协议 |
| 一对线程传消息 | SPSC 环 | 每条几十纳秒 | 只适用于单生产单消费者 |
| 高频短任务 | 线程池 | 每任务不到一微秒 | worker 不超过核数，任务不能太碎 |
| 大量并发等待 | 协程 + 异步 I/O | 每次切换百纳秒级 | 换栈不等于换线程，阻塞调用仍会占住线程 |
| 两个变量被不同线程写 | 按 64 B 隔开 | 多占内存 | 先确认它们真的在热点路径上 |

## 十、命令与接口速查

| 目的 | 命令或接口 |
| --- | --- |
| 编译 | `gcc -O2 -pthread -Wall -Wextra conc-lab.c -o conc-lab` |
| 观察线程栈范围与驻留 | `./conc-lab threads 8` |
| 改线程栈大小 | `pthread_attr_setstacksize()`，检查返回值；小于最小值返回 `EINVAL` |
| 计数器四种写法 | `./conc-lab counter plain\|mutex\|seq\|relaxed 2 5000000` |
| 伪共享对照 | `./conc-lab share pack\|pad 2 20000000` |
| 无锁环与加锁环 | `./conc-lab ring atomic\|lock 2000000` |
| 线程池对照 | `./conc-lab pool 4000 0\|2\|4` |
| 切换成本 | `./conc-lab switch ucontext\|spin\|sem 200000` |
| 绑核对照 | `./conc-lab aff split\|same 2000000` |
| 看生成的指令 | `objdump -d --no-show-raw-insn conc-lab`，或 `gcc -O2 -S memorder.c` |
| 看协程切换的系统调用 | `strace -c -f -e trace=rt_sigprocmask ./conc-lab switch ucontext 10000` |
| 确认 cacheline 大小 | `getconf LEVEL1_DCACHE_LINESIZE` |
| 绑定 CPU | `taskset -c 0,1 ./prog`，或 `pthread_setaffinity_np()` |
| 看缺页与上下文切换 | `/usr/bin/time -v ./prog`、`perf stat -e context-switches,cache-misses ./prog` |

## 十一、名词解释

- **数据竞争**：两个线程访问同一内存位置、至少一个写、且没有同步；在 C11 里是未定义行为。
- **顺序一致性（`seq_cst`）**：所有线程看到同一个全局操作顺序，最强的内存序。
- **acquire / release**：一对单向约束。release 保证它之前的操作不会被观察到晚于它；acquire 保证它之后的操作不会被观察到早于它。
- **`stlr` / `ldar`**：ARM64 上带 release / acquire 语义的存储与加载指令。
- **cacheline**：CPU 与内存之间搬运的最小单位，这台机器上是 64 B。
- **伪共享**：两个线程写同一条 cacheline 里的不同变量，硬件仍按整行处理，导致该行在核间反复迁移。
- **SPSC**：单生产者单消费者。两个指针各只有一个写者，因此不需要 CAS。
- **CAS**：compare-and-swap，读旧值、比较、相等才写入的原子指令；多写者场景用它抢占。
- **futex**：内核提供的等待/唤醒机制，用户态先原子的比较，确认要睡才进内核。
- **ucontext**：POSIX 的用户态上下文接口，`swapcontext` 换栈；glibc 实现里每次切换带两次信号掩码系统调用。
- **自旋**：不睡眠、循环检查条件；适合预期等待极短的场景。
