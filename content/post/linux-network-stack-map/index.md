---
title: "Linux 网络栈（一）：一个包的完整路径地图"
slug: linux-network-stack-map
date: "2026-11-22T16:01:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "跟着一次五字节发送走完 S1–S7、R1–R7 十四个交接点：每一站谁拿着数据、做了什么、交给谁，细节留给后续九篇。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

一次 `write()` 返回 5，说明内核收下了这 5 个字节，仅此而已。它们随后被拷进 skb、排队、加头、交给驱动，再从对端网卡的中断里出来，穿过协议栈，最后落进 `read()` 的缓冲区。中间换过几次上下文、经过谁的队列，就是本文要走的路线。

主线是一次五字节 TCP 发送。文中的报文数、中断数和耗时都来自 1.2 节那台机器：Ubuntu ARM64、内核 7.0.0-44。这一篇是地图，不是手册——发送侧记作 S1–S7，接收侧记作 R1–R7，全文只用这一套编号，每一站只回答一个问题：这一刻数据在谁手里，下一步谁接手。函数和分支细节留给对应篇目，7.2 那张表是索引。

系列安排：一、路径地图 · 二、socket 与多路复用 · 三、sk_buff · 四、驱动与 NAPI · 五、路由与邻居 · 六、qdisc 与流控 · 七、Netlink · 八、网络命名空间 · 九、veth 与 bridge · 十、Netfilter 与连接跟踪。

## 一、问题与实验环境

### 1.1 一个可以核对的问题

在 A 上写 5 个字节，B 上读出这 5 个字节。同一次收发，三个工具给出三个不相等的数字：`tcpdump` 说链路上是 71 字节的一帧，`strace` 说 `write` 的第三个参数是 5，`ss -ti` 里则是一串拥塞窗口和队列长度。这三个数字分别属于链路、应用和内核队列，把它们的对应关系讲清楚，大半条路径也就出来了。

### 1.2 实验环境

环境是 VMware Fusion 里的 Ubuntu 26.04.1 LTS（ARM64），内核 `7.0.0-44-generic`，2 vCPU，3892 MB 内存；源码参照上游 Linux v7.0。

虚拟机接了两块 `e1000e`，分工是固定的：`enp2s0` 走 NAT，`192.168.126.135/24`，负责出网和 DNS；`enp10s0` 走仅主机网络，静态 `192.168.190.10/24`，Mac 从这里 SSH 进来，也就是实验流量走的那块。两块网卡参数相同：单收发队列、RX/TX 环形缓冲各 256、排队规则 `fq_codel`。这两块网卡怎么分的网段、静态地址怎么填，见[《Ubuntu Server 虚拟机安装及配置》](/p/ubuntu-server-vm-install/)（那篇装的是 24.04，双网卡布局与此相同）。

路径实验跑在两个 network namespace 里，用一对 veth 相连，不碰物理网卡。用到的文件有五个：[netdemo.c](netdemo.c)、[lab.sh](lab.sh)，以及 5.1、4.3、6.2 三节各用一个的 [tl3.bt](tl3.bt)、[gro3.bt](gro3.bt)、[hooks.bt](hooks.bt)。一并放到同一个工作目录，后面每条命令都在这里执行：

```bash
mkdir -p ~/nslab && cd ~/nslab
wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/netdemo.c
wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/lab.sh
wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/tl3.bt
wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/gro3.bt
wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/hooks.bt
# 新装的 Ubuntu Server 上 bpftrace、tcpdump、nft、nc、gcc 不一定齐
sudo apt install -y bpftrace tcpdump nftables netcat-openbsd ethtool strace gcc
gcc -O0 -g -Wall -Wextra netdemo.c -o netdemo
sudo bash lab.sh direct
```

建完网，一次完整的收发按这个顺序来：**先抓包，再起服务端，最后发客户端**；重复实验前先 `sudo bash lab.sh clean`，否则 namespace 已存在会直接退出。三个终端各跑一条：

