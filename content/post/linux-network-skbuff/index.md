---
title: "Linux 网络栈（三）：sk_buff 的数据视图、共享与释放"
slug: linux-network-skbuff
date: "2026-11-22T16:03:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "从 headroom 到页片段：边界怎么动、共享怎么算、什么时候不得不真复制，以及为什么抓包看到的长度不等于一个 skb 的长度。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

每加一层头就要复制一份数据吗？抓包里的一个大包，是不是设备在线上发出的大帧？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · **3** · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

第一篇第三章只回答了一件事：一次收发里，字节只在两个边界上各复制一次，中间都在原地挪指针。这一篇把"原地"摊开——边界落在哪几个字段上，共享的是什么，什么时候还是得真复制。块 B 加头、块 C 交给设备、块 D 收上来，用的都是这套几何。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`，下面的字段摘录和链接都按这个 tag。

## 一、描述对象与数据存储

![skb 的元数据、线性缓冲区与非线性页片段](/p/linux-network-skbuff/geometry.svg)

三块要分开看：skb 本身记长度、协议、设备和位置；线性区域放连续可访问的数据；非线性部分通过页片段描述别处的存储。内核不要求一份网络数据完整放在一段连续内存里。

### 1.1 四个边界与两个长度

`include/linux/skbuff.h` 里的相关字段，中间的成员省略：

```c
unsigned int len, data_len;
sk_buff_data_t tail;
sk_buff_data_t end;
unsigned char *head, *data;
unsigned int truesize;
refcount_t users;
```

| 字段 | 记住这条关系就够了 |
| --- | --- |
| `head` | 线性区域起点 |
| `data` | 当前数据视图起点 |
| `tail` | 线性有效数据的末端 |
| `end` | 线性区域可用空间的末端 |
| `len` | 总数据长度，含非线性部分 |
| `data_len` | 非线性部分长度，线性部分等于 `len - data_len` |
| `truesize` | 内存记账量，不是报文有效字节数 |

`tail` 和 `end` 的类型是 `sk_buff_data_t`。64 位内核上 `NET_SKBUFF_DATA_USES_OFFSET` 恒被定义（条件是 `BITS_PER_LONG > 32`），所以这两个字段是相对 `head` 的偏移；32 位才有指针那一支。因此图里的四个箭头不能直接抄成四个 C 指针，访问要走 `skb_tail_pointer()` 这类 helper。字段定义和 helper 都在 [skbuff.h](https://github.com/torvalds/linux/blob/v7.0/include/linux/skbuff.h)。

`truesize` 记的不是有效字节。v7.0 里它在 `__finalize_skb_around()` 中一次算定：

```c
size -= SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
skb->truesize = SKB_TRUESIZE(size);
```

`SKB_TRUESIZE(X)` 展开是 `X + SKB_DATA_ALIGN(sizeof(struct sk_buff)) + SKB_DATA_ALIGN(sizeof(struct skb_shared_info))`，三部分都要按缓存行对齐。描述对象本身多大，本机能直接看：

```bash
grep skbuff_head_cache /proc/slabinfo
```

```text
skbuff_head_cache    847    848    256   16    1 : tunables    0    0    0 : slabdata     53     53      0
```

第四列 `256` 就是对齐后的 `sizeof(struct sk_buff)`，第二列是当前活跃对象数。也就是说，一个 100 字节的包，记账要算上 256 字节的元数据、一段 `skb_shared_info` 和按缓存行取整的余量，量级是有效字节的好几倍。小包场景下按"包长乘个数"估内存，永远偏低。

### 1.2 预留空间与边界移动

举一个纯演算的例子：线性可用空间 128 字节，空 skb 先预留 32 字节，放 20 字节负载，再前置 8 字节头部。

| 操作 | `data` | `tail` | `len` | 发生了什么 |
| --- | ---: | ---: | ---: | --- |
| 空缓冲区 | 0 | 0 | 0 | 没有有效数据 |
| `skb_reserve(skb, 32)` | 32 | 32 | 0 | 在头部留出余量 |
| `skb_put(skb, 20)` | 32 | 52 | 20 | 扩大尾部，调用方填内容 |
| `skb_push(skb, 8)` | 24 | 52 | 28 | 向前扩大视图，调用方填头部 |
| `skb_pull(skb, 8)` | 32 | 52 | 20 | 越过刚才的头部 |

`push` 和 `pull` 只改边界和长度，那 20 字节负载没有动过。前提是空间、线性可访问范围和所有权都满足：headroom 不够要扩容，数据被共享又要改写要先克隆，头部不线性要先线性化。这些条件不满足时，代价就回来了。

## 二、共享的两种对象

### 2.1 users 与数据引用

![两个 skb 描述对象可以共享同一缓冲区，改写前需满足所有权要求](/p/linux-network-skbuff/sharing.svg)

`skb->users` 管的是 skb 这个描述对象本身的引用。`skb_clone()` 造出的是另一个描述对象，两者指向同一份数据存储，而这份存储还有自己的引用计数——`skb_shared_info` 里的 `dataref`。一个是"描述对象还剩几个引用"，一个是"这份数据还能不能被改写"。

这是两个问题：这个描述对象能不能释放，和这份共享数据能不能释放或改写。TCP 为重传留数据、协议栈要分出不同视图，靠的都是后者省下的一次整份复制。共享不等于随便改——头部可写性和数据引用还要再判断一次。实现看 [skbuff.c](https://github.com/torvalds/linux/blob/v7.0/net/core/skbuff.c)。

这一节讲的是机制，本站没有对 `users` 和 `dataref` 做直接观测：它们没有现成的 tracepoint，`kfree_skb` 只报"谁被释放了"，不报还剩几个引用。所以本篇能拿出的硬证据只有第三章那条释放事件，共享关系靠读源码对照。

### 2.2 存储的三个来源

`alloc_skb()` 把 skb 和它的缓冲区一起分配出来，是最常见的一条路。`build_skb()` 一类接口把已有、布局合适的缓冲区挂上去，不再新分配数据区。非线性片段可以指向某个页里的一段范围——所以"一份 payload 必定经过一次 `kmalloc()`"不成立。

头部的分配也分了两条：小的头区优先走 `skbuff_small_head` 这块专用缓存，本机 `/proc/slabinfo` 里它的 objsize 是 704，超过这个尺寸才回落到通用的 kmalloc 缓存。把 1.1 那条 `grep skbuff_head_cache` 放宽成 `grep -E '^skbuff' /proc/slabinfo`，两行并排就能看到 256 和 704——描述对象和它的数据区各记各的账。

驱动还会用 page_pool 复用接收缓冲页，省掉反复分配和 DMA 映射。页池是存储复用设施，skb 是网络数据描述，两者不在一个层次上。页池存在也不等于应用能零拷贝拿到数据。接口说明见 [Page Pool 文档](https://docs.kernel.org/7.0/networking/page_pool.html)。

## 三、一次 NO_SOCKET 丢弃

### 3.1 跟踪实例与事件行

用第一篇的直连拓扑，但 B 的 19999 端口故意不启动接收端。

```bash
sudo bash lab.sh clean
sudo bash lab.sh direct
```

下面这一整块一次贴进终端。用 `sudo bash -c` 而不是先 `sudo -i`：`sudo -i` 会另起一个 shell，后面粘进去的行有概率被密码提示吃掉，屏幕上就只剩"什么都没执行"的样子；`bash -c` 只有一个 shell，步骤顺序就是字面顺序。

```bash
sudo bash -c '
rmdir /sys/kernel/tracing/instances/nslab 2>/dev/null
mkdir /sys/kernel/tracing/instances/nslab
cd /sys/kernel/tracing/instances/nslab
echo 1 > events/skb/kfree_skb/enable
ip netns exec nslab-a /home/lxj/nslab/netdemo udp-client 10.77.1.2 19999; echo "client exit=$?"
sleep 0.5
echo 0 > events/skb/kfree_skb/enable
grep -c "reason: NO_SOCKET" trace
grep "reason: NO_SOCKET" trace
'
```

`/home/lxj/nslab` 换成第一篇下载六个文件的目录，`netdemo` 就是那里编译出来的那个程序。正常情况下这一段会按顺序给出四样东西：

| 输出 | 说明 |
| --- | --- |
| `send: Connection refused` | 第二个数据报没发出去，见下 |
| `client exit=1` | 客户端以非零码结束，同样不是故障 |
| `1` 或 `2` | `grep -c` 数到几条 `kfree_skb` 事件 |
| 一行 `kfree_skb: ... reason: NO_SOCKET` | 要的那条证据 |

任何一步没有回话，就停在原地查，别接着往下跑——否则后面的 `grep` 读的是空文件，看起来像"命令没执行"。

`udp-client` 发两个数据报，第一个正常出去，B 上没有监听者，B 的协议栈回一个 ICMP 端口不可达；第二个数据报还没离开本机，这次 `send` 就直接拿到 `ECONNREFUSED`，程序按约定退出码 1 结束。所以退出码非零是"端口没人收"的直接后果，不是环境搭错了。

这个先后关系取决于 ICMP 回来的快慢：如果第二个数据报赶在错误回来之前也发出去了，`grep -c` 会是 2，事件行也多一条，结论不变——两次都是同一路径上的同一种丢弃。

`sleep 0.5` 是给接收路径留一点时间：报文在软中断里被处理、事件写进 ring buffer，都需要一点时刻，写完立刻关 `enable` 有概率抢在它前面。`grep -c` 先给条数，`grep` 再给原文，两行合起来才说明"抓到了几条、长什么样"。这次是 1 条：

```text
         netdemo-54771   [000] ..s1. 79957.001577: kfree_skb: skbaddr=00000000b66a5999 rx_sk=0000000000000000 protocol=2048 location=__udp4_lib_rcv+0x4f0/0x850 reason: NO_SOCKET
