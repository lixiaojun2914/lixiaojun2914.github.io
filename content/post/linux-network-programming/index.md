---
title: "Linux 网络编程：socket 接口、枚举与四种多路复用"
slug: linux-network-programming
date: "2026-11-28T19:50:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "socket 一族的参数、枚举和返回值怎么用对，select、poll、epoll、io_uring 各写一遍，最后按接口形状比较。"
categories:
    - Linux 与系统编程
tags:
    - Linux
    - 网络编程
---

这一篇讲接口：BSD socket 在 Linux 上的那一组函数，参数里的枚举各是什么，返回值怎么判，写错会看到什么现象。多路复用一次讲四种——`select`、`poll`、`epoll`、`io_uring`——每种都给可运行的骨架，最后比较它们的接口形状。

协议栈内部怎么走不在本篇展开。[《Linux 网络栈（二）》](/p/linux-network-socket-transport/)讲 `send q`、`recv q`、等待队列和就绪通知，那是"数据在哪、谁唤醒谁"；本篇讲"函数怎么调"。需要机制时去那一篇看。

程序是 [`net-lab.c`](/p/linux-network-programming/net-lab.c)，一个文件带九个模式：

```bash
wget -q https://lixiaojun2914.github.io/p/linux-network-programming/net-lab.c
sudo apt install -y liburing-dev          # io_uring 部分用 liburing
gcc -O2 -Wall -Wextra net-lab.c -o net-lab -luring
```

运行环境是 Ubuntu 26.04、内核 7.0.0-44、aarch64、2 vCPU。

## 一、socket 与地址结构

### 1.1 三个参数

```c
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
```

返回文件描述符，失败返回 -1 并设置 `errno`。三个参数都是枚举：

| 参数 | 常用值 | 含义 |
| --- | --- | --- |
| `domain` | `AF_INET` | IPv4 |
| | `AF_INET6` | IPv6 |
| | `AF_UNIX` | 本机进程间，走文件系统路径 |
| `type` | `SOCK_STREAM` | 有序、可靠、有连接，对应 TCP |
| | `SOCK_DGRAM` | 保留报文边界、不可靠，对应 UDP |
| | `SOCK_RAW` | 自己拼 IP 头，需要 `CAP_NET_RAW` |
| `protocol` | `0` | 按前两个参数选默认协议 |
| | `IPPROTO_TCP` / `IPPROTO_UDP` | 显式指定，一般不用 |

`type` 还能或上两个标志：`SOCK_CLOEXEC`（exec 时关闭）和 `SOCK_NONBLOCK`（非阻塞）。程序里一直用 `SOCK_STREAM | SOCK_CLOEXEC`，省掉一次 `fcntl`：

```c
fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
```

`AF_` 是地址族（address family），`PF_` 是协议族（protocol family），在 Linux 上两者数值相同，混用不会出错，但按接口约定：`socket()` 的第一个参数写 `AF_*`。

### 1.2 sockaddr 家族

所有收发地址的函数都接 `struct sockaddr *`，但没人真的用这个类型，而是用它的具体形态再强转：

![sockaddr 与三个具体形态](/p/linux-network-programming/sockaddr-family.svg)

三个要点：

- 第一个字段永远是 `sa_family_t`（2 字节）。内核只读这 2 字节，据此决定后面按哪个结构解释。
- 长度参数不能写 `sizeof(struct sockaddr)`。它只有 16 字节，IPv6 是 28，Unix 域按路径长度算。写错了不会立刻报错，而是截断或多读。
- `accept`、`recvfrom`、`getsockname` 的长度参数是**双向**的：调用前告诉内核缓冲区有多大，返回时被改写成实际长度。所以必须传 `socklen_t` 变量的地址，不能传常量。

### 1.3 字节序与地址转换

网络字节序是大端，主机字节序看 CPU。四个函数：

```c
#include <arpa/inet.h>
#include <netinet/in.h>

uint16_t htons(uint16_t hostshort);      /* 主机 → 网络，16 位 */
uint32_t htonl(uint32_t hostlong);       /* 主机 → 网络，32 位 */
uint16_t ntohs(uint16_t netshort);
uint32_t ntohl(uint32_t netlong);
```

端口必须过 `htons`，地址用 `inet_pton` 从文本转成二进制：

```c
struct sockaddr_in a = {0};

a.sin_family = AF_INET;
a.sin_port = htons(8080);
inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);   /* 返回 1 成功，0 格式不对，-1 出错 */
```

反过来用 `inet_ntop`，它比 `inet_ntoa` 多一个缓冲区参数，是线程安全的：

```c
char ip[INET_ADDRSTRLEN];

inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
```

`inet_ntoa` 返回指向静态缓冲区的指针，两次调用会互相覆盖，多线程下直接错。

### 1.4 一次完整的调用序列

