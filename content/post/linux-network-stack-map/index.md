---
title: "Linux 网络栈（一）：一个包的完整路径地图"
slug: linux-network-stack-map
date: "2026-11-22T16:01:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "跟着一次五字节发送，把 DMA、二层分发、tc、五个 netfilter hook 和 TCP 收包放回同一条路径，并给出实测的函数级时间线。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

一次 `write()` 返回 5，只说明内核收下了这 5 个字节。它们变成什么样、经过谁的队列、在哪个进程里被处理、最后怎样出现在另一端的 `read()` 里，是一条有十四次状态转移的路。

本篇文章介绍这条路的骨架。主线是一次真实的五字节 TCP 发送，实测环境是 Ubuntu ARM64、内核 7.0.0-44；发送侧记作 S1–S7，接收侧记作 R1–R7，全文只用这一套编号。路由、qdisc、netfilter、驱动这些子系统各自长什么样，留给后面九篇展开，这里只回答一件事：**在哪个时刻，数据在谁手里，下一步谁接手**。

系列安排：**一：路径地图** · 二：socket 与多路复用 · 三：sk_buff · 四：驱动与 NAPI · 五：路由与邻居 · 六：qdisc 与流控 · 七：Netlink · 八：网络命名空间 · 九：veth 与 bridge · 十：Netfilter 与连接跟踪。

## 一、先把问题钉死

### 1.1 一个可以核对的问题

在 A 上写 5 个字节，B 上读出这 5 个字节。中间发生了什么，读者可以用三条命令自己核对：`tcpdump` 看到的帧长、`strace` 看到的系统调用、`ss -ti` 看到的连接状态。本文要解释的是，为什么抓包显示 71 字节，而 `write()` 的参数是 5。

### 1.2 实验环境

实测环境是 VMware Fusion 里的 Ubuntu ARM64，内核 `7.0.0-44-generic`，2 vCPU，3892 MB 内存；源码参照上游 Linux v7.0。宿主机两块网卡都是 `e1000e`，单收发队列，RX/TX 环形缓冲各 256，排队规则 `fq_codel`。

路径实验跑在两个 network namespace 里，用一对 veth 相连，不碰物理网卡：

```bash
gcc -O0 -g -Wall -Wextra netdemo.c -o netdemo
sudo bash lab.sh direct
```

两个文件也在同目录：[netdemo.c](netdemo.c) 是收发程序，[lab.sh](lab.sh) 负责建网和清理（`lab.sh clean`）。

A 是 `10.77.1.1/24`，B 是 `10.77.1.2/24`，中间没有网关。[环境记录](environment.txt)、[抓包原文](wire-71bytes.txt)、[函数时间线](trace-5byte.txt)、[网卡侧计数](nic-napi.txt)、[hook 对照](netfilter-hooks.txt) 都在同目录，也可以直接下[实验记录包](experiment-records.zip)。

### 1.3 为什么用两套实验

veth 能把 S1 到 R6 完整走通，且耗时在微秒级，适合看顺序；但它没有 DMA、没有描述符环、没有中断，S7 和 R1 在这条路上根本不存在。所以网卡侧的 DMA、NAPI、GRO 用另一组实验单独测：从那台机器真实网卡上收 20 MB。两组数据不能拼成一条时间线，后面会分开标注。

![从 write 到 read 的十四个交接点：发送 S1–S7，接收 R1–R7，左侧是执行上下文，右侧是 tc 与 netfilter 的位置](path.svg)

## 二、线路上到底有几个字节

### 2.1 抓包看到的 71 字节

在 A 的 `a0` 口抓一次，五字节数据那一帧是这样的：

```text
08:56:40.337619 ... length 71: 10.77.1.1.57658 > 10.77.1.2.18083: Flags [P.], seq 1:6, length 5
	0x0000:  4500 0039 577c 4000 4006 cca6 0a4d 0101  E..9W|@.@....M..
	0x0010:  0a4d 0102 e13a 46a3 82c4 f119 1082 14d1  .M...:F.........
	0x0020:  8018 007e 16c8 0000 0101 080a 184c 5601  ...~.........LV.
	0x0030:  b195 aad7 4141 4141 41                   ....AAAAA
```

