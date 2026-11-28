---
title: "Linux 网络栈（六）：出口排队、qdisc 与延迟控制"
slug: linux-network-qdisc
date: "2026-11-22T16:06:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "同样 5 Mbit 的出口，换个队列策略，ping 从 134 ms 回到 9 ms：等待堆在哪里，怎么把它排掉。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

网卡还在发，ping 也没全丢，延迟却从不到一毫秒涨到一百多毫秒。这段等待发生在哪里？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · **6** · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

第一篇的块 C 里，数据过了路由就被"交给设备"，中间只留了一句"这里可能会排队"。这一篇把那一句展开：排队发生在哪一层，排队规则怎么决定顺序和丢弃，以及为什么加大缓冲不是解法。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。出口固定限速 5 Mbit/s，只比较 FIFO 与 FQ-CoDel 两种子队列，不涉及吞吐极限和复杂生产配置。

## 一、四种等待

![socket、qdisc、驱动和设备分别持有不同阶段的等待数据](/p/linux-network-qdisc/queues.svg)

沿发送方向看，数据可能停在四个地方，各有各的原因：

| 位置 | 主要问题 | 看什么 |
| --- | --- | --- |
| socket / TCP | 未确认数据、窗口、发送内存 | `ss -tinm` |
| qdisc | 等待服务、限速、调度、丢弃或标记 | `tc -s qdisc` |
| 驱动与设备 | 描述符、发送资源 | 驱动统计、环形队列 |
| 链路 | 已在传播的信号 | 时延测量 |

持续排队的根源只有一条：到达速度长期大于可服务速度。加缓冲能让丢弃晚一点发生，代价是把丢弃换成了等待；短时突发确实需要缓冲，长期积压则必须靠调度、限速或反馈来处理。

## 二、固定瓶颈的对照

### 2.1 测试条件

用第一篇的程序和脚本建直连拓扑，在 A 的出口挂 TBF 限速，子队列先放 FIFO：

```bash
sudo bash lab.sh clean
sudo bash lab.sh direct
sudo ip netns exec nslab-a tc qdisc add dev a0 root handle 1: tbf rate 5mbit burst 16kb latency 1000ms
sudo ip netns exec nslab-a tc qdisc add dev a0 parent 1:1 handle 10: pfifo limit 1000
```

`lab.sh` 拒绝在已有 `nslab-*` 命名空间上再建一套，所以前面先 `clean`。

TBF（令牌桶）把出口压到 5 Mbit/s；`burst` 是允许的突发额度；`latency` 只用于 TBF 自己算默认队列长度，挂上自定义子队列后实际限制由子队列的 `limit` 决定，不是"每个包等一秒"。