`./net-lab listen` 起一个服务端，另一个终端跑 `./net-lab client <port>`。用 `strace -e trace=network` 抓到的顺序（[完整记录](/p/linux-network-programming/net-syscalls.txt)）：

```
socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, IPPROTO_IP) = 3
setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
bind(3, {sa_family=AF_INET, sin_port=htons(0), sin_addr=inet_addr("127.0.0.1")}, 16) = 0
listen(3, 8) = 0
getsockname(3, {sa_family=AF_INET, sin_port=htons(42251), ...}, [16]) = 0
accept(3, {sa_family=AF_INET, sin_port=htons(56310), ...}, [16]) = 4
recvfrom(4, "hello", 256, 0, NULL, NULL) = 5
sendto(4, "hello", 5, 0, NULL, 0) = 5
```

看完这八行，后面几节都是在给它加细节：

- `SOCK_CLOEXEC` 出现在 `socket` 的 type 里，`strace` 显示为 `SOCK_STREAM|SOCK_CLOEXEC`；
- `bind` 的端口写的是 `htons(0)`，由内核分配，`getsockname` 取回来；
- `accept` 返回**新的** fd（3 是监听 fd，4 是连接 fd）；
- `sendto` 在 TCP 上等价于 `send`，`strace` 统一按系统调用名显示。

## 二、服务端：bind、listen、accept

### 2.1 bind

```c
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

把本地地址绑到 socket 上，返回 0 成功，-1 失败。三种常见写法：

```c
a.sin_addr.s_addr = htonl(INADDR_ANY);   /* 所有网卡，0.0.0.0 */
a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* 只有本机，127.0.0.1 */
inet_pton(AF_INET, "192.168.1.10", &a.sin_addr);  /* 指定网卡 */
```

端口写 0 表示让内核挑一个空闲端口，服务端测试时很方便，用 `getsockname` 取回真实端口：

```c
socklen_t len = sizeof(a);

getsockname(fd, (struct sockaddr *)&a, &len);
printf("port=%u\n", ntohs(a.sin_port));
```

失败的两个常见 `errno`：`EADDRINUSE`（端口被占）、`EACCES`（1024 以下端口没权限，除非有 `CAP_NET_BIND_SERVICE`）。

### 2.2 listen 的 backlog

```c
int listen(int sockfd, int backlog);
```

把 socket 从主动变为被动，开始接受连接。`backlog` 在 Linux 上是**已完成握手队列**的上限，不是"最多连接数"：

- 半连接队列：收到 SYN、还没完成三次握手，长度受 `net.ipv4.tcp_max_syn_backlog` 限制；
- 已完成队列：握手完成、等 `accept` 取走，长度由 `backlog` 与 `net.core.somaxconn` 中较小的一个决定。

所以 `listen(fd, 8)` 的意思是"最多允许 8 个连接排队等我 accept"，第 9 个客户端的握手会被丢弃或延迟。程序里用 8 是为了让 8 个客户端刚好排下。

`backlog` 传 0 或负数不会报错，内核会当成 1 或默认值。要放大就同时调 `somaxconn`。

### 2.3 accept 与 accept4

```c
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
```

`accept` 从已完成队列取一个连接，返回新的 fd，原来的监听 fd 继续用。`addr` 和 `addrlen` 可以传 `NULL`，表示不关心对端地址。

`accept4` 多一个 `flags`，可以传 `SOCK_CLOEXEC` 和 `SOCK_NONBLOCK`，省掉 `fcntl` 两次调用：

```c
int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen,
                  SOCK_CLOEXEC | SOCK_NONBLOCK));
if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)  /* 非阻塞且没连接可拿 */
                return;
        if (errno == EINTR)
                return;                               /* 被信号打断，下次再来 */
        die("accept4");
}
```

三个返回值分支都要处理。非阻塞监听 fd 上 `accept4` 返回 `EAGAIN` 是正常情况，不是错误。

`./net-lab listen` 里打印对端地址：

```
listen fd=3 addr=127.0.0.1:35391
accept4 fd=4 peer=127.0.0.1:51702
recv 5 bytes: hello
```

### 2.4 SO_REUSEADDR 为什么每次都要设

```c
int on = 1;

setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
```

服务端重启时，上一个进程留下的连接处于 `TIME_WAIT`，端口还在被占用，`bind` 会返回 `EADDRINUSE`。`SO_REUSEADDR` 允许绑定这种状态下的地址，是重启调试的必需品。

`SO_REUSEPORT` 是另一回事：允许多个 socket 绑同一个端口，内核在它们之间做负载分担，多进程服务常用。两个选项名字像，作用不同。

## 三、客户端：connect 与地址解析

### 3.1 connect

```c
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

返回 0 表示连接建立，-1 表示失败。阻塞 socket 上，`connect` 会一直等到握手完成或超时（内核默认几十秒到两分钟，取决于 `tcp_syn_retries`）。常见的 `errno`：