```bash
# 终端一：先把 tcpdump 起起来，-c 8 抓够自己退
sudo ip netns exec nslab-a tcpdump -i a0 -nn -e -X -c 8 'tcp port 18082'

# 终端二：B 侧监听，前台跑着，接上之后先等 1500 ms 再 read
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.1.2 18082 1500

# 终端三：A 侧连过去，写 5 个字节
sudo ip netns exec nslab-a ./netdemo tcp-client 10.77.1.2 18082 5
```

`tcpdump` 必须早于客户端，晚一步就漏掉 SYN 和握手三帧；放在第一个终端最省事。

`'tcp port 18082'` 这个过滤条件不是可选项。新建的 netns 里接口刚起来那两三秒，IPv6 会发 DAD 和 MLD 组播，不加过滤时 `-c 8` 会被这些帧吃光：抓到的 8 帧全是 `ethertype IPv6`，握手一帧都进不来。

服务端第 4 个参数是 accept 之后等多少毫秒再读，客户端的是写多少字节（不给就是 5）。终端二先打 `ready TCP 10.77.1.2:18082`，客户端退出后才补一行 `received=5 bytes`——中间这段空白就是那 1500 ms。

A 是 `10.77.1.1/24`，B 是 `10.77.1.2/24`，中间没有网关。[环境记录](environment.txt)、[抓包原文](wire-71bytes.txt)、[函数时间线](trace-5byte.txt)、[网卡侧计数](nic-napi.txt)、[hook 对照](netfilter-hooks.txt) 都在同目录，也可以直接下[实验记录包](experiment-records.zip)。

### 1.3 两套实验各自覆盖的站点

veth 能把 S1 到 R6 完整走通，且耗时在微秒级，适合看顺序；但它没有 DMA、没有描述符环、没有中断，S7 和 R1 在这条路上根本不存在。DMA、NAPI、GRO 只能在同一台机器的真实网卡上看，做法是从对端灌 20 MB，数接收侧的计数。两组数据不能拼成一条时间线，后面分开标注。

![从 write 到 read 的十四个交接点：发送 S1–S7，接收 R1–R7，左侧是执行上下文，右侧是 tc 与 netfilter 的位置](path.svg)

## 二、线路上 71 字节帧的构成

### 2.1 数据帧的抓包记录

命令就是 1.2 节终端一那条，`-e` 让它打出链路层头，`-X` 把从 IP 头开始的内容按十六进制排开。所以下面这段输出里既有两端的 MAC 和 ethertype，也有末尾那五个 `41`：

```text
12:34:18.140047 02:00:00:77:01:01 > 02:00:00:77:01:02, ethertype IPv4 (0x0800), length 71: 10.77.1.1.44862 > 10.77.1.2.18082: Flags [P.], seq 1:6, ack 1, win 126, options [nop,nop,TS val 2833223901 ecr 702975311], length 5
	0x0000:  4500 0039 9831 4000 4006 8bf1 0a4d 0101  E..9.1@.@....M..
	0x0010:  0a4d 0102 af3e 46a2 0f82 a941 4c34 7e60  .M...>F....AL4~`
	0x0020:  8018 007e 16c8 0000 0101 080a a8df 90dd  ...~............
	0x0030:  29e6 8d4f 4141 4141 41                   )..OAAAAA