终端一起接收端，它会一直等到对端关闭：

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.1.2 18083
```

终端二把下面整块一次贴完：客户端放到后台，睡 1 秒让传输真跑起来，再测往返、读队列，最后等客户端写完。

```bash
sudo ip netns exec nslab-a ./netdemo tcp-client 10.77.1.2 18083 4194304 > cli.txt 2>&1 &
sleep 1
sudo ip netns exec nslab-a ping -n -c 10 -i 0.1 10.77.1.2
sudo ip netns exec nslab-a tc -s qdisc show dev a0
wait
cat cli.txt
```

这样安排是为了不跟时间抢：4 MiB 走 5 Mbit/s 只有六秒上下，人先等客户端跑完再切终端去敲 `ping`，多半已经错过窗口。客户端在后台占着发送路径，`ping` 和 `tc` 就一定落在传输进行中；`wait` 等到它退出，`cat` 再把它的输出补上。

客户端的 `write accepted=4194304 bytes in 5197 ms` 只说明本机 socket 收下了这么多；服务端的 `received=4194304 bytes` 才是对端应用读完。

### 2.2 只换子队列

两条根规则删掉重建，速率不动，子队列换成 FQ-CoDel；终端二的整块命令原样再贴一次（`cli.txt` 会被覆盖）：

```bash
sudo ip netns exec nslab-a tc qdisc del dev a0 root
sudo ip netns exec nslab-a tc qdisc add dev a0 root handle 1: tbf rate 5mbit burst 16kb latency 1000ms
sudo ip netns exec nslab-a tc qdisc add dev a0 parent 1:1 handle 10: fq_codel limit 1000 target 5ms interval 100ms
```

换子队列要连根一起删，是因为 `tbf` 的子队列挂在 `1:1` 下面，单删子队列会留下悬空的父句柄。服务端上一轮已经退出，重新起一次即可。

FQ 按流分队列，让一条大流不至于把别的流一直压在后面；CoDel 看的是排队驻留时间，超过 `target` 持续一段时间就丢包或标记。`target 5ms` 是控制的瞄准点，不是"每个 ping 必低于 5 ms"的承诺。

## 三、延迟与积压

### 3.1 同一负载，两种策略

| 指标 | FIFO | FQ-CoDel |
| --- | ---: | ---: |
| ping 平均 RTT | 132.4 ms | 8.8 ms |
| ping 最大 RTT | 169.5 ms | 20.1 ms |
| ping 收到 / 发送 | 10 / 10 | 10 / 10 |
| 采样时子队列 backlog | 118092 B，78 包 | 30280 B，2 包 |
| 累计 dropped | 0 | 5 |
| 4 MiB 写入耗时 | 5198 ms | 6416 ms |

平均和最大值取自 `ping` 最后一行的 `min/avg/max/mdev`；backlog 和 dropped 取自 `tc -s qdisc show dev a0` 里子队列那一段的 `backlog` 与 `dropped`；写入耗时取自 `cat cli.txt` 那行 `write accepted=... in ... ms`。

空载时这段 veth 的 ping 是零点几毫秒；两轮的服务速率相同，写入耗时都在六秒上下，说明吞吐没变，变的只是等待怎么分配。

FIFO 那一列很直白：78 个包排在队列里，每个包都要等前面几十个发完，ping 的往返自然被拉到百毫秒级。dropped 是 0——它没丢，它只是在等。

FQ-CoDel 把队列压到 2 个包，代价是丢掉 5 个。丢的是大流的数据，TCP 会重传；换来的是 ping 能插进去。这就是 AQM 的取舍：用少量丢弃换排队时间。

原始记录都在同目录：[FIFO 的 ping](/p/linux-network-qdisc/q-fifo-ping.txt)、[FIFO 的队列快照](/p/linux-network-qdisc/q-fifo-stats.txt)、[FQ-CoDel 的 ping](/p/linux-network-qdisc/q-fq_codel-ping.txt)、[FQ-CoDel 的队列快照](/p/linux-network-qdisc/q-fq_codel-stats.txt)。

这是单次样本，两台虚拟机之间的一次对照。它能说明机制的方向，不能当性能倍数用。真要下结论，得重复多次、固定负载和采样时机，同时看吞吐、时延分布和丢弃。

### 3.2 tc 的计数怎么读

| 字段 | 含义与边界 |
| --- | --- |
| `Sent` | 经这个 qdisc 统计的字节和包数，受层级和采样时刻影响 |
| `backlog` | 采样那一刻排队中的字节与包数 |
| `dropped` | 该队列累计丢弃，不能直接归到某个应用 |
| `overlimits` | 触发限速或调度条件的次数，不是丢包数 |
| `requeues` | 交付受阻后重新入队的次数，与 TCP 重传无关 |
| `ecn_mark` | ECN 标记计数，这轮快照是 0 |

ECN（显式拥塞通知）在条件满足时用标记代替丢弃来传话。配置里出现 `ecn` 不代表真的发生了标记，要看 `ecn_mark` 有没有涨。

## 四、qdisc 在发送路径上的位置

`__dev_queue_xmit()` 选发送队列，选 qdisc，然后按规则决定直接发还是入队。排队算法只实现 enqueue/dequeue 这些操作，取出来的包再交给驱动的发送回调；设备忙时还有重新入队这条路。所以"任何包都先在一个 FIFO 里等"是错的，路径取决于 qdisc 类型和设备状态。入口见 [dev.c](https://github.com/torvalds/linux/blob/v7.0/net/core/dev.c) 和 [sch_generic.c](https://github.com/torvalds/linux/blob/v7.0/net/sched/sch_generic.c)。

| 策略 | 提供什么 | 分析时看什么 |
| --- | --- | --- |
| FIFO | 按到达顺序排队 | 队列长度、流之间互相挤压 |
| FQ-CoDel | 流调度 + 延迟控制 | 持续积压、交互延迟、丢弃与标记 |
| fq | 按流组织，支持 pacing | socket 给出的发送时间与流服务 |
| netem | 人为制造延迟、丢包、重排 | 搭实验环境，不是队列管理机制本身 |

pacing、AQM、调度是三件事：pacing 安排发送时刻，AQM 处理持续积压，调度决定谁先谁后。相关，但别混成一个词。FQ-CoDel 的算法见 [RFC 8290](https://www.rfc-editor.org/rfc/rfc8290) 和[实现](https://github.com/torvalds/linux/blob/v7.0/net/sched/sch_fq_codel.c)。

BDP 是速率乘时延，用来估算路径上在途的数据量，不是"队列必须设这么大"的规定。流数、拥塞控制、瓶颈位置都会改变结论。

收尾：

```bash
sudo ip netns exec nslab-a tc qdisc del dev a0 root
sudo bash lab.sh clean
```

## 五、命令与接口速查

| 命令 | 用途 |
| --- | --- |
| `tc -s qdisc show dev DEV` | 看规则和统计 |
| `root handle 1:` | 设根队列与句柄 |
| `parent 1:1 handle 10:` | 挂到 TBF 的子队列位置 |
| `tc qdisc del dev DEV root` | 删掉出口规则 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| backlog | 当前还没被服务的排队数据 |
| bufferbloat | 缓冲过大导致持续高延迟 |
| 整形 | 通过安排发送时机控制出口速率 |
| AQM | 主动队列管理，用丢弃或标记压制持续积压 |
| clsact | 只提供 ingress/egress 挂点的特殊 qdisc，不是普通缓存队列 |