| errno | 含义 |
| --- | --- |
| `ECONNREFUSED` | 对端端口没人监听，回了 RST |
| `ETIMEDOUT` | 一直没响应，SYN 重传耗尽 |
| `ENETUNREACH` / `EHOSTUNREACH` | 路由不可达 |
| `EINPROGRESS` | 非阻塞 socket，握手已发起，还没结果 |

### 3.2 非阻塞 connect

非阻塞 fd 上 `connect` 几乎总会返回 -1 且 `errno == EINPROGRESS`，这不代表失败，只代表"在等"。判断结果要用 `poll` 或 `select` 等可写，再用 `getsockopt(SO_ERROR)` 取真正的结果：

```c
int err = 0;
socklen_t len = sizeof(err);

if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        die("getsockopt");
if (err != 0) {                       /* 这里才是真正的失败原因 */
        errno = err;
        die("connect");
}
```

要点：`SO_ERROR` 读一次就被清零，别读两次。

### 3.3 getaddrinfo

```c
#include <netdb.h>

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
```

`node` 是主机名或地址文本，`service` 是端口号或服务名（如 `"http"`）。返回值 0 成功，非 0 是 **getaddrinfo 自己的错误码**，要用 `gai_strerror` 解释，不是 `errno`：

```c
struct addrinfo hints = {0}, *res, *p;

hints.ai_family = AF_UNSPEC;          /* v4 和 v6 都接受 */
hints.ai_socktype = SOCK_STREAM;
rc = getaddrinfo("example.com", "80", &hints, &res);
if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
}
for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
                continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
                break;
        close(fd);
}
freeaddrinfo(res);
```

它返回的是链表，因为一个域名可能对应多个地址（v4、v6、多台机器），要逐个试。`gethostbyname` 已废弃：不支持 IPv6、不可重入、没有超时控制。

## 四、收发：send 与 recv

### 4.1 返回值怎么判

```c
#include <sys/socket.h>

ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
```

返回值是 `ssize_t` 不是 `int`，缓冲区可以超过 2 GB。`recv` 的四种含义：

![recv 返回值的四种含义](/p/linux-network-programming/recv-return.svg)

`send` 少一种：它不会返回 0（除非请求长度为 0），对端关闭时返回 -1 且 `errno == EPIPE`，同时默认还会发一个 `SIGPIPE` 把进程杀掉。

### 4.2 短写：send 返回的字节数可能少于请求

```c
ssize_t n = send(fd, buf, 100000, 0);

if (n < 100000) {
        /* 只送出了一部分，剩下的要自己接着发 */
}
```

阻塞 socket 上，发送缓冲区满了 `send` 会阻塞，所以短写不常见；但被信号打断、或 fd 是非阻塞时，短写很正常。写完整数据要用循环：

```c
static int send_all(int fd, const char *p, size_t len)
{
        size_t off = 0;

        while (off < len) {
                ssize_t n = send(fd, p + off, len - off, 0);

                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                if (n == 0)
                        return -1;
                off += (size_t)n;
        }
        return 0;
}
```

`recv` 相反：它读多少返回多少，不会替你凑满，除非用 `MSG_WAITALL`。

### 4.3 flags 枚举

| flag | 作用 | 什么时候用 |
| --- | --- | --- |
| `MSG_DONTWAIT` | 本次调用非阻塞，不改 fd 状态 | 临时试读一次 |
| `MSG_WAITALL` | 读满请求字节数才返回 | 定长协议，注意仍可能被信号打断 |
| `MSG_PEEK` | 读数据但不从队列取走 | 看一眼是什么再决定怎么读 |
| `MSG_MORE` | 告诉内核后面还有数据，先攒着 | 连续发多个小片段，减少小包 |
| `MSG_NOSIGNAL` | 对端关闭时不发 SIGPIPE | 服务端必须用，或全局忽略 SIGPIPE |

`MSG_NOSIGNAL` 值得单独说：不设它，往已关闭的连接写会触发 `SIGPIPE`，默认动作是终止进程——一个客户端断开就能带走整个服务端。

### 4.4 忽略 SIGPIPE 的两种写法

```c
/* 全局：整个进程都不因 SIGPIPE 退出 */
signal(SIGPIPE, SIG_IGN);

/* 局部：每次 send 都带标志 */
send(fd, buf, len, MSG_NOSIGNAL);
```

服务端推荐第一种，配 `send_all` 里的 `EPIPE` 处理。

## 五、字节流没有消息边界

### 5.1 一次 recv 不等于一条消息

TCP 是字节流。发送端 `send` 两次 10 字节，接收端可能一次 `recv` 读到 20 字节，也可能分三次读到 4、4、12 字节。内核只保证字节顺序，不保证边界。

第一次写网络程序最常见的错误就是：