`4141 4141 41` 就是应用写出的五个 `A`。这一帧在链路上 71 字节，按层次拆开是：

| 层次 | 长度 | 依据 |
| --- | --- | --- |
| 应用数据 | 5 字节 | `write(fd, "AAAAA", 5)` |
| TCP 段 | 37 字节 | 20 字节固定头 + 12 字节选项（`nop,nop,TS`）+ 5 字节数据 |
| IP 包 | 57 字节 | `4500 0039` 里 `0x0039 = 57`，即 20 字节 IP 头 + 37 字节负载 |
| 以太网帧 | 71 字节 | 14 字节帧头 + 57 字节 IP 包，与 tcpdump 的 `length 71` 一致 |

### 2.2 三个长度别混

同一个包有三个长度，各有各的用处：

- 应用关心 5，这是它写进去的字节数。
- `ip_rcv_core` 关心 57，这是 IP 头里声明的总长，决定了要交给上层多少字节。
- 驱动和 `tc` 关心 71，这是链路上实际搬运的字节数，MTU 比对、qdisc 计数都用它。

还有一个容易漏的：以太网帧在线路上还要加 8 字节前导码和 12 字节帧间隙，但这两段不进 `skb`，所以 `ip -s link` 的字节数和抓包长度都对不上物理层速率。

### 2.3 一个容易猜错的点

抓包里的 `length 71` 不是"数据 71 字节"。打开 `-X` 的十六进制就会看到，从 `0x0000` 开始的 57 个字节都是头部和数据，最后 5 个字节才是应用写的。头部开销 66 字节，是负载的 13 倍——小包场景下，协议开销远大于数据本身。

## 三、发送侧：从 write 到描述符环

### 3.1 S1：五个字节进入 socket