```

`grep -c` 输出 0 就说明这条证据没拿到，按顺序查三件事：拓扑是不是 `direct` 且 `a0` 是 up 的（`ip netns exec nslab-a ping -c 1 10.77.1.2` 能通）；`enable` 是不是真写进去了（`cat events/skb/kfree_skb/enable` 应为 1）；B 的 19999 是不是确实没人监听（`ip netns exec nslab-b ss -lun` 里不该有这个端口）。

| 字段 | 能说明什么 |
| --- | --- |
| `skbaddr` | 被记录的 skb 身份；地址可能被内核格式化隐藏 |
| `rx_sk` | 接收侧关联的 socket；这里是 0，正对应"没找到" |
| `protocol=2048` | 2048 即 `0x0800`，IPv4 以太类型 |
| `location` | 丢弃发生的位置，这里在 UDP 接收函数里 |
| `NO_SOCKET` | 丢弃原因：找不到接收 socket |

`location` 后面那个 `+0x4f0/0x850` 是函数内偏移和函数长度，随内核构建与编译器变化；换台机器或换个内核版本，这两个数字会不一样，函数名 `__udp4_lib_rcv` 才是能跨版本对照的部分。

"端口没监听"这个条件、UDP 接收的位置、`NO_SOCKET` 这个原因，三者一起才构成证据。只看某个释放计数在涨，说明不了同样的事。

跟踪窗口开着的这半秒里，别的路径也可能往同一个 buffer 写事件——这轮就混进一条 IPv6 无路由的丢弃（`protocol=34525`，`reason: IP_INNOROUTES`，`location=ip6_pkt_drop.isra.0`），来源是本机发出的路由器请求在 B 上没有去处。所以这里用 `reason` 过滤，而不是把 buffer 里的第一行当结论。完整记录和事件格式见[事件文本](/p/linux-network-skbuff/skb-trace.txt)和[字段格式](/p/linux-network-skbuff/skb-format.txt)。

清理：

```bash
sudo bash -c 'cd /; rmdir /sys/kernel/tracing/instances/nslab'
sudo bash lab.sh clean
```

### 3.2 这条证据的边界

正常处理结束也会释放 skb，丢弃还发生在别处。`kfree_skb` 覆盖不到所有消费路径，对象地址也不能把克隆、分段、聚合之后的全部关系自动串起来。

这个实验只证明一件事：端口关闭这个特定条件下，UDP 接收侧出现了一个 `NO_SOCKET` 事件。源码对照 [udp.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/udp.c) 和[事件定义](https://github.com/torvalds/linux/blob/v7.0/include/trace/events/skb.h)。

## 四、对象数与帧数为什么不一致

一次写、一个数据报，各自数一遍帧。还是直连拓扑：

```bash
sudo bash lab.sh clean
sudo bash lab.sh direct
```

TCP 先来，接收端：

```bash
sudo ip netns exec nslab-b /home/lxj/nslab/netdemo tcp-server 10.77.1.2 18084 0
```

另一个终端在 `nslab-a` 里抓包，再写 1 MB：

```bash
sudo ip netns exec nslab-a tcpdump -nn -i a0 -c 200 'tcp and port 18084'
sudo ip netns exec nslab-a /home/lxj/nslab/netdemo tcp-client 10.77.1.2 18084 1000000
```

发送侧抓到 131 个带数据的 TCP 帧，长度加起来正好 1000000 字节；其中 122 个超过 1500 的 MTU，最长 65160 字节。MTU 是 1500，帧却有 65 KB——抓包看到的是内核交给设备之前的大对象，不是线上真正跑的小帧。这就是 GSO。

UDP 换个数据报。接收端改成 `udp-server`，抓包这次不加过滤：

```bash
sudo ip netns exec nslab-b /home/lxj/nslab/netdemo udp-server 10.77.1.2 19999 100 0
sudo ip netns exec nslab-a tcpdump -nn -i a0
sudo ip netns exec nslab-a python3 -c 'import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);print(s.sendto(b"x"*60000,("10.77.1.2",19999)))'
```

同样 60000 字节，这次是 41 个帧，每个都不超过 1500：第一片带着 UDP 头（`UDP, length 60000`），后面 40 片只剩 `ip-proto-17`。单个大 UDP 数据报没有 TCP 那样的分段卸载可用，只能按 MTU 分成 IP 分片，接收端再拼回去——服务端打印的仍是 `len=60000`。

这里有个容易踩的坑：抓包时若加 `'udp and port 19999'`，只会数到 1 个帧，后面 40 片没有 UDP 头，过滤条件不匹配。数帧就不要带上层过滤。

校验和同理。给抓包加上 `-vv`，同一批数据帧里打出来的是：

```text
Flags [P.], cksum 0x26c3 (incorrect -> 0x5939), seq 4096:8192, ... length 4096
```

不是包坏了，是卸载把校验和的计算推到了后面，而抓包点在它之前。

| 机制 | 改变什么 | 观察上的后果 |
| --- | --- | --- |
| GSO | 大对象延后分段 | 发送侧能看见超过单帧大小的对象（1 MB 写：131 帧里 122 帧超 MTU） |
| IP 分片 | 超过 MTU 的报文在 IP 层切开 | 一个数据报变成多个帧，只有第一片带上层头 |
| GRO | 接收侧先合并再上交 | 一个上层对象对应多个接收帧 |
| 校验和卸载 | 校验推给后续阶段或设备 | 本地抓包可能看到还没填完的校验和 |
| 克隆与重传 | 多个描述或多次发送指向同一份字节 | 对象数、发送次数与业务字节数不一一对应 |

第一、二、四行这轮都量到了，成本就是两条命令加一个 `python3` 的 `sendto`。GRO 和克隆没量：GRO 的效果要在接收侧看，veth 上合并的规模不明显；`users` 和 `dataref` 没有现成的 tracepoint，得挂钩子，不在本篇范围内。

这些机制不改变协议语义，改变的是内核处理和工具观察的单位。要问"到底发了几个包"，得先说清问的是应用写了几次、几条抓包记录，还是几个设备帧——在 UDP 那个例子里，三者分别是 1 次、41 条（带 UDP 过滤时 1 条）、41 个。两段抓包的原始输出在[帧数与卸载记录](/p/linux-network-skbuff/skb-frames.txt)。

## 五、命令与接口速查

| 接口 / 路径 | 用途 |
| --- | --- |
| `skb_reserve/push/pull/put` | 预留空间、前后调整边界 |
| `skb_clone()` | 造一个共享数据的描述对象 |
| `events/skb/kfree_skb` | 观察 skb 释放或丢弃事件 |
| `instances/NAME` | 把跟踪配置放进专属实例，不动全局设置 |
| `/proc/slabinfo` | skb 元数据缓存与头区缓存的 objsize、活跃对象数 |
| `ethtool -k DEV` / `ethtool -K DEV …` | 看、改分段与校验和卸载能力 |
| `tcpdump -nn -i IF`（不加过滤） | 数帧；带 `udp and port` 会漏掉后续分片 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| headroom / tailroom | 线性有效数据前后的可用余量 |
| 非线性数据 | 不在线性区域里连续存放的那部分 |
| page_pool | 面向接收等场景的页复用设施 |
| 线性化 | 把需要的数据变成连续可访问区域的操作 |
| `users` | skb 描述对象自身的引用计数，与数据存储的引用是两回事 |
| `dataref` | `skb_shared_info` 里的数据区引用计数，决定这份字节还能不能被改写 |
| GSO | 发送侧把大对象推迟到交给设备前再分段，抓包会看到超过 MTU 的帧 |
| IP 分片 | 报文超过 MTU 时在 IP 层切开，只有第一片带上层协议头 |