```c
recv(fd, buf, sizeof(buf), 0);
/* 假设 buf 里正好是一条完整消息 */
```

在本机回环、消息很小、负载很轻时它经常"能用"，一旦换到真实网络就散架。

### 5.2 三种拆包方式

| 方式 | 怎么定边界 | 失效场景 |
| --- | --- | --- |
| 定长 | 双方约定每条消息 N 字节 | 消息长度可变就用不了 |
| 分隔符 | 用 `\n` 或 `\0` 结尾 | 内容里本身就含分隔符，必须转义 |
| 长度前缀 | 先发 4 字节长度，再发内容 | 长度字段本身要定长，且要防超大长度 |

长度前缀最通用，代价是多 4 字节和一次解析。

### 5.3 长度前缀的写法

```c
/* 发送：先发长度，再发内容 */
uint32_t be = htonl((uint32_t)len);

send_all(fd, (char *)&be, 4);
send_all(fd, payload, len);
```

接收端要有状态：先凑够 4 字节，再按长度凑够内容。

```c
/* 读满 n 字节，返回 0 成功，-1 出错或对端关闭 */
static int recv_all(int fd, char *buf, size_t n)
{
        size_t off = 0;

        while (off < n) {
                ssize_t r = recv(fd, buf + off, n - off, 0);

                if (r == 0)
                        return -1;              /* 对端关闭 */
                if (r < 0) {
                        if (errno == EINTR)
                                continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                                return 1;       /* 还没读够，下次继续 */
                        return -1;
                }
                off += (size_t)r;
        }
        return 0;
}
```

非阻塞 + 多路复用时，这个函数不能简单地"循环到读满"——读不够就返回 1，把已读的部分存进连接自己的缓冲区，等下次就绪再续。否则一个慢客户端就能卡住整个事件循环。

## 六、关闭：close、shutdown、SO_LINGER

### 6.1 close 关两个方向

```c
int close(int fd);
int shutdown(int fd, int how);
```

`close` 把 fd 的引用计数减一，减到 0 才真正关闭，同时关掉读和写两个方向。`shutdown` 只关指定方向，fd 本身还能用：

| how | 作用 | 对端看到 |
| --- | --- | --- |
| `SHUT_WR` | 关写方向，发 FIN | `recv` 返回 0 |
| `SHUT_RD` | 关读方向，收到数据直接丢弃 | 无 |
| `SHUT_RDWR` | 两个方向都关 | `recv` 返回 0 |

`SHUT_WR` 的用处是"我说完了，但我还要听你说"。`./net-lab shutdown` 就是这件事（[记录](/p/linux-network-programming/net-shutdown.txt)）：

```
server first recv 4 bytes: ping
server second recv 0 (0 = 对端关了写方向)
client after SHUT_WR: recv 3 bytes: bye
```

客户端先 `send("ping")` 再 `shutdown(SHUT_WR)`，服务端读到 `ping` 之后第二次 `recv` 得到 0。但客户端仍然读到了 `bye`：写方向关了，读方向还开着。

`close` 做不到这一点——它把两个方向一起关，之后就收不到回话了。

### 6.2 SO_LINGER 与 RST

```c
struct linger {
        int l_onoff;     /* 0 = 关闭该选项 */
        int l_linger;    /* 单位秒 */
};
```

`close` 默认的行为是"后台把剩下的数据发完，然后正常挥手"。两个组合会改变它：

| 设置 | close 的行为 |
| --- | --- |
| `l_onoff = 0` | 默认，正常挥手，进程不必等 |
| `l_onoff = 1, l_linger > 0` | 阻塞等待数据发完，最多等 `l_linger` 秒 |
| `l_onoff = 1, l_linger = 0` | 立即丢弃缓冲、发 RST，不挥手 |

`./net-lab rst` 演示第三种（[记录](/p/linux-network-programming/net-rst.txt)）：

```
server recv: -1 Connection reset by peer
```

客户端设 `{1, 0}` 后 `close`，服务端的 `recv` 直接拿到 `ECONNRESET`，连"对端正常关闭"都看不到。这个选项要慎用，它会把还没发出去的数据一起丢掉。

### 6.3 谁先关，谁进 TIME_WAIT

主动关闭的一方在发完最后一个 ACK 后进入 `TIME_WAIT`，持续 2 倍 MSL（Linux 上 60 秒）。这段时间端口不能复用，除非设了 `SO_REUSEADDR`。

所以服务端频繁短连接会出现大量 `TIME_WAIT`，用 `ss -tan state time-wait | wc -l` 能看到。这属于协议行为，不是程序 bug；减少它的办法是复用连接，而不是调小超时。

## 七、socket 选项一览

```c
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
```

