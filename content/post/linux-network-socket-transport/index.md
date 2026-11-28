---
title: "Linux 网络栈（二）：socket、协议队列与应用交付"
slug: linux-network-socket-transport
date: "2026-11-22T16:02:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "写成功、被确认、被读走是三个不同时刻：socket 与 sock 各自记住什么，数据在哪个队列上等待，UDP 又为什么不保留没有读完的那部分。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

`write()` 返回成功，对端应用可能一个字节都没读。数据此时在哪？继续发为什么最终会失败，等下去又是谁被挂起、谁被唤醒？

系列导航：[1](/p/linux-network-stack-map/) · **2** · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

这一篇展开[第一篇](/p/linux-network-stack-map/)的块 A、块 B 的前半和块 E：socket 与 sock 各自记住什么，字节在哪个队列上等待，接收侧怎样找到连接并叫醒应用，UDP 又为什么在这里与 TCP 分道。

实验沿用第一篇的 C 程序与直连拓扑脚本。准备步骤（`lab.sh direct`、`wget` 取文件、编译）在第一篇 1.2 已经做过，这里只重复用到的命令。多路复用那一节另有一个程序 `muxdemo.c`，也在本篇目录，编译命令写在 5.4。实验机是 7.0.0-44，源码对照上游 `v7.0`。

![socket 对象与两条队列](/p/linux-network-socket-transport/objects.svg)

## 一、场景：数据到了，没人读

这一节要三个终端，按顺序敲，全部动作都落在同一个 20 秒窗口里。

### 1.1 服务端先挂住

`tcp-server` 的第四个参数是 `accept` 之后睡多少毫秒。这里给 20000：这段时间它不读，睡醒后一直读到对端关闭，才打印总数。窗口留这么长不是为了测延迟，是为了给后面两条命令腾出时间。

终端一：

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.1.2 18081 20000
```

```text
ready TCP 10.77.1.2:18081
```

### 1.2 客户端填到发不动，然后挂住

客户端请求 16384 字节 `SO_SNDBUF`，置 `O_NONBLOCK`，每次写 4096 字节直到发不动；第五个参数是发不动之后挂多久，给 30000，比服务端窗口长，保证观察时连接还在。

终端二：

```bash
sudo ip netns exec nslab-a ./netdemo tcp-fill 10.77.1.2 18081 30000
```

```text
SO_SNDBUF requested=16384
accepted=120560 bytes; send=EAGAIN; holding fd 30000 ms
```

### 1.3 趁窗口还没结束，看两端的队列

终端三，两条命令各进一个 netns：

```bash
sudo ip netns exec nslab-b ss -tinm 'sport = :18081'
```

```text
State Recv-Q Send-Q Local Address:Port  Peer Address:Port
ESTAB 120560 0      10.77.1.2:18081    10.77.1.1:42716
	 skmem:(r124400,rb131072,t0,tb87040,f2576,w0,o0,bl0,d1) cubic …
```

```bash
sudo ip netns exec nslab-a ss -tinm 'dport = :18081'
```

```text
State Recv-Q Send-Q Local Address:Port  Peer Address:Port
ESTAB 0      0      10.77.1.1:42716    10.77.1.2:18081
	 skmem:(r0,rb131072,t0,tb32768,f0,w0,o0,bl0,d0) cubic …
