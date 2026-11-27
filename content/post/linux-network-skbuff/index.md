---
title: "Linux 网络栈（三）：sk_buff 的数据视图、共享与释放"
slug: linux-network-skbuff
date: "2026-11-22T16:03:00+08:00"
lastmod: "2026-11-27"
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

`tail` 和 `end` 的类型是 `sk_buff_data_t`。开着 `NET_SKBUFF_DATA_USES_OFFSET` 时它们是相对 `head` 的偏移；另一个分支里才是指针。所以图里的四个箭头不能直接抄成四个 C 指针，访问要走 `skb_tail_pointer()` 这类 helper。字段定义和 helper 都在 [skbuff.h](https://github.com/torvalds/linux/blob/v7.0/include/linux/skbuff.h)。

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

`skb->users` 管的是 skb 这个描述对象本身的引用。`skb_clone()` 造出的是另一个描述对象，两者指向同一份数据存储，而这份存储还有自己的引用计数。

这是两个问题：这个描述对象能不能释放，和这份共享数据能不能释放或改写。TCP 为重传留数据、协议栈要分出不同视图，靠的都是后者省下的一次整份复制。共享不等于随便改——头部可写性和数据引用还要再判断一次。实现看 [skbuff.c](https://github.com/torvalds/linux/blob/v7.0/net/core/skbuff.c)。

### 2.2 存储的三个来源

`alloc_skb()` 把 skb 和它的缓冲区一起分配出来，是最常见的一条路。`build_skb()` 一类接口把已有、布局合适的缓冲区挂上去，不再新分配数据区。非线性片段可以指向某个页里的一段范围——所以"一份 payload 必定经过一次 `kmalloc()`"不成立。

驱动还会用 page_pool 复用接收缓冲页，省掉反复分配和 DMA 映射。页池是存储复用设施，skb 是网络数据描述，两者不在一个层次上。页池存在也不等于应用能零拷贝拿到数据。接口说明见 [Page Pool 文档](https://docs.kernel.org/7.0/networking/page_pool.html)。

## 三、一次 NO_SOCKET 丢弃

### 3.1 跟踪实例与事件行

用第一篇的直连拓扑，但 B 的 19999 端口故意不启动接收端。另开一个 root shell，建一个专属跟踪实例：

```bash
sudo -i
cd /home/lxj/nslab                       # 第一篇下载五个文件的目录，换成自己的
mkdir /sys/kernel/tracing/instances/nslab
cd /sys/kernel/tracing/instances/nslab
echo 1 > events/skb/kfree_skb/enable
ip netns exec nslab-a /home/lxj/nslab/netdemo udp-client 10.77.1.2 19999
echo 0 > events/skb/kfree_skb/enable
grep 'reason: NO_SOCKET' trace
```

`netdemo` 就是第一篇编译出来的那个程序。往没有监听者的 UDP 端口发，后续发送可能收到异步错误，命令本身不保证一定成功；这个实验只看第一份数据到达接收侧的结果。

筛出来的那一行：

```text
         netdemo-1568850 [000] ..s1. 731777.685704: kfree_skb: skbaddr=000000004c32bef1 protocol=2048 location=__udp4_lib_rcv+0x440/0x738 reason: NO_SOCKET
```

| 字段 | 能说明什么 |
| --- | --- |
| `skbaddr` | 被记录的 skb 身份；地址可能被内核格式化隐藏 |
| `protocol=2048` | 2048 即 `0x0800`，IPv4 以太类型 |
| `location` | 丢弃发生的位置，这里在 UDP 接收函数里 |
| `NO_SOCKET` | 丢弃原因：找不到接收 socket |

"端口没监听"这个条件、UDP 接收的位置、`NO_SOCKET` 这个原因，三者一起才构成证据。只看某个释放计数在涨，说明不了同样的事。完整记录和事件格式见[事件文本](/p/linux-network-skbuff/skb-trace.txt)和[字段格式](/p/linux-network-skbuff/skb-format.txt)。

清理：

```bash
cd /
rmdir /sys/kernel/tracing/instances/nslab
exit
sudo bash lab.sh clean
```

### 3.2 这条证据的边界

正常处理结束也会释放 skb，丢弃还发生在别处。`kfree_skb` 覆盖不到所有消费路径，对象地址也不能把克隆、分段、聚合之后的全部关系自动串起来。

这个实验只证明一件事：端口关闭这个特定条件下，UDP 接收侧出现了一个 `NO_SOCKET` 事件。源码对照 [udp.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/udp.c) 和[事件定义](https://github.com/torvalds/linux/blob/v7.0/include/trace/events/skb.h)。

## 四、对象数与帧数为什么不一致

| 机制 | 改变什么 | 观察上的后果 |
| --- | --- | --- |
| GSO | 大对象延后分段 | 发送侧能看见超过单帧大小的对象 |
| GRO | 接收侧先合并再上交 | 一个上层对象对应多个接收帧 |
| 校验和卸载 | 校验推给后续阶段或设备 | 本地抓包可能看到还没填完的校验和 |
| 克隆与重传 | 多个描述或多次发送指向同一份字节 | 对象数、发送次数与业务字节数不一一对应 |

这些机制不改变协议语义，改变的是内核处理和工具观察的单位。要问"到底发了几个包"，得先说清问的是应用写了几次、几个 skb、几条抓包记录，还是几个设备帧。

## 五、命令与接口速查

| 接口 / 路径 | 用途 |
| --- | --- |
| `skb_reserve/push/pull/put` | 预留空间、前后调整边界 |
| `skb_clone()` | 造一个共享数据的描述对象 |
| `events/skb/kfree_skb` | 观察 skb 释放或丢弃事件 |
| `instances/NAME` | 把跟踪配置放进专属实例，不动全局设置 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| headroom / tailroom | 线性有效数据前后的可用余量 |
| 非线性数据 | 不在线性区域里连续存放的那部分 |
| page_pool | 面向接收等场景的页复用设施 |
| 线性化 | 把需要的数据变成连续可访问区域的操作 |
| `users` | skb 描述对象自身的引用计数，与数据存储的引用是两回事 |