| level | optname | 作用 |
| --- | --- | --- |
| `SOL_SOCKET` | `SO_REUSEADDR` | 允许绑定 `TIME_WAIT` 状态的地址 |
| | `SO_REUSEPORT` | 多 socket 绑同一端口，内核分担 |
| | `SO_KEEPALIVE` | 长时间空闲时发探测包 |
| | `SO_RCVBUF` / `SO_SNDBUF` | 收发缓冲区大小（内核会翻倍并夹在上下限之间）|
| | `SO_RCVTIMEO` / `SO_SNDTIMEO` | 阻塞调用的超时 |
| | `SO_LINGER` | close 的行为 |
| | `SO_ERROR` | 取异步错误，读一次清零 |
| `IPPROTO_TCP` | `TCP_NODELAY` | 关掉 Nagle，小包立即发 |
| | `TCP_KEEPIDLE` | keepalive 首次探测的空闲秒数 |
| | `TCP_MAXSEG` | MSS 上限 |

`SO_RCVBUF` 设了不会立刻生效，`getsockopt` 读回来通常是所设值的两倍（内核把它当上限并留余量），这是正常的。

## 八、select

### 8.1 四个宏与一个位图

```c
#include <sys/select.h>

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

void FD_ZERO(fd_set *set);
void FD_SET(int fd, fd_set *set);
void FD_CLR(int fd, fd_set *set);
int FD_ISSET(int fd, fd_set *set);
```

`fd_set` 是一个位图，第 n 位代表 fd n。`FD_SETSIZE` 在 glibc 里是 1024，位图大小就是 1024 位。

四个参数：

- `nfds`：**最大 fd 加一**，不是 fd 个数。内核只扫描 0 到 `nfds-1`。
- `readfds` / `writefds` / `exceptfds`：关心可读、可写、异常的三组集合，可以传 `NULL`。
- `timeout`：`NULL` 表示一直等，`{0,0}` 表示立刻返回（轮询），正数表示最长等这么久。

返回值：就绪 fd 的总数，0 表示超时，-1 表示出错（`EINTR` 是信号打断）。

### 8.2 骨架

```c
while (running) {
        fd_set rfds;
        int maxfd = lfd;

        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        for (i = 0; i < nconn; i++) {
                if (conns[i] < 0)
                        continue;
                FD_SET(conns[i], &rfds);
                if (conns[i] > maxfd)
                        maxfd = conns[i];
        }

        tv.tv_sec = 5;
        tv.tv_usec = 0;

        rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
                if (errno == EINTR)
                        continue;
                die("select");
        }
        if (rc == 0)
                break;                  /* 超时 */
        /* 用 FD_ISSET 逐位查 */
}
```

### 8.3 两个必须知道的坑

**每次调用都要重建集合。** `select` 返回时会把 `fd_set` 改写成"哪些就绪"，原来的关注集合被覆盖。所以 `FD_ZERO` + `FD_SET` 要放在循环里，不能只做一次。`timeval` 也一样，Linux 会把它改成剩余时间。

**fd 不能超过 1023。** `FD_SET(1103, ...)` 会写到数组之外，是未定义行为。这不是理论问题：`./net-lab mux select` 在进程里预先占住 1100 个 fd 后，监听 fd 变成 1103，程序直接报错退出（[记录](/p/linux-network-programming/net-fds-select.txt)）：

```
listen fd=1103 addr=127.0.0.1:44947
fd 1103 超过 FD_SETSIZE=1024
```

同一个进程换成 `poll`，同样的 fd 号能正常工作（[记录](/p/linux-network-programming/net-fds-poll.txt)）：

```
listen fd=1103 addr=127.0.0.1:41771
mode=poll conns=2 accepted=2 bytes=14 loops=4
```

`select` 的 1024 是编译期常量，改它要重编 glibc。生产环境里 fd 号上千很常见，所以 `select` 早就不适合做服务端主循环。

## 九、poll

### 9.1 结构体与参数

```c
#include <poll.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

struct pollfd {
        int   fd;       /* 关心的 fd，传负数表示忽略这一项 */
        short events;   /* 输入：关心哪些事件 */
        short revents;  /* 输出：实际发生了哪些 */
};
```

三个参数：数组、数组长度、超时毫秒数（-1 表示一直等，0 表示立刻返回）。返回值是 `revents` 非 0 的项数，0 超时，-1 出错。

与 `select` 的三个区别：

- 没有 1024 限制，数组多大都行；
- `events` 是输入、`revents` 是输出，两个字段分开，**不用每次重设**；
- 仍然是线性扫描：内核每次都遍历整个数组，fd 多时是 O(n)。

### 9.2 事件枚举

| 事件 | 含义 |
| --- | --- |
| `POLLIN` | 有数据可读 |
| `POLLOUT` | 可以写 |
| `POLLPRI` | 带外数据 |
| `POLLRDHUP` | 对端关闭了写方向（需 `_GNU_SOURCE`）|
| `POLLERR` | 出错，只在 `revents` 里出现 |
| `POLLHUP` | 挂断，只在 `revents` 里出现 |
| `POLLNVAL` | fd 无效，只在 `revents` 里出现 |