```

| 观察端 | 状态 | Recv-Q | Send-Q |
| --- | --- | --- | --- |
| 服务端 nslab-b | `ESTAB` | 120560 | 0 |
| 客户端 nslab-a | `ESTAB` | 0 | 0 |

服务端的 `Recv-Q` 是它自己 socket 的接收队列——数据已经到了这台机器、在它自己的缓冲里，只是没有交给应用。客户端的发送队列此刻清空，因为这一侧的数据已经被确认并释放。

`-t` 只列 TCP，`-i` 打内部信息（`cwnd`、`rtt`），`-n` 不解析名字，`-m` 打 `skmem`。`Recv-Q`、`Send-Q` 是队列深度；`skmem` 的 `r124400` 比 `Recv-Q` 的 120560 多出的是 skb 头与内存记账，两者描述不同对象，不能混用。

这条 `ss` 必须在前两个终端还挂着的时候跑。服务端 20 秒一到就把积压读走，`Recv-Q` 立刻归零，再查就只剩一条空队列。

### 1.4 窗口结束，两端各自收尾

服务端醒来读到对端关闭，打印总数：

```text
received=120560 bytes
```

三处数字相等：客户端的 `accepted`、窗口内服务端的 `Recv-Q`、服务端最后的 `received`。这条关系是可复现的部分；具体数值每次不同，另一次运行是 61520，取决于 `EAGAIN` 那一刻对端通告了多大窗口、已经确认掉多少。所以也不能拿这个数反推缓冲区容量：发送循环在跑的同时，协议栈一直在把数据送出去并获得确认，腾出的空间可以再用。

`EAGAIN` 是先前某次调用的结果，`ss` 是稍后的快照。窗口内 `Send-Q` 归零完全可能，再发一次也可能又有空间，快照不能反过来否定此前的瞬时条件。

## 二、fd 后面的对象

### 2.1 socket 与 sock 的分工

```c
struct socket {
	struct file *file;          // 创建它的 file 对象
	socket_state state;
	short type;                 // SOCK_STREAM、SOCK_DGRAM
	unsigned long flags;
	const struct proto_ops *ops;
	struct file *wq_file;
	struct socket_wq wq;        // 等待队列：谁在等读、谁在等写
	struct sock *sk;            // 具体的传输层对象
};

struct sock {                   // 通用骨架，具体协议再往上加
	struct socket *sk_socket;
	void *sk_user_data;
	struct sk_buff_head sk_receive_queue;   // 已到本机、尚未交付给应用
	struct sk_buff_head sk_write_queue;     // 已交给协议、尚未被确认
	struct socket_wq *sk_wq;                // 指向 socket 的等待队列
	...
};