```

`4141 4141 41` 就是应用写出的五个 `A`。这一帧在链路上 71 字节，按层次拆开是：

| 层次 | 长度 | 依据 |
| --- | --- | --- |
| 应用数据 | 5 字节 | `write(fd, "AAAAA", 5)` |
| TCP 段 | 37 字节 | 20 字节固定头 + 12 字节选项（`nop,nop,TS`）+ 5 字节数据 |
| IP 包 | 57 字节 | `4500 0039` 里 `0x0039 = 57`，即 20 字节 IP 头 + 37 字节负载 |
| 以太网帧 | 71 字节 | 14 字节帧头 + 57 字节 IP 包，与 tcpdump 的 `length 71` 一致 |

### 2.2 应用、IP 与链路的三个长度

同一个包有三个长度，各有各的用处：

- 应用关心 5，这是它写进去的字节数。
- `ip_rcv_core` 关心 57，这是 IP 头里声明的总长，决定了要交给上层多少字节。
- 驱动和 `tc` 关心 71，这是链路上实际搬运的字节数，MTU 比对、qdisc 计数都用它。

另有一段开销不进任何一层：线路上每个帧前面还有 8 字节前导码，帧与帧之间留 12 字节间隙。这两段不写进 `skb`，所以 `ip -s link` 的字节数永远对不上物理层速率，算带宽时要自己加上去。

### 2.3 66 字节头部与 13 倍固定开销

2.1 那张表里四层加起来，属于应用数据的只有 5 个字节，其余 66 字节都是各层头部。为送这 5 个字节，协议栈在每个包上花了 13 倍的代价。短请求、小响应躲不开这笔固定开销，各种合并小包的优化也大多出自这个动机。

## 三、发送侧：从 write 到描述符环

### 3.1 S1：五个字节进入 socket

`write()` 要做的事只有一件：把用户缓冲区里的字节交出去。从 fd 找到 `socket` 这一段由 socket 层完成，交给协议的入口是 `tcp_sendmsg`（[net/socket.c](https://github.com/torvalds/linux/blob/v7.0/net/socket.c)、[net/ipv4/tcp.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/tcp.c)）。数据在这一步换了主人：之前属于用户缓冲区，之后被拷进 skb，挂到 `sk_write_queue`，归 socket 所有。

### 3.2 S2：队列与发送判定

`tcp_sendmsg` 只负责入队，真正决定"现在发多少"的是 `tcp_write_xmit`：拥塞窗口 `snd_cwnd`、对端通告窗口、Nagle、TSQ 任何一个不满足，数据就继续排队。所以 `write()` 返回 5 只说明字节进了内核队列，不说明它们已经在路上；抓不到包而 `write` 成功，问题多半落在 S2 到 S6 之间。整条链上最慢的也是这一段：建 skb、更新队列、准备校验和都在这儿，5.1 的表里它单独占了 5.5 µs。

### 3.3 S3：IP 头与两次出方向 hook

TCP 段交给 IP 层后，路由在 `ip_queue_xmit` 里就定下来了：出口设备和源地址写进 `skb_dst`，后面拿到的 skb 都带着这个结果。接着填 IP 头、算校验和，两次出方向 hook `LOCAL_OUT` 与 `POST_ROUTING` 分别夹在填头之后和交给链路层之前（[net/ipv4/ip_output.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/ip_output.c)）。

### 3.4 S4：分片判断

`ip_finish_output` 只判断一件事：这个 skb 能不能整块交给下一跳。不超过 MTU 直接放过；超过且允许分片就进 `ip_fragment`；超过又置了 DF 位，就回一个 ICMP 并丢弃。本例 71 字节、MTU 1500，直接过。开了 GSO 的 skb 长度可以远超 MTU，但它交给网卡去切，不在协议栈里分片，这两种"分段"要分清。

### 3.5 S5：下一跳地址的 MAC 解析

`ip_finish_output2` 拿下一跳地址查邻居表：查到就填好帧头交给设备队列，查不到就发 ARP、把 skb 挂在 `neigh->arp_queue` 上等。邻居解析只在首次通信或缓存过期时发生，第一个包的延迟总比后续高，原因就在这里；这一帧的 ARP 在 `connect()` 时就完成了。

### 3.6 S6：tc egress 与 qdisc 入队

`__dev_queue_xmit` 是设备发送的总入口（[net/core/dev.c](https://github.com/torvalds/linux/blob/v7.0/net/core/dev.c)）：选发送队列、过 tc egress、入 qdisc，再交给驱动。tc 的 egress 挂点在 qdisc 之前，匹配走 `tcf_classify`。qdisc 差别很大：veth 的 `a0` 是 `noqueue`，没有排队，直接进 `dev_hard_start_xmit`；真实网卡是 `fq_codel`，有入队、有调度、有丢弃统计。所以"在 veth 上测不出队列延迟"不是方法问题，是这条路上根本没有队列。

### 3.7 S7：描述符、DMA 与门铃

`dev_hard_start_xmit` 交给驱动的发送函数，e1000e 里是 `e1000_xmit_frame`：取一个空闲描述符，把 skb 的 DMA 地址和长度写进去，敲门铃通知网卡。之后网卡自己发 DMA 读请求搬数据，CPU 不再参与，大包吞吐由 DMA 和总线决定，与 CPU 频率关系不大。环的大小和队列数一眼能看：`ethtool -g enp10s0` 给出 RX/TX 各 256，`ls /sys/class/net/enp10s0/queues/` 只有 `rx-0` 与 `tx-0`。

veth 没有这一站：`dev_hard_start_xmit` 直接调 `veth_xmit`，把 skb 交给对端 netns 的接收路径。这也是 5.2 里接收半程没有换线程的原因。

## 四、接收侧：从 DMA 到 read

### 4.1 R1：DMA 写内存与中断通知

网卡收到帧，用 DMA 写进 rx 环里某个描述符指向的缓冲区，回写描述符状态，触发 MSI-X 中断。驱动 `e1000_intr_msix_rx` 先把该队列的中断关掉，再 `napi_schedule` 把 poll 排进软中断——中断只负责叫醒，搬运交给轮询。这能省下多少中断，收一次大流就看得出：灌 20 MB 进来，一万四千多个报文只触发 82 次硬中断，平均 180 个包一次。

### 4.2 R2：NAPI poll 的批量取包

`net_rx_action` 遍历挂起的 napi 实例，按预算（`NAPI_POLL_WEIGHT`，默认 64）调各自的 poll 函数——e1000e 里是 `e1000_clean_rx_irq`，它从环上按顺序取描述符，为每个描述符建 skb，再用 `eth_type_trans` 从帧头取出 ethertype 写进 `skb->protocol`。同一批报文，一次 poll 平均取走 48 个包，`eth_type_trans` 则是一个包一次。

### 4.3 R3：GRO 合并同一条流的连续段

`napi_gro_receive` 在交给协议栈之前做一次合并：同一条流、同方向、序号连续的段，合进前一个 skb 的 frags。合并后的 skb 长度可以远超 MTU，因为它已经不是链路上的帧，而是给协议栈看的段。还是那 20 MB（脚本 [gro3.bt](gro3.bt)，七个 kprobe 各数一件事），一万四千多个 skb 只换来 610 次 IP 层处理，约 24 个合一个：GRO 省的不是中断，是上层遍历 skb 的次数。

### 4.4 R4：二层分发，tc 在 ptype 之前

`netif_receive_skb` → `__netif_receive_skb_core` 是二层分发的总入口，顺序固定：先过 tc ingress，再按 `skb->protocol` 在 `ptype_all` / `ptype_base` 里找处理函数。IPv4 在这里注册了两个入口：`ip_rcv` 处理单个 skb，`ip_list_rcv` 处理批量交付。网卡接收因为 GRO 会攒成 list，走的是 `ip_list_rcv` 那条，`ip_rcv` 一次都不会被调用——只画 `ip_rcv` 的路径图，在网卡这条路上是错的。不挂 tc 过滤器时，`sch_handle_ingress` 一开头就返回，tc 不参与。

### 4.5 R5：本地交付与转发的分岔

`ip_rcv_core` 之后进 `PRE_ROUTING`，`ip_rcv_finish` 用 `ip_route_input_noref` 查路由、写进 `skb_dst`。查完只有两条路：目的地址是本机就 `ip_local_deliver`（中间过 `LOCAL_IN`），不是本机且允许转发就 `ip_forward`（中间过 `FORWARD`）。这 5 个字节走前者，不碰转发那一段。

### 4.6 R6：四元组查找与入队

`ip_local_deliver_finish` → `ip_protocol_deliver_rcu` 按协议号找到 `tcp_v4_rcv`（[net/ipv4/tcp_ipv4.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/tcp_ipv4.c)）。TCP 用四元组做哈希，在 `ehash`（已建立连接）和 `lhash`（监听）里找到 socket，`tcp_v4_do_rcv` → `tcp_rcv_established` 把数据挂到 `sk_receive_queue`；序号不连续则先进 `out_of_order_queue` 等重传。到这里数据已经躺在接收 socket 的队列里，但接收进程还没醒。

### 4.7 R7：唤醒接收进程并拷贝数据

`tcp_rcv_established` 入队后调 `sk_data_ready`（默认实现 `sock_def_readable`），唤醒在 `sk_sleep` 上等的进程；被唤醒的 `read()` 进 `tcp_recvmsg`，把 `sk_receive_queue` 里的 skb 逐段拷到用户缓冲区。用了 epoll 的话，`sock_def_readable` 还会经 `ep_poll_callback` 把就绪事件挂到就绪链表——这条链是第二篇的题目。

## 五、一次五字节收发的函数时间线

### 5.1 二十七个探针点与时间线

把 14 站拆成 27 个探针点，先后和量级就都落在一条时间轴上。原始记录在 [trace-5byte.txt](trace-5byte.txt)，探针脚本是 [tl3.bt](tl3.bt)；`Δ` 是相邻两点的差，`累计` 从 `__sock_sendmsg` 起算。

采集时让 bpftrace 直接把被测进程拉起来，只记录这一趟收发：

```bash
sudo bpftrace tl3.bt -c 'ip netns exec nslab-a ./netdemo tcp-client 10.77.1.2 18082 5'
```

跑之前先在另一个终端起服务端（1.2 节那条 `tcp-server`）。另外不要同时开 tcpdump：它走 AF_PACKET，每个包都要多复制一份并唤醒读进程，时间线就不干净了。

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

### 5.2 执行上下文与耗时量级

27 个点的 `tid` 全是 4443、`cpu` 全是 0：B 的接收处理是在 A 的 `write()` 里跑完的，中途没有换线程。原因在 veth：`veth_xmit` → `dev_forward_skb` → `netif_rx` 把 skb 挂到本 CPU 的 `process_queue`，等这一层 `local_bh_enable` 时就地触发 `net_rx_action`，于是 `enqueue_to_backlog` 和 `net_rx_action` 在表里紧挨着。

换一种触发方式，执行者就变了：同一条路径也可能落在 `ksoftirqd` 上，取决于软中断在哪个上下文里被抬起。所以"接收路径属于接收进程"这句话是错的，**它属于当时正在执行软中断的那个任务**。

绝对耗时不能当真实值读：换几组探针数，同一段路径的总量在十几到二十几微秒之间跳，与探针自身的开销同量级。能用的是顺序和量级——整趟不到 20 µs，最长的两段都在发送侧，其余每站几百纳秒。要测绝对耗时，得减少探针、换 `fentry`，或者直接用 `perf`。

### 5.3 测量的边界

这份记录不覆盖三样东西：S7 和 R1（veth 没有 DMA、描述符环和中断，那两站的数字来自网卡实验）；函数内部的耗时（`tcp_transmit_skb` 在本机内核上没有独立符号，时间线从 `tcp_write_xmit` 直接跳到 `ip_queue_xmit`）；并发行为（拥塞窗口、qdisc 排队、GRO 合并要批量场景才显现）。

## 六、netfilter hook、tc 与 XDP 的挂点

### 6.1 五个 hook 的位置与参与计数

netfilter 的 IPv4 hook 有五个，编号和出现位置固定：

| 编号 | 常量 | 出现在 | 无规则时 | 空基链 | 加 tc 过滤 |
| --- | --- | --- | --- | --- | --- |
| 0 | `NF_INET_PRE_ROUTING` | `ip_rcv_core` 之后、`ip_rcv_finish` 之前 | 0 | 8 | 8 |
| 1 | `NF_INET_LOCAL_IN` | `ip_local_deliver` 内 | 0 | 8 | 8 |
| 2 | `NF_INET_FORWARD` | `ip_forward` 内 | 0 | 0 | 0 |
| 3 | `NF_INET_LOCAL_OUT` | `__ip_local_out` 内 | 8 | 8 | 8 |
| 4 | `NF_INET_POST_ROUTING` | `ip_output` 之后、`ip_finish_output` 之前 | 8 | 8 | 8 |
| — | `tcf_classify` | egress 与 ingress 各一处 | 0 | 0 | 8 |

三列分别是：不装 nft、两个 netns 各装五个空基链、再加 `clsact` 与 `matchall`。0 表示这一列里没有包经过那个 hook，非 0 表示每个包都停了一下；绝对值随连接里的包数浮动，不必正好是 8。配置与计数的完整过程在 [netfilter-hooks.txt](netfilter-hooks.txt)。

由此看清两件事：入方向的两个 hook 要装了 filter 基链才会进 `nf_hook_slow`，规则为空也照样进，只是立即返回；出方向的两个 hook 一条规则都没有也在调，而且不是 nft 干的。

### 6.2 出方向两个 hook 上的 AppArmor 表项

在一个全新的 netns 里（`nft list ruleset` 为空）ping 三次回环，hook 3 和 hook 4 仍各有 6 次进入 `nf_hook_slow`。把表项里的函数地址打出来，挂的是 `apparmor_ip_localout` 和 `apparmor_ip_postroute`（脚本 [hooks.bt](hooks.bt)）：Ubuntu 默认启用的 AppArmor 在出方向常驻一条表项，与 nft 规则无关。

所以判断包有没有经过 netfilter，看的是 `nf_hook_slow` 有没有被调用，不是 `nft list ruleset` 空不空。"我没配防火墙，所以包没经过 netfilter"这句话，在 Ubuntu 上基本是错的。

### 6.3 三个挂点的先后顺序

- `tc egress` 在 `__dev_queue_xmit` 里、qdisc 入队之前，作用对象是已经走完 IP 层、即将下发的 skb。
- `tc ingress` 在 `__netif_receive_skb_core` 里、ptype 分发之前，作用对象是刚建好、协议字段已填但还没进 IP 层的 skb。
- `XDP` 在驱动里、`skb` 还没分配之前，拿到的是 `xdp_buff`，只能看到线性区的原始帧。能改的东西比 tc 少，代价也最低。

这三者的顺序决定了它们能做什么：XDP 最早，看不到 `skb` 的元数据；tc ingress 能看到 `skb`，但还没有路由结果；tc egress 已经知道出口设备。eBPF 程序可以挂在这三处，本篇只标出位置，程序写法不在十篇的实验范围内。

## 七、现象定位与后续篇目

### 7.1 现象与对应站点

| 现象 | 先看哪一站 | 命令 |
| --- | --- | --- |
| `write` 返回 5，但抓包没有 | S2–S6 | `ss -ti`、`tc -s qdisc show` |
| 本机抓到了，对端抓不到 | S6–S7 | `ip -s link`、`ethtool -S` |
| 对端抓到了，应用读不到 | R4–R7 | `ss -mni`、`cat /proc/net/snmp` |
| 收包丢在环上 | R1–R2 | `ethtool -S enp10s0 \| grep -i drop` |
| 收包丢在协议栈 | R4–R5 | `nstat -az`、`tc -s filter show` |
| 延迟忽高忽低 | R1–R2、S6 | `cat /proc/interrupts`、`tc -s qdisc` |

### 7.2 各站与后续篇目

| 站 | 交给 |
| --- | --- |
| S1、S2、R6、R7 | 第二篇 socket 与多路复用 |
| S3 的 skb 结构、S4 的分片 | 第三篇 sk_buff |
| R1、R2、R3 | 第四篇 驱动与 NAPI |
| S5 | 第五篇 路由与邻居 |
| S6 | 第六篇 qdisc 与流控 |
| 配置类命令怎么下发 | 第七篇 Netlink |
| 这套拓扑怎么造出来 | 第八篇 网络命名空间 |
| veth 与 bridge 的内部 | 第九篇 veth 与 bridge |
| hook 与 conntrack | 第十篇 Netfilter 与连接跟踪 |

## 八、命令与接口速查

| 命令 | 用途 | 关键输出 |
| --- | --- | --- |
| `wget -q https://lixiaojun2914.github.io/p/linux-network-stack-map/netdemo.c` | 取源码、建网脚本、三个 bt 脚本 | 落到当前目录，其余四个同理 |
| `sudo bash lab.sh direct` | 建两个 netns 加一对 veth | `nslab-a` / `nslab-b`，`lab.sh clean` 清掉 |
| `sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.1.2 18082 1500` | 起服务端（前台） | `ready TCP`，接上后等 1500 ms 再 `received=5 bytes` |
| `sudo ip netns exec nslab-a ./netdemo tcp-client 10.77.1.2 18082 5` | 发 5 字节 | 客户端 `write accepted=5 bytes`，服务端 `received=5 bytes` |
| `sudo ip netns exec nslab-a tcpdump -i a0 -nn -e -X -c 8 'tcp port 18082'` | 看链路层帧 | `length 71`，末尾 5 字节是负载 |
| `sudo bpftrace tl3.bt -c 'ip netns exec nslab-a ./netdemo tcp-client 10.77.1.2 18082 5'` | 抓 27 点函数时间线 | 带 `tid`、`cpu`，顺序为主 |
| `nc -l 9002 > /dev/null` + 宿主机灌 20 MB | 造批量收包流量 | 配合 `gro3.bt` 看合并比 |
| `sudo bpftrace gro3.bt` | 数 skb、ip_list_rcv、poll、中断 | 7 个计数点 |
| `write(fd, buf, 5)` | 发送 5 字节 | 返回 5 只表示进了内核队列 |
| `tc -s qdisc show dev enp10s0` | 看排队与丢弃 | `sent/backlog/drops` |
| `nft list ruleset` | 看 nft 规则 | 空不等于 hook 没参与 |
| `ethtool -g enp10s0` | 看环形缓冲 | RX/TX 256 |
| `ethtool -S enp10s0 \| grep -i drop` | 看网卡侧丢弃 | `rx_missed_errors` 等 |
| `cat /proc/interrupts` | 看中断分布 | 每队列一条 MSI-X |
| `ss -ti` | 看 TCP 内部状态 | `cwnd`、`rtt`、`retrans` |
| `ss -mni` | 看 socket 内存与队列 | `skmem`、`recv-q` |
| `ip -s link show enp10s0` | 看设备计数 | `RX/TX packets/errors/dropped` |
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
2026-11-27 修订：统一 H2/H3 为陈述式标题（对齐其余九篇）、速查小节改名「命令与接口速查」；2.1 删去与 1.2 重复的 tcpdump 命令及说明；速查表删去正文未使用的命令并与正文写法对齐；修正 6.3 的篇目预告。
2026-11-27 二审：H3 统一为名词短语；2.1 只留输出怎么读，参数与过滤交回 1.2；2.3 不再重复 2.1 表内已列的头长。
2026-11-27 四审：按 roadmap 重排。三、四章每站压到两三句，删去调用顺序树、分片分支细节、逐站命令块与 nft/tc 三组构造命令；GRO 与 hook 的计数只留表、比值和结论，命令与过程回到 netfilter-hooks.txt 等记录文件。
2026-11-27 三审：正文去掉“实测”标签与逐站 ns 读数（数字留给 5.1 表与记录文件），计数改为量级与比值表述，把逐次实验的口吻改成机制陈述。
已清理旧结构的五张 SVG（topology、three-paths、capture-points、rx-data-notification、socket-wait-stages），只保留 path.svg。
下一步：校验通过后改写第二篇（socket 与多路复用、LT/ET、四组 demo），其余八篇随后。
-->