`write()` 落到 `sock_write_iter`，按 fd 找到 `struct file`，再找到 `socket`，然后经 `__sock_sendmsg` 走到 `sk->sk_prot->sendmsg`。TCP 的 `sk_prot` 是 `tcp_prot`，所以这一步就进了 `tcp_sendmsg`（[net/socket.c](https://github.com/torvalds/linux/blob/v7.0/net/socket.c)、[net/ipv4/tcp.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/tcp.c)）。

实测里 `__sock_sendmsg` 到 `tcp_sendmsg` 相隔 2458 ns。这段距离花在系统调用入口、fd 到 `struct file` 的查找、`socket` 到 `sock` 的转换上，还没碰任何网络协议。

数据的归属在这里发生变化：在此之前它属于用户缓冲区，在此之后它被拷进 `skb`，挂到 `sk_write_queue`，归 socket 所有。

### 3.2 S2：能不能发、发多少，由 tcp_write_xmit 决定

`tcp_sendmsg` 只负责把字节放进队列。真正决定"现在发多少"的是 `tcp_write_xmit`：拥塞窗口 `snd_cwnd`、对端通告窗口、Nagle 判断、TSQ（发送队列长度限制）任何一个不满足，数据就继续排队。

所以 `write()` 返回 5 不代表 5 个字节已经在线路上，只代表它们已经进了内核队列。这个区别在后面的排障里很关键：`write` 成功而抓不到包，问题就在 S2 到 S6 之间。

实测从 `tcp_sendmsg` 到 `tcp_write_xmit` 相隔 5500 ns，是整条链上第二个大间隔——拷贝 5 个字节本身很快，主要是建 skb、更新队列、算校验和的准备开销。

### 3.3 S3：加 IP 头，中间夹着两次 hook

这一段在 [net/ipv4/ip_output.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/ip_output.c)。调用顺序是：

```text
tcp_write_xmit → (组 TCP 段) → ip_queue_xmit → __ip_local_out
              → NF_INET_LOCAL_OUT → dst_output → ip_output
              → NF_INET_POST_ROUTING → ip_finish_output
```

两个细节值得记住：

- **路由在 `ip_queue_xmit` 里就查完了**。`__ip_queue_xmit` 调 `ip_route_output_ports`，把出口设备和源地址定下来，写进 `skb_dst`。所以 `LOCAL_OUT` 之后的代码拿到的 skb，路由已经确定。
- **`LOCAL_OUT` 和 `POST_ROUTING` 之间夹着 `ip_output`**。`ip_output` 做的事是填 IP 头、算校验和、处理 `IPSKB_REROUTED`。因此 hook 3 看到的 skb 已经有完整 IP 头，hook 4 看到的是即将交给链路层的版本。

实测三个点的间隔：`__ip_local_out` → hook 3 是 333 ns，hook 3 → `ip_output` 是 500 ns，`ip_output` → hook 4 是 250 ns。这点差值已经接近探针本身的开销，只能说明顺序，不能说明各步的真实耗时。

### 3.4 S4：超过 MTU 才分片

`ip_finish_output` 只做一件事：判断这个 skb 能不能整块交给下一跳。

- 长度不超过 MTU，直接进 `ip_finish_output2`。
- 超过 MTU 且允许分片，进 `ip_fragment`。
- 超过 MTU 且设置了 DF 位，回一个 ICMP `fragmentation needed` 并丢弃。

本例 71 字节，MTU 1500，直接放过。这里有个容易踩的坑：开了 GSO 的 skb 长度可能远超 MTU，但它是"允许分段"的，`ip_finish_output` 会走 GSO 分支交给网卡分段，而不是在协议栈里分片。

### 3.5 S5：下一跳的 IP 换成 MAC

`ip_finish_output2` 拿下一跳地址去邻居表里查以太网地址，查到就填好帧头交给设备队列，查不到就发 ARP 并把 skb 挂在 `neigh->arp_queue` 上等。

实测 `ip_finish_output2` 到 `__dev_queue_xmit` 只隔 417 ns，说明 ARP 早在 `connect()` 时就解析完了，这次是命中缓存。**邻居解析不是每个包都做**，只在缓存过期或首次通信时做；这也是为什么第一个包的延迟总比后续包高。

### 3.6 S6：tc 在这里，qdisc 也在这里

`__dev_queue_xmit` 是设备发送的总入口（[net/core/dev.c](https://github.com/torvalds/linux/blob/v7.0/net/core/dev.c)），顺序是：

```text
__dev_queue_xmit → (选 txq) → tc egress → qdisc 入队 → __qdisc_run → dev_hard_start_xmit → ndo_start_xmit
```

`tc` 的 egress 挂点就在 qdisc 之前，函数是 `sch_handle_egress`，它内部调 `tcf_classify` 逐条匹配过滤器。这个函数在 7.0 里是 `net/core/dev.c` 中的 `static __always_inline`，没有独立符号，所以探针只能看到它内部的 `tcf_classify`。实测把 `tcf_classify` 夹在 `__dev_queue_xmit` 和 `dev_hard_start_xmit` 之间，间隔 333 ns。

qdisc 差别很大：

- veth 的 `a0` 是 `noqueue`，没有排队，`__dev_xmit_skb` 直接调 `dev_hard_start_xmit`。
- 真实网卡是 `fq_codel`，有入队、有调度、有丢弃统计。

所以"在 veth 上测不出队列延迟"不是测量方法的问题，是这条路径上根本没有队列。

### 3.7 S7：描述符、DMA 和门铃

`dev_hard_start_xmit` 调驱动的 `ndo_start_xmit`，e1000e 里是 `e1000_xmit_frame`：从 `tx_ring` 取一个空闲描述符，把 skb 的 DMA 地址和长度写进去，更新 `next_to_use`，最后敲门铃寄存器通知网卡。

网卡拿到通知后自己发 DMA 读请求，把内存里的数据搬到片上、组帧、发到线路上。**CPU 到这一步就不再参与搬运**，这也是为什么大包吞吐主要由 DMA 和总线决定，而不是 CPU 频率。

这一步的参数是实测的：RX/TX 环形缓冲各 256 个描述符（`ethtool -g`），队列只有一组（`/sys/class/net/enp2s0/queues/` 下只有 `rx-0` 和 `tx-0`），MSI-X 中断三条。

veth 没有这一站。`dev_hard_start_xmit` 会直接调 `veth_xmit`，把 skb 交给对端 netns 的接收路径——这就是下一节开头那个反直觉现象的来源。

## 四、接收侧：从 DMA 到 read

### 4.1 R1：网卡写内存，然后发中断

网卡收到帧后，用 DMA 把数据写进 rx 环里某个描述符指向的缓冲区，回写描述符状态，然后触发 MSI-X 中断。驱动 `e1000_intr_msix_rx` 进来后**先把该队列的中断关掉**，再 `napi_schedule` 把 poll 排进软中断。

关中断这个动作是 NAPI 的核心：中断只负责"叫醒"，搬运交给轮询。

实测 20 MB 传输里 14575 个报文只触发了 99 次硬中断，比例约 147:1。

### 4.2 R2：一次 poll 取走一批

软中断 `net_rx_action` 遍历挂起的 napi 实例，按预算（`NAPI_POLL_WEIGHT`，默认 64）调 `napi_poll`，e1000e 里是 `e1000_clean_rx_irq`。它从环上按顺序取描述符，为每个描述符建 `skb`（`napi_alloc_skb`），再用 `eth_type_trans` 从帧头取出 ethertype 写进 `skb->protocol`。

实测 14575 个报文触发了 319 次 poll，平均一次取 46 个；`eth_type_trans` 被调用 14573 次，基本等于一个包一次。

### 4.3 R3：GRO 把同一条流的小段焊起来

`napi_gro_receive` 在交给协议栈之前做一次合并：同一条流、同方向、序号连续的段，能合就合进前一个 skb 的 frags 里。合并后的 skb 长度可以远超 MTU，因为它已经不是"链路上的帧"，而是"给协议栈看的段"。

实测最能说明问题：

| 计数点 | 次数 | 含义 |
| --- | --- | --- |
| `eth_type_trans` | 14573 | 建了多少个 skb，约等于报文数 |
| `ip_list_rcv` | 540 | 多少次批量交付 |
| `ip_rcv_core` | 600 | 多少次真正进入 IP 层 |
| `tcp_v4_rcv` | 600 | 多少次交给 TCP |

14573 个 skb 只换来 600 次 IP 层处理，合并比约 24:1。**GRO 省的不是中断，是上层遍历 skb 的次数。**

### 4.4 R4：二层分发，tc 在 ptype 之前

`netif_receive_skb` → `__netif_receive_skb_core` 是二层分发的总入口，里面做两件事，顺序很关键：

1. 先过 tc ingress（`sch_handle_ingress` → `tcf_classify`）。
2. 再按 `skb->protocol` 在 `ptype_all` / `ptype_base` 里找处理函数。

实测 `tcf_classify` 出现在 `__netif_receive_skb` 之后、`ip_rcv` 之前，位置和源码一致。`sch_handle_ingress` 开头会判断 `dev->tcx_ingress` 是否为空，为空就直接返回，所以不挂 tc 过滤器时 `tcf_classify` 一次都不会被调用——这与实测的 0 次一致。

这里有个本文实测出来的坑：**IPv4 在 `ptype_base` 里注册了两个入口**，`func = ip_rcv` 对应单个 skb，`list_func = ip_list_rcv` 对应批量交付。单个走 `ip_rcv`；批量走 `ip_list_rcv` → `ip_sublist_rcv` → `ip_rcv_core`。网卡接收因为 GRO 会攒成 list，实测 5 个 ping 包里 `ip_list_rcv=5`、`ip_rcv=0`。凡是只画 `ip_rcv` 的路径图，在网卡这条路上都对不上。

### 4.5 R5：路由决定本地交付还是转发

`ip_rcv_core` 之后立刻进 `PRE_ROUTING`，然后 `ip_rcv_finish` 调 `ip_rcv_finish_core`，用 `ip_route_input_noref` 查路由并把结果写进 `skb_dst`。这一步的结论只有两种：

- 目的地址是本机 → `ip_local_deliver`，中间过 `LOCAL_IN`。
- 目的地址不是本机且允许转发 → `ip_forward`，中间过 `FORWARD`。

实测的 5 字节包走本地交付，只出现 hook 0 和 hook 1，hook 2 一次没有——因为没有转发包。

### 4.6 R6：四元组查连接，数据入队

`ip_local_deliver_finish` → `ip_protocol_deliver_rcu` 按 `skb->protocol` 找到 TCP 处理函数 `tcp_v4_rcv`（[net/ipv4/tcp_ipv4.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/tcp_ipv4.c)）。

`tcp_v4_rcv` 用源地址、目的地址、源端口、目的端口做哈希，在 `ehash`（已建立连接）和 `lhash`（监听）里找到 socket，然后 `tcp_v4_do_rcv` → `tcp_rcv_established`，把数据挂到 `sk_receive_queue`；如果序号不连续，先进 `out_of_order_queue` 等重传。

实测 `ip_protocol_deliver_rcu` → `tcp_v4_rcv` 292 ns，→ `tcp_v4_do_rcv` 541 ns。到这一步，数据已经躺在接收 socket 的队列里，但接收进程还没醒。

### 4.7 R7：唤醒，然后拷贝

`tcp_rcv_established` 在把数据入队后会调 `sk_data_ready`（默认实现 `sock_def_readable`），唤醒 `sk_sleep` 上等待的进程。被唤醒的 `read()` 进入 `tcp_recvmsg`，把 `sk_receive_queue` 里的 skb 逐段拷到用户缓冲区。

如果接收进程用了 epoll，`sock_def_readable` 还会走 `ep_poll_callback` 把就绪事件挂到 epoll 的就绪链表——这条链正是第二篇要展开的"谁被唤醒"。

## 五、把一次五字节发送的真实时间线读一遍

### 5.1 二十七个点

下面是 S1 到 R6 的完整记录，原始文件在 [trace-5byte.txt](trace-5byte.txt)。`Δ` 是与上一个点的时间差，`累计` 从 `__sock_sendmsg` 起算。

| # | 函数 | Δ ns | 累计 µs |
| --- | --- | --- | --- |
| 1 | `__sock_sendmsg` | — | 0.00 |
| 2 | `tcp_sendmsg` | 2458 | 2.46 |
| 3 | `tcp_write_xmit` | 5500 | 7.96 |
| 4 | `ip_queue_xmit` | 1708 | 9.67 |
| 5 | `__ip_local_out` | 375 | 10.04 |
| 6 | `nf_hook_slow`，hook 3 | 333 | 10.37 |
| 7 | `ip_output` | 500 | 10.87 |
| 8 | `nf_hook_slow`，hook 4 | 250 | 11.12 |
| 9 | `ip_finish_output` | 375 | 11.50 |
| 10 | `ip_finish_output2` | 375 | 11.87 |
| 11 | `__dev_queue_xmit` | 417 | 12.29 |
| 12 | `tcf_classify` | 333 | 12.62 |
| 13 | `dev_hard_start_xmit` | 708 | 13.33 |
| 14 | `eth_type_trans` | 334 | 13.67 |
| 15 | `enqueue_to_backlog` | 333 | 14.00 |
| 16 | `net_rx_action` | 500 | 14.50 |
| 17 | `__netif_receive_skb` | 375 | 14.87 |
| 18 | `ip_rcv` | 458 | 15.33 |
| 19 | `ip_rcv_core` | 250 | 15.58 |
| 20 | `nf_hook_slow`，hook 0 | 334 | 15.92 |
| 21 | `ip_rcv_finish` | 499 | 16.42 |
| 22 | `ip_local_deliver` | 542 | 16.96 |
| 23 | `nf_hook_slow`，hook 1 | 208 | 17.16 |
| 24 | `ip_local_deliver_finish` | 334 | 17.50 |
| 25 | `ip_protocol_deliver_rcu` | 333 | 17.83 |
| 26 | `tcp_v4_rcv` | 292 | 18.12 |
| 27 | `tcp_v4_do_rcv` | 541 | 18.67 |

### 5.2 三个从这份记录里得出的结论

**第一，接收半程没有换线程。** 27 个点的 `tid` 全是 4443，`cpu` 全是 0。也就是 B 的接收处理，是在 A 的 `write()` 系统调用里跑完的。

原因在 veth 的实现：`veth_xmit` → `dev_forward_skb` → `netif_rx` 把 skb 挂到**本 CPU** 的 `process_queue`，等这一层 `local_bh_enable` 时就地触发 `net_rx_action`，于是 `enqueue_to_backlog` 和 `net_rx_action` 在时间线上挨着出现，中间只隔 500 ns。

**第二，同样的代码可以跑在完全不同的上下文里。** 另一份记录里，同一个包从定时器软中断发出，时间线是这样的：

```text
1894023326984 kprobe:ip_queue_xmit   pid=0      cpu=1 comm=swapper/1
1894023427490 kprobe:net_rx_action   pid=24     cpu=1 comm=ksoftirqd/1
```

同样是 `ip_queue_xmit` 到 `net_rx_action`，这次隔了 100 µs，执行者从"发送进程"变成"ksoftirqd"。所以"接收路径属于接收进程"这个说法是错的：**它属于当时正在执行软中断的那个任务**。

**第三，绝对耗时不能读。** 同一段路径在不同探针数量下差异很大：15 个探针那一轮 13.9 µs，17 个探针那一轮 22.6 µs，26 个探针这一轮 18.7 µs。没有稳定的线性关系，说明探针开销和运行时噪声同量级。

所以这份记录的用途是**顺序**和**量级**：整趟收发在几十微秒内完成，`tcp_sendmsg` 和 `tcp_write_xmit` 是最大的两个间隔，其余每一站都是几百纳秒。要测绝对耗时，得减少探针，换 `fentry` 或直接用 `perf`。

### 5.3 这份测量的边界

三件事这份记录证明不了：

- veth 没有 DMA、描述符环和中断，S7 和 R1 在这条路上是空的。第 3.7 节和 4.1 节的数字来自网卡实验，不是这条时间线。
- 探针只覆盖了选定的函数，函数内部的耗时看不到；`tcp_transmit_skb` 在本机内核上没有独立符号，所以时间线从 `tcp_write_xmit` 直接跳到 `ip_queue_xmit`。
- 单包测量不带并发，拥塞窗口、qdisc 排队、GRO 合并这些行为在批量场景下才显现。

## 六、hook、tc 和 eBPF 到底在哪一步

### 6.1 五个 hook 的坐标

netfilter 的 IPv4 hook 有五个，编号和出现位置固定：

| 编号 | 常量 | 出现在 | 无规则时 | 空基链 | 加 tc 过滤 |
| --- | --- | --- | --- | --- | --- |
| 0 | `NF_INET_PRE_ROUTING` | `ip_rcv_core` 之后、`ip_rcv_finish` 之前 | 0 | 8 | 8 |
| 1 | `NF_INET_LOCAL_IN` | `ip_local_deliver` 内 | 0 | 8 | 8 |
| 2 | `NF_INET_FORWARD` | `ip_forward` 内 | 0 | 0 | 0 |
| 3 | `NF_INET_LOCAL_OUT` | `__ip_local_out` 内 | 8 | 8 | 8 |
| 4 | `NF_INET_POST_ROUTING` | `ip_output` 之后、`ip_finish_output` 之前 | 8 | 8 | 8 |
| — | `tcf_classify` | egress 与 ingress 各一处 | 0 | 0 | 8 |

表里的数字是同一次完整连接的计数：8 表示这次连接里经过该 hook 的 8 个包全都在 `nf_hook_slow` 里停了一下，0 表示一个都没有。原始记录见 [netfilter-hooks.txt](netfilter-hooks.txt)。

三列对比能读出两件事：

- **装了空的 filter 基链，入方向的两个 hook 才出现**。规则为空也照样进 `nf_hook_slow`，只是立即返回；没装基链时，`net->nf.hooks_ipv4[hook]` 是空的，`nf_hook()` 直接跳过。
- **出方向的两个 hook 即使一条规则都没有也在调**。这不是 nft 的行为。

### 6.2 出方向那两个 hook 是谁占的

在一个全新的、`nft list ruleset` 为空的 netns 里 ping 三次回环，`nf_hook_slow` 仍然在 hook 3 和 hook 4 各被调用 6 次。把表项里的函数地址打出来：

```text
hook=3 n=1 fn=apparmor_ip_localout  pf=2
hook=4 n=1 fn=apparmor_ip_postroute  pf=2
```

是 AppArmor。Ubuntu 默认启用 AppArmor，它在出方向的两个 hook 上常驻一条表项，和 nft 规则无关。

结论：**判断某个 hook 是否参与，要看 `nf_hook_slow` 有没有被调用，而不是看 `nft list ruleset` 有没有内容。** 这也是为什么"我没配防火墙，所以包没经过 netfilter"这句话经常是错的。

### 6.3 tc 两个点，XDP 一个点

- **tc egress**：在 `__dev_queue_xmit` 里、qdisc 入队之前。作用对象是已经走完 IP 层、即将下发的 skb。
- **tc ingress**：在 `__netif_receive_skb_core` 里、ptype 分发之前。作用对象是刚建好、协议字段已填但还没进 IP 层的 skb。
- **XDP**：在驱动里、`skb` 还没分配之前。它拿到的是 `xdp_buff`，只能看到线性区的原始帧，所以能改的东西比 tc 少，但代价也最低。

这三者的顺序决定了它们能做什么：XDP 最早，看不到 `skb` 的元数据；tc ingress 能看到 `skb`，但还没有路由结果；tc egress 已经知道出口设备。eBPF 的挂载点和这三个位置重合，具体写法留给第九篇之后的实验。

## 七、这张地图怎么用

### 7.1 现象对应到站

| 现象 | 先看哪一站 | 命令 |
| --- | --- | --- |
| `write` 返回 5，但抓包没有 | S2–S6 | `ss -ti`、`tc -s qdisc show` |
| 本机抓到了，对端抓不到 | S6–S7 | `ip -s link`、`ethtool -S` |
| 对端抓到了，应用读不到 | R4–R7 | `ss -mni`、`cat /proc/net/snmp` |
| 收包丢在环上 | R1–R2 | `ethtool -S dev \| grep -i drop` |
| 收包丢在协议栈 | R4–R5 | `nstat -az`、`tc -s filter show` |
| 延迟忽高忽低 | R1–R2、S6 | `cat /proc/interrupts`、`tc -s qdisc` |

### 7.2 每一站交给后面哪一篇

| 站 | 交给 |
| --- | --- |
| S1、S2、R6、R7 | 第二篇 socket 与多路复用 |
| S3 的 skb 结构、S4 的分片 | 第三篇 sk_buff |
| R1、R2、R3 | 第四篇 驱动与 NAPI |
| S5 | 第五篇 路由与邻居 |
| S6 | 第六篇 qdisc 与流控 |
| 配置类命令怎么下发 | 第七篇 Netlink |
| 这套拓扑怎么造出来 | 第八篇 网络命名空间 |
| veth 与 bridge 的内部 | 第九篇 |
| hook 与 conntrack | 第十篇 |

## 八、命令速查

| 命令 | 用途 | 关键输出 |
| --- | --- | --- |
| `write(fd, buf, 5)` | 发送 5 字节 | 返回 5 只表示进了内核队列 |
| `strace -e trace=write ./netdemo tcp-client IP PORT 5` | 确认系统调用 | `write(3, "AAAAA", 5) = 5` |
| `tcpdump -i a0 -nn -e -X -c 8` | 看链路层帧 | `length 71`，末尾 5 字节是负载 |
| `sudo bpftrace -e 'kprobe:tcp_sendmsg { @=count(); }'` | 统计某函数调用次数 | `@: N` |
| `sudo bpftrace -e 'kprobe:nf_hook_slow { @[((struct nf_hook_state *)arg1)->hook]=count(); }'` | 看 hook 是否参与 | 按 hook 号分组计数 |
| `sudo bpftrace -e 'kprobe:tcf_classify { @=count(); }'` | 看 tc 是否参与 | 没装 filter 时为 0 |
| `tc -s qdisc show dev DEV` | 看排队与丢弃 | `sent/backlog/drops` |
| `tc filter show dev DEV egress` | 看过滤器 | `matchall`、`action` |
| `nft list ruleset` | 看 nft 规则 | 空不等于 hook 没参与 |
| `ethtool -g DEV` | 看环形缓冲 | RX/TX 256 |
| `ethtool -S DEV \| grep -i drop` | 看网卡侧丢弃 | `rx_missed_errors` 等 |
| `cat /proc/interrupts` | 看中断分布 | 每队列一条 MSI-X |
| `cat /proc/softirqs` | 看软中断分布 | `NET_RX` 两列对应 CPU |
| `ss -ti` | 看 TCP 内部状态 | `cwnd`、`rtt`、`retrans` |
| `ss -mni` | 看 socket 内存与队列 | `skmem`、`recv-q` |
| `ip -s link show DEV` | 看设备计数 | `RX/TX packets/errors/dropped` |
| `nstat -az` | 看协议栈统计 | `IpInReceives`、`TcpInErrs` |

## 九、名词解释

| 术语 | 一句话含义 |
| --- | --- |
| skb（`struct sk_buff`） | 内核里描述一个报文的对象，数据本身在它指向的缓冲区里 |
| veth | 成对出现的虚拟以太网设备，一端发送直接进另一端接收 |
| DMA | 设备直接读写内存，不经过 CPU 搬运 |
| 描述符环 | 驱动与网卡共享的环形数组，每项描述一块缓冲区的地址和状态 |
| MSI-X | 一种 PCI 中断方式，可以为不同队列分配不同中断向量 |
| NAPI | 中断只负责唤醒、由软中断轮询批量取包的机制 |
| 软中断（softirq） | 中断后半段执行的机制，`NET_RX` 是网络收包用的那一个 |
| GRO | 收包时把同一条流的连续小段合并成一个大段，减少上层遍历次数 |
| qdisc | 设备上的排队规则，决定包按什么顺序、什么时候发出 |
| tc | 流量控制工具，egress 在 qdisc 之前、ingress 在协议分发之前 |
| netfilter hook | 协议栈里五个固定位置的回调点，防火墙规则挂在这里 |
| XDP | 驱动层的最早挂点，在 `skb` 分配之前处理原始帧 |
| backlog | 每 CPU 的待处理包队列，网卡中断和 veth 都往这里放 |
| `sk_receive_queue` | 接收 socket 上等待应用读取的数据队列 |
| MTU | 一次能在链路上传输的最大负载长度，以太网默认 1500 |
| GSO | 发送侧延迟分段，让网卡或后续路径去切分大 skb |

<!--
进度：第一篇已按 S1–S7 / R1–R7 重写，实测数据来自 7.0.0-44 虚拟机，待作者校验。
已清理旧结构的五张 SVG（topology、three-paths、capture-points、rx-data-notification、socket-wait-stages），只保留 path.svg。
下一步：校验通过后改写第二篇（socket 与多路复用、LT/ET、四组 demo），其余八篇随后。
-->