struct tcp_sock {
	struct inet_connection_sock inet_conn;
	u32 rcv_nxt;                // 期待的下一个序号
	u32 copied_seq;             // 已交给应用的位置
	u32 snd_una;                // 最早未确认的序号
	u32 snd_nxt;                // 下一个要发送的序号
	...
};
```

`file` 与 `sk` 不是一对一同步的。`dup()` 出来的两个 fd 指向同一个 `file`，也就指向同一个 socket 和同一个 TCP 连接，读写共享一条队列。`close()` 一个 fd 只是减少引用计数，连接不因此结束。

接收到的字节属于连接，不属于某一个 fd。用哪个 fd 读都一样。

`listen()` 之后，新连接来自 accept 队列；`accept()` 返回的是新 socket，那个 socket 的 `ops` 还是 `inet_stream_ops`，真正的协议对象是新建的连接。

### 2.2 阻塞与非阻塞

`send()` 阻塞，等的是发送队列有空间；`recv()`、`read()` 阻塞，等的是接收队列非空。两个条件都挂在 socket 的等待队列（`wq`）上：等待的主体是线程，等待的位置在 socket。

非阻塞描述符不进等待队列，条件不满足直接返回 `EAGAIN`，由调用方自己决定下一轮。

队列由空变非空、由满变不满时，协议栈才去通知等待者。这一步属于块 E，放到 5.1 一起看。

## 三、写成功、被确认、被读走

### 3.1 三个时刻

![三个不同时刻](/p/linux-network-socket-transport/delivery.svg)

| 时刻 | 发生了什么 | 应用能推出什么 |
| --- | --- | --- |
| `write()` 返回 n | 数据进入本地发送队列，协议栈接管 | 这 n 字节不会因为本地缓冲不足而丢；不表示对端收到 |
| 对端 TCP 确认 | 对端协议栈已收到并校验 | 对端内核有了这些字节，对端应用还没读 |
| 对端 `read()` 返回 n | 数据从对端接收队列进入用户缓冲区 | 对端应用已拿到这些字节 |

`write()` 返回小于请求长度，很常见：本地缓冲剩余空间不足，或被信号打断。返回值是"这次收下了多少"，不是"发出了多少"。发不完的部分由调用方接着发。

"发送成功"不能当业务凭据。要确认对端应用收到并处理，只能在应用层做应答；TCP 的确认属于协议栈之间。

计数、日志都不能代替这个区别。两者都只证明本地接受了数据。

### 3.2 两组分发

发送方向的分发在 socket 层：`sock_sendmsg()` 按 `sk->sk_prot` 找到 `tcp_sendmsg()` 或 `udp_sendmsg()`，之后进入对应协议。

接收方向是三步：先按四元组找到连接（`__inet_lookup_skb()`），再把数据放进该连接的接收队列或直接交付，最后通知等待者。找不到连接、校验失败、队列满，都在这一步决定丢弃；到块 D 走完，一个包为什么消失才说得清楚。

不是每个包都唤醒一次 `read()`。唤醒是按需发生的：队列非空且有人在等，才唤醒；线程正在跑，就不需要。

这里"交付"的边界要分清：数据进了 `sk_receive_queue` 算到了本机 socket，但还没进用户内存；`read()` 才做那次复制。

## 四、UDP 的报文边界

### 4.1 一次截断

`udp-server` 的第四个参数是每次 `recv()` 最多取多少字节。这里给 3，也就是让缓冲区装不下整条报文。

终端三起服务端：

```bash
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.1.2 18081 3
```

终端四发两个数据报：先 6 字节 `ABCDEF`，再 2 字节 `FG`。

```bash
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.1.2 18081
```

```text
sent datagrams: 6 bytes, 2 bytes
```

服务端输出：

```text
recv[0] len=6 copied=3 content=ABC
recv[1] len=2 copied=2 content=FG
```

`len` 是这条报文原本的长度，`copied` 是真正落进缓冲区的字节。程序在 `recv()` 上带了 `MSG_TRUNC`，两者才能同时看到；不带这个标志时，返回值只有 `copied`，被截掉多少就无从得知。

`D`、`E`、`F` 不是留在队列里等下一次读——它们随这条数据报一起被丢弃，下一次读到的是下一条报文 `FG`。UDP 保留边界，也就保留了这个后果。

把第四个参数换成 1024，同一条报文就完整回来了：

```bash
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.1.2 18081 1024
```

```text
recv[0] len=6 copied=6 content=ABCDEF
recv[1] len=2 copied=2 content=FG
```

这里改的是**每次读取的长度**，不是 socket 的接收缓冲区。后者是内核排队的地方，用 `SO_RCVBUF` 设置，`ss -u` 的 `Recv-Q` 看的是排了多少字节。

### 4.2 与 TCP 的分界

TCP 是字节流：接收队列里是连续的字节，`read()` 读多少由调用方决定，剩余部分下次接着读，边界由应用自己划。

UDP 是报文：一次 `read()` 对应一条数据报，读不完的剩余部分被丢掉。UDP 同样有接收队列、同样会满、同样会丢包，只是没有重传和接收窗口来兜底。

这里也是块 A 的两种收尾：TCP 的连接状态和重传都属于 `sock`；UDP 没有连接状态，`sock` 更接近一个带队列的端口。

## 五、多路复用与唤醒

### 5.1 等待队列与就绪通知

`sock_def_readable()` 先看 `sk_sleep()`，也就是 `sk->sk_wq->wait`。有人等，就调用 `wake_up_interruptible_sync_poll()`；没人等，什么也不做，数据留在队列里。

唤醒只是把等待者标成可运行，不等于马上跑。它还要重新竞争 CPU，醒来时条件也可能已经被别的线程改掉——所以阻塞调用会重新检查条件，而不说被唤醒就直接返回。

`epoll` 挂上去的不是线程，是回调。`epoll_ctl(ADD)` 把一个 `eppoll_entry` 挂到目标 socket 的等待队列上；数据到达时，`ep_poll_callback()` 把这个 fd 加入 `ep->rdllist`，需要时再唤醒正在 `epoll_wait()` 的线程。

所以"数据到了"和"应用知道数据到了"是两件事。中间那一步叫通知，可能立刻发生，也可能等到应用下次查询。

### 5.2 水平触发与边缘触发

水平触发（LT）下，只要接收队列非空，每次 `epoll_wait()` 都会报告这个 fd。读慢一点不会漏，代价是每次都要重新检查。

边缘触发（ET）下，只在状态发生变化时报告一次：从空变成非空。应用必须一次读到 `EAGAIN`，把数据读干净；中途停下，没有新数据到来就不会再有通知，连接就卡在那里。

差别要等一次读不完的时候才显出来。客户端一口气写 256 KiB 并保持连接，接收端每次 `recv()` 只取 4 KiB：

| 接收策略 | `epoll_wait` 返回次数 | `recv` 次数 | 读到的字节 |
| --- | --- | --- | --- |
| 水平触发，每次事件读一次 | 66 | 66 | 262144 |
| 边缘触发，读到 `EAGAIN` | 3 | 66 | 262144 |
| 边缘触发，每次事件只读一次 | 7 | 7 | 28672 |

前两行读到的字节一样多，通知次数差 20 倍。第三行是漏写循环的后果：剩下的 233472 字节还在接收队列里，连接没断，也没有报错，只是没有新数据到达，`epoll_wait()` 不再返回这个 fd。

ET 不是更快，是把"检查"的责任交给应用。它省掉的是重复报告，换来的是必须写对循环；而重复报告只在一次读不完的时候才存在，每条报文都独立到达的负载里，两种触发方式的唤醒次数几乎一样。

### 5.3 select、poll、epoll 与 io_uring 的位置

| 接口 | 关注集合放在哪 | 每次等待要做什么 |
| --- | --- | --- |
| `select` | 调用方栈上，位图 | 把集合拷进内核，遍历，返回后还要自己找哪一位被置上 |
| `poll` | 调用方数组 | 拷贝 `pollfd` 数组，遍历，返回 `revents` |
| `epoll` | 内核里的关注列表与就绪链表 | 只把就绪的 fd 取回，集合不重复拷贝 |
| `io_uring` | 用户与内核共享的提交/完成环 | 提交与回收通过环进行，不必每次操作都进一次系统调用 |

差别在"找哪个 fd 就绪"的成本，不在数据搬运。`epoll` 不会减少 `read()` 那次从内核到用户缓冲区的复制，也不会让协议栈少处理一个包。

`io_uring` 更进一步，把提交和完成也做成环上的条目；配合 `IORING_SETUP_SQPOLL` 还能由内核线程轮询提交环，进一步减少系统调用。它改变的是调用方式，不是块 A 到块 E 的路径。

多路复用的位置在块 E 的末尾：数据已经在 socket 队列里，等待者从"一个线程阻塞在 `read()`"变成"一个线程等在 `epoll_wait()`，由回调把 fd 送进就绪链表"。前面的查找、入队、唤醒都没有变。

### 5.4 四种写法在同一次负载下

100 个连接、每个连接 100 次 64 字节收发，服务端和客户端分别跑在两个 netns 里：

| 写法 | 服务端线程 | 峰值 RSS | 上下文切换 | `epoll_wait` | 耗时 |
| --- | --- | --- | --- | --- | --- |
| 每连接一个线程 | 101 | 2228 kB | 8987 | — | 314 ms |
| `epoll`，水平触发 | 1 | 1404 kB | 9868 | 10049 | 214 ms |
| `epoll`，边缘触发 | 1 | 1396 kB | 9275 | 10044 | 196 ms |
| `epoll`，4 线程加 `EPOLLONESHOT` | 5 | 1588 kB | 9434 | 10053 | 315 ms |

三点值得记：

- 线程从 101 降到 1，RSS 少了约 800 kB。线程版慢一些，主要慢在 100 个线程争两个 CPU 的调度上，不是每次收发更费。多路复用省的是每连接一份栈的成本，不是把 `read()` 变快。
- 边缘触发的 `epoll_wait` 次数和水平触发几乎一样（10044 对 10049）。64 字节一条报文，每次到达都是一次"从空到非空"，没有重复通知可省；上一节那种一次写 256 KiB 的场景才省出 20 倍。哪个快取决于负载形状，不取决于触发方式的名字。
- 多线程版每个 fd 必须带 `EPOLLONESHOT`，处理完再 `EPOLL_CTL_MOD` 重新武装。少了这一步，两个线程会同时被叫醒去读同一个 socket，一个读到数据，另一个白跑一趟。

程序在[本目录](/p/linux-network-socket-transport/muxdemo.c)，`gcc -O2 -pthread -o muxdemo muxdemo.c` 编译。上面的计数都来自程序自己：服务端收到 `SIGUSR1` 打印 `accept`、`read`、`write`、`epoll_wait` 的次数，接收端的 `wakes` 是 `epoll_wait()` 有返回的次数。四次运行与三种接收策略的原始输出在[写法对照](/p/linux-network-socket-transport/mux-modes.txt)和[接收策略对照](/p/linux-network-socket-transport/mux-drain.txt)。

## 六、命令与接口速查

| 目的 | 命令 |
| --- | --- |
| 看队列深度与内存 | `ss -tinm 'sport = :18081'` |
| 非阻塞发送到 EAGAIN | `./netdemo tcp-fill <ip> <port> [hold_ms]` |
| UDP 截断复现 | `./netdemo udp-server <ip> <port> [recv_bytes] [delay_ms]` |
| 观察系统调用 | `strace -f -e trace=network,read,write ./netdemo ...` |
| 四写法对照 | `./muxdemo thread <port>`，`epoll-lt`、`epoll-et` 同参数；多线程用 `epoll-mt <port> 4` |
| 一次灌 256 KiB | `./muxdemo blast <ip> <port> 262144 6000` |
| 三种接收策略 | `./muxdemo sink-lt <port>`，换成 `sink-et`、`sink-et-once` |
| 取服务端计数 | `kill -USR1 <pid>`，读 `epoll_wait`、`read`、`write` |

## 七、名词解释

- **`struct socket`**：fd 对应的协议无关外壳，持有 `ops`、`wq` 和 `sk`。
- **`struct sock`**：传输层通用对象，队列、状态、回调挂在这里。TCP 的连接是 `struct tcp_sock`，内嵌 `inet_connection_sock` 和 `sock`。
- **`sk_receive_queue`**：已到本机、尚未交付应用的 skb 队列。
- **`sk_write_queue`**：已交给协议、尚未被确认的 skb 队列。
- **`EAGAIN`**：非阻塞条件下暂时无法完成，稍后重试；不是错误。
- **水平触发（LT）**：条件持续满足就持续报告。
- **边缘触发（ET）**：只在条件由不满足变为满足时报告一次，应用必须读到 `EAGAIN`。
- **`wq` / `sk_wq`**：socket 的等待队列，挂的是等待读、等待写的线程。
- **就绪链表（`rdllist`）**：`epoll` 实例里已经就绪、等 `epoll_wait()` 取走的 fd 列表。
- **`eppoll_entry`**：`epoll` 挂到目标 socket 等待队列上的回调载体，数据到达时触发 `ep_poll_callback()`。
- **`EPOLLONESHOT`**：事件报告一次后自动失效，处理完要用 `EPOLL_CTL_MOD` 重新武装，用来保证同一时刻只有一个线程处理同一个 fd。