后三个不需要在 `events` 里申请，只要发生了就会置位，所以判断时别只查 `POLLIN`：

```c
if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
        handle(pfd[i].fd);
```

### 9.3 骨架

```c
struct pollfd pfd[MAXCONN + 1];
int cnt = 1;

pfd[0].fd = lfd;
pfd[0].events = POLLIN;

while (running) {
        rc = poll(pfd, (nfds_t)cnt, 5000);
        if (rc < 0) {
                if (errno == EINTR)
                        continue;
                die("poll");
        }
        if (rc == 0)
                break;
        if (pfd[0].revents & POLLIN) {
                int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd >= 0) {
                        pfd[cnt].fd = cfd;
                        pfd[cnt].events = POLLIN;
                        cnt++;
                }
        }
        for (i = 1; i < cnt; i++) {
                if (pfd[i].fd < 0)
                        continue;
                if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                        continue;
                handle(pfd[i].fd);
        }
}
```

关闭连接时把 `pfd[i].fd` 置 -1，`poll` 会跳过它，不用压缩数组。

## 十、epoll

### 10.1 三个函数

```c
#include <sys/epoll.h>

int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
```

- `epoll_create1(EPOLL_CLOEXEC)` 返回一个 epoll 实例的 fd；
- `epoll_ctl` 增删改关注列表，`op` 是 `EPOLL_CTL_ADD`、`EPOLL_CTL_MOD`、`EPOLL_CTL_DEL`；
- `epoll_wait` 只返回**就绪的** fd，`maxevents` 是这次最多取回几个。

和 `poll` 的本质区别：关注列表放在内核里，只增删一次；每次 `epoll_wait` 不用把整个集合拷进去，也不用扫描全部，只取就绪链表。

### 10.2 epoll_event 与事件位

```c
struct epoll_event {
        uint32_t     events;    /* 关心的事件，也是返回的事件 */
        epoll_data_t data;      /* 用户数据，常放 fd 或指针 */
};
```

```c
struct epoll_event ev;

memset(&ev, 0, sizeof(ev));
ev.events = EPOLLIN;
ev.data.fd = fd;
epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
```

`data` 是个联合体，可以放 `fd`、`ptr`、`u32`、`u64`。放 `fd` 最省事；要区分连接对象时放指针，省掉一次查表。

| 事件位 | 含义 |
| --- | --- |
| `EPOLLIN` | 可读 |
| `EPOLLOUT` | 可写 |
| `EPOLLRDHUP` | 对端关闭写方向 |
| `EPOLLPRI` | 带外数据 |
| `EPOLLERR` / `EPOLLHUP` | 出错、挂断，总是会被报告 |
| `EPOLLET` | 边缘触发 |
| `EPOLLONESHOT` | 只报一次，要重新 `MOD` 才再报 |
| `EPOLLEXCLUSIVE` | 多个 epoll 等同一个 fd 时只唤醒一个 |

### 10.3 水平触发与边缘触发的写法差别

默认是水平触发（LT）：只要缓冲区里还有数据，每次 `epoll_wait` 都会报。写起来简单，读一次不够下次还会被叫醒。

边缘触发（ET）只在"状态变化"时报一次：从没数据到有数据。所以一次通知必须把数据读空：

```c
static int drain_once(int fd, struct stat_s *st)
{
        char buf[BUFSZ];

        for (;;) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);

                if (n > 0) {
                        st->bytes += n;
                        if (send_all(fd, buf, (size_t)n) < 0)
                                break;
                        continue;
                }
                if (n == 0)
                        break;                  /* 对端关闭 */
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return 0;               /* 读空了，等下次 */
                if (errno == EINTR)
                        continue;
                break;
        }
        close(fd);
        st->done++;
        return -1;
}
```

三个条件缺一不可：`EPOLLET`、fd 非阻塞、循环读到 `EAGAIN`。少任何一个都会挂住——ET 只报一次，读不完的部分再也不会通知。

`./net-lab mux epoll` 与 `mux epoll-et` 跑同一负载（8 个连接各一条消息）：

```
mode=epoll conns=8 accepted=8 bytes=56 loops=16
mode=epoll-et conns=8 accepted=8 bytes=56 loops=18
```

这个负载下两者差不多：每条消息都是"从空到有"，没有重复通知可省。ET 省的是"一次没读完"时的重复唤醒，代价是必须写对循环。这个道理在[《Linux 网络栈（二）》](/p/linux-network-socket-transport/)第 5.2 节有更细的对照。

### 10.4 四个常见错法

**重复 ADD。** 同一个 fd 对同一个 epoll 实例 `EPOLL_CTL_ADD` 两次，第二次返回 `EEXIST`。想改事件用 `EPOLL_CTL_MOD`。

**ET 没配非阻塞。** ET 要求读到 `EAGAIN` 才算读完，阻塞 fd 读到没数据就永远阻塞在 `recv` 里，整个事件循环停住。

**忘记 DEL 已经关闭的 fd。** `close(fd)` 之后它自动从 epoll 集合里移除，但如果有 `dup`、或者 fd 号被新连接复用，行为会很微妙。规范做法是关闭前先 `EPOLL_CTL_DEL`。

**用 `data.fd` 但 fd 已被复用。** 关闭连接后 fd 号会被新连接拿走，如果事件队列里还留着旧事件，`data.fd` 就指向了新连接。用指针指向连接对象、并在关闭时标记失效，可以避免这类错乱。

## 十一、io_uring

### 11.1 三个系统调用与 liburing

io_uring 的接口是三个系统调用：

```c
int io_uring_setup(unsigned entries, struct io_uring_params *p);
int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                   unsigned flags, sigset_t *sig);
int io_uring_register(int fd, unsigned opcode, void *arg, unsigned nr_args);
```

直接用它们要自己 `mmap` 提交环和完成环、自己管 head/tail 指针，代码量很大。实际用 liburing 包装：

```c
#include <liburing.h>

int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags);
struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring);
void io_uring_prep_accept(struct io_uring_sqe *sqe, int fd, ...);
int io_uring_submit(struct io_uring *ring);
int io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe);
void io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe);
void io_uring_queue_exit(struct io_uring *ring);
```

### 11.2 一次操作的完整生命周期

![SQE 与 CQE 的生命周期](/p/linux-network-programming/uring-lifecycle.svg)

五步，每个请求都走一遍：

1. `io_uring_get_sqe()` 从提交环拿一个空位；
2. `io_uring_prep_xxx()` 把操作填进去，设置 `user_data` 做标记；
3. `io_uring_submit()` 告诉内核"有几项要处理"；
4. `io_uring_wait_cqe()` 取一个完成事件，`cqe->res` 就是这次操作的结果（等同于系统调用返回值）；
5. `io_uring_cqe_seen()` 把这个完成事件归还给内核，否则环会满。

和 `epoll` 最大的区别：`epoll` 是"你问内核哪些 fd 就绪了"，`io_uring` 是"你把要做的事排进队列，内核做完把结果放回另一个队列"。读写不再需要单独的系统调用。

### 11.3 一个 accept → recv → send 的循环

```c
#define UD_ACCEPT       0ULL
#define UD_RECV(fd)     ((uint64_t)(fd))
#define UD_SEND(fd)     ((uint64_t)(fd) | (1ULL << 32))

while (st.done < n) {
        struct io_uring_sqe *sqe;
        struct io_uring_cqe *cqe;

        if (st.accepted < n && !pending_accept) {
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(sqe, lfd, NULL, NULL, SOCK_CLOEXEC);
                sqe->user_data = UD_ACCEPT;
                io_uring_submit(&ring);
                pending_accept = 1;
        }

        if (io_uring_wait_cqe(&ring, &cqe) < 0)
                break;
        /* 按 user_data 分发：accept / recv / send */
        io_uring_cqe_seen(&ring, cqe);
}
```

`user_data` 是唯一能区分"这个完成事件是哪个操作"的东西，所以要在提交时打好标记。程序里用一个整数的高位区分 recv 和 send，低位存 fd。

`./net-lab mux uring` 的结果：

```
mode=uring conns=8 accepted=8 bytes=56 loops=24
```

`loops=24` 不是 16，因为这里的计数是"完成事件个数"：8 次 accept、8 次 recv、8 次 send。它和 `epoll_wait` 的返回次数不是同一个东西，不能直接比。

### 11.4 两个容易写错的地方

**循环条件要自己判。** `io_uring_wait_cqe` 在没有完成事件时会一直等，它不会返回一个"没有事件"的 0 告诉你该退出了。程序的第一版写成 `for (;;)`，所有连接处理完仍然卡在 `wait_cqe` 里不退出，改成 `while (st.done < n)` 才对。

**提交和完成要成对。** 每个 `get_sqe` 拿到的位置最终都要 `submit`，每个取到的 `cqe` 都要 `seen`。漏一次 `seen`，完成环的 head 不前进，环满了之后 `wait_cqe` 就永远拿不到新事件。

## 十二、四种写法比较

### 12.1 接口形状

| | `select` | `poll` | `epoll` | `io_uring` |
| --- | --- | --- | --- | --- |
| 关注集合在哪 | 调用方栈上的位图 | 调用方数组 | 内核里的关注列表 | 用户与内核共享的环 |
| 每次等待做什么 | 拷集合进内核、遍历、返回后逐位查 | 拷数组、遍历、查 `revents` | 只取就绪项 | 提交请求、取完成事件 |
| fd 上限 | `FD_SETSIZE`，1024 | 无 | 无 | 无 |
| 集合要重设吗 | 要，每次 | 不要，`events` 不被改写 | 不要，`ctl` 一次 | 不要 |
| 返回值 | 就绪 fd 总数 | `revents` 非 0 的项数 | 就绪事件数 | 完成事件，`res` 是操作结果 |
| 触发方式 | 水平 | 水平 | 水平 / 边缘 | 完成即通知 |
| 一次能提交几个操作 | 一次等待 | 一次等待 | 一次等待 | 一批 SQE |

### 12.2 怎么选

- **学习、调试、连接数很少**：`poll`。接口最简单，没有 1024 限制，比 `select` 少两个坑。
- **常规服务端**：`epoll` 水平触发。写起来最省心，读一次不够下次还会报，不会因为漏读而挂住。
- **高并发、要求少唤醒**：`epoll` 边缘触发，但必须配非阻塞和读空循环，写错就是连接挂死。
- **要减少系统调用、或需要批量提交**：`io_uring`。它把读写也变成队列操作，一次 `enter` 可以提交几十个请求。代价是代码复杂度和内核版本要求。

### 12.3 三点要记住

第一，多路复用省的是**每连接一个线程**的成本，不是让 `read` 变快。数据从内核到用户缓冲区的那次复制一次都不会少，[《Linux 网络栈（二）》](/p/linux-network-socket-transport/)第 5.4 节用同一负载量过：单线程 `epoll` 与 101 个线程的吞吐差别主要来自调度，不来自系统调用次数。

第二，四种接口的差别在"怎么知道哪个 fd 就绪"，不在数据路径。选错接口不会让协议栈少处理一个包。

第三，`io_uring` 改变的也是调用方式：把"每次读写进一次系统调用"变成"把请求排进环里"。它不跳过协议栈，也不减少内存拷贝，除非配合注册缓冲区。

## 十三、命令与接口速查

| 目的 | 接口或命令 |
| --- | --- |
| 建 socket | `int socket(int domain, int type, int protocol)` |
| 绑地址 | `int bind(int fd, const struct sockaddr *addr, socklen_t len)` |
| 开始监听 | `int listen(int fd, int backlog)`，backlog 是已完成队列上限 |
| 取连接 | `int accept(int fd, struct sockaddr *addr, socklen_t *len)` |
| 取连接并设标志 | `int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)` |
| 发起连接 | `int connect(int fd, const struct sockaddr *addr, socklen_t len)` |
| 收发 | `ssize_t send(int fd, const void *buf, size_t len, int flags)`、`recv` |
| 半关闭 | `int shutdown(int fd, int how)`，`SHUT_WR` 只关写 |
| 地址转换 | `inet_pton`、`inet_ntop`、`htons`、`ntohs` |
| 域名解析 | `getaddrinfo`、`freeaddrinfo`、`gai_strerror` |
| 选项 | `setsockopt(fd, level, optname, &val, sizeof(val))` |
| select | `select(nfds, &rfds, &wfds, &efds, &tv)`，`nfds` 是最大 fd 加一 |
| poll | `poll(struct pollfd *fds, nfds_t n, int timeout)` |
| epoll | `epoll_create1`、`epoll_ctl(ep, op, fd, &ev)`、`epoll_wait` |
| io_uring | `io_uring_queue_init`、`io_uring_get_sqe`、`io_uring_submit`、`io_uring_wait_cqe`、`io_uring_cqe_seen` |
| 看连接状态 | `ss -tan`、`ss -tan state time-wait`、`ss -ltn` |
| 看 socket 缓冲 | `ss -tmn`（`skmem` 一行） |
| 跟踪网络系统调用 | `strace -f -e trace=network ./prog` |
| 抓包 | `tcpdump -i lo -nn port 8080` |

## 十四、名词解释

- **fd**：文件描述符，进程内的小整数，指向内核里的 socket 对象。
- **backlog**：`listen` 的第二个参数，已完成三次握手、等待 `accept` 取走的连接队列上限。
- **短写**：`send` 只送出请求的一部分，剩下的要再调一次。
- **半关闭**：只关一个方向，`shutdown(SHUT_WR)` 后仍能读。
- **水平触发**：只要还有数据就反复通知，`select`、`poll`、`epoll` 默认都是。
- **边缘触发**：只在状态变化时通知一次，必须一次读空。
- **`TIME_WAIT`**：主动关闭方发完最后一个 ACK 后的状态，持续 2 倍 MSL。
- **`SO_ERROR`**：异步错误队列的读取入口，读一次清零。
- **SQE / CQE**：io_uring 的提交队列条目与完成队列条目。
- **`user_data`**：SQE 里的一个 64 位字段，完成时原样带回，用来区分是哪个请求。
