---
title: "Linux 网络栈（六）：出口排队、qdisc 与延迟控制"
slug: linux-network-qdisc
date: "2026-11-22T16:06:00+08:00"
lastmod: "2026-11-28"
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

用第一篇的程序和脚本建直连拓扑。下面所有命令都在 `lab.sh` 和 `netdemo` 所在的目录里执行：

```bash
sudo bash lab.sh clean
sudo bash lab.sh direct
```

`lab.sh` 拒绝在已有 `nslab-*` 命名空间上再建一套，所以前面先 `clean`。

还没挂任何限速，先量一次空载往返：

```bash
sudo ip netns exec nslab-a ping -n -c 10 -i 0.1 10.77.1.2
```

```text
10 packets transmitted, 10 received, 0% packet loss, time 938ms
rtt min/avg/max/mdev = 0.014/0.060/0.088/0.024 ms
```

零点零几毫秒，比后面两轮的百毫秒低三个数量级。这就是没有排队时的底数。

在 A 的出口挂 TBF 限速，子队列先放 FIFO：

```bash
sudo ip netns exec nslab-a tc qdisc add dev a0 root handle 1: tbf rate 5mbit burst 16kb latency 1000ms
sudo ip netns exec nslab-a tc qdisc add dev a0 parent 1:1 handle 10: pfifo limit 1000
```

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

客户端的 `write accepted=4194304 bytes in 5904.625 ms` 只说明本机 socket 收下了这么多；服务端的 `received=4194304 bytes` 才是对端应用读完。

### 2.2 只换子队列

两条根规则删掉重建，速率不动，子队列换成 FQ-CoDel；终端二的整块命令原样再贴一次（`cli.txt` 会被覆盖）：

```bash
sudo ip netns exec nslab-a tc qdisc del dev a0 root
sudo ip netns exec nslab-a tc qdisc add dev a0 root handle 1: tbf rate 5mbit burst 16kb latency 1000ms
sudo ip netns exec nslab-a tc qdisc add dev a0 parent 1:1 handle 10: fq_codel limit 1000 target 5ms interval 100ms
```

换子队列要连根一起删，是因为 `tbf` 的子队列挂在 `1:1` 下面，单删子队列会留下悬空的父句柄。服务端上一轮已经退出，重新起一次即可。

FQ 按流分队列，让一条大流不至于把别的流一直压在后面；CoDel 看的是排队驻留时间，超过 `target` 持续一段时间就丢包或标记。`target 5ms` 是控制的瞄准点，不是"每个 ping 必低于 5 ms"的承诺。

### 2.3 句柄、类、过滤器与动作

前面两条命令里的 `root handle 1:` 和 `parent 1:1 handle 10:`，拆开是这样的关系：

| 写法 | 意思 |
| --- | --- |
| `root` | 挂在这台设备的根上，一台设备只有一个根队列 |
| `handle 1:` | 给这个 qdisc 编号 `1:`，冒号后面是类编号，根这一层是 0 |
| `parent 1:1` | 挂到 `1:` 的第 1 个子类下面 |
| `handle 10:` | 子队列自己的编号 |

tbf 这种 qdisc 不直接收包排队，它先按类组织，真正排队的是类下面挂的子队列。`tc class show` 能看到 tbf 自己建的那个类：

```bash
sudo ip netns exec nslab-a tc class show dev a0
```

```text
class tbf 1:1 parent 1: leaf 10:
```

`leaf 10:` 正是 2.1 里挂上去的 pfifo，和 `tc qdisc show` 里那行 `qdisc pfifo 10: parent 1:1` 是同一件事的两头。所以 qdisc 分两类：pfifo、fq_codel、fq 没有类，包进来就排；tbf、htb、prio 有类，可以用过滤器把不同流量分进不同的类，每个类下面再挂自己的子队列。

过滤器（filter）就是"按条件选类"，只对有类的 qdisc 有意义。设备上现在是 tbf 加 fq_codel，先删掉根换成 htb，开两个不同速率的类：

```bash
sudo ip netns exec nslab-a tc qdisc del dev a0 root
sudo ip netns exec nslab-a tc qdisc add dev a0 root handle 1: htb default 20
sudo ip netns exec nslab-a tc class add dev a0 parent 1: classid 1:10 htb rate 2mbit
sudo ip netns exec nslab-a tc class add dev a0 parent 1: classid 1:20 htb rate 20mbit
sudo ip netns exec nslab-a tc filter add dev a0 parent 1: protocol ip prio 1 u32 match ip dport 18085 0xffff flowid 1:10
```

`default 20` 指没被任何过滤器命中的流量落到哪个类；`prio` 决定同一父队列上多条规则的比较顺序；`flowid` 是命中去哪个类。发两个数据报，一个去 18085、一个去 18086，再看分类计数：

```bash
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.1.2 18085 &
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.1.2 18086 &
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.1.2 18085
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.1.2 18086
sudo ip netns exec nslab-a tc -s class show dev a0
sudo pkill -x netdemo
```

两个 `udp-server` 是后台起的，最后一条按可执行文件名把它们一起收掉；如果这台机器上还跑着别的 `netdemo`，也会一并结束。

```text
class htb 1:10 root prio 0 rate 2Mbit ceil 2Mbit burst 1600b cburst 1600b
 Sent 92 bytes 2 pkt (dropped 0, overlimits 0 requeues 0)
class htb 1:20 root prio 0 rate 20Mbit ceil 20Mbit burst 1600b cburst 1600b
 Sent 310 bytes 5 pkt (dropped 0, overlimits 0 requeues 0)
```

两个类各算各的：目的端口 18085 的数据报落到 `1:10`，其余——18086 的数据报加上链路上的邻居解析——落到默认类 `1:20`。字节数随当轮流量变，要看的是哪个类在涨：计数不在根上，在类上。

动作（action）是另一层。命中的包除了交给某个类，还可以先执行一个动作：

```bash
sudo ip netns exec nslab-a tc filter add dev a0 parent 1: protocol ip prio 2 u32 match ip dport 18099 0xffff action drop
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.1.2 18099
sudo ip netns exec nslab-a tc -s filter show dev a0
```

```text
	action order 1: gact action drop
	Action statistics:
	Sent 92 bytes 2 pkt (dropped 2, overlimits 0 requeues 0)
```

这条规则没有 `flowid`，命中就丢，计数记在 action 上；带 `flowid` 的规则负责分流，带 `action` 的规则负责执行动作，两者可以写在同一条里。常见的动作还有 `police`（超速就丢或打标）和 `mirred`（重定向到另一台设备）。

`tc filter show` 里那几行 `match 000046a5/0000ffff at 20` 不是给人读的：18085 就是 `0x46a5`，`at 20` 指的是 IP 头往后 20 字节处的那个 16 位字段——端口。u32 把条件编译成偏移加掩码，读规则时知道这一点就够了。

## 三、延迟与积压

### 3.1 同一负载，两种策略

| 指标 | 空载 | FIFO | FQ-CoDel |
| --- | ---: | ---: | ---: |
| ping 平均 RTT | 0.060 ms | 130.7 ms | 10.7 ms |
| ping 最大 RTT | 0.088 ms | 175.3 ms | 19.3 ms |
| ping 收到 / 发送 | 10 / 10 | 10 / 10 | 10 / 10 |
| 采样时子队列 backlog | — | 109008 B，72 包 | 24224 B，2 包 |
| 累计 dropped | — | 0 | 4 |
| 4 MiB 写入耗时 | — | 5905 ms | 6499 ms |

平均和最大值取自 `ping` 最后一行的 `min/avg/max/mdev`（`130.659` 与 `175.255` 这样的原值，表里只保留一位小数），收发数取自同一文件上一行的 `10 packets transmitted, 10 received`；backlog 和 dropped 取自 `tc -s qdisc show dev a0` 里子队列那一段的 `backlog <字节>b <包数>p` 与 `dropped`；写入耗时取自客户端输出的 `write accepted=... in 5904.625 ms`，表里四舍五入到毫秒。

两轮的服务速率相同，写入耗时都在六秒上下，说明吞吐没变，变的只是等待怎么分配。空载那一列是挂 TBF 之前量的，用来标出这段 veth 自己的底噪。

FIFO 那一列很直白：72 个包排在队列里，每个包都要等前面几十个发完，ping 的往返自然被拉到百毫秒级。dropped 是 0——它没丢，它只是在等。

FQ-CoDel 把队列压到 2 个包，代价是丢掉 4 个。丢的是大流的数据，TCP 会重传；换来的是 ping 能插进去。这就是 AQM 的取舍：用少量丢弃换排队时间。

原始记录都在同目录：[空载 ping](/p/linux-network-qdisc/q-idle-ping.txt)、[类与过滤器的输出](/p/linux-network-qdisc/q-classes.txt)、[FIFO 的 ping](/p/linux-network-qdisc/q-fifo-ping.txt)、[FIFO 的队列快照](/p/linux-network-qdisc/q-fifo-stats.txt)、[FQ-CoDel 的 ping](/p/linux-network-qdisc/q-fq_codel-ping.txt)、[FQ-CoDel 的队列快照](/p/linux-network-qdisc/q-fq_codel-stats.txt)、[两轮的写入与对端读取](/p/linux-network-qdisc/q-write.txt)。

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

有类 qdisc 把包统计记在类上，2.3 那两个 htb 类的 `Sent` 就各算各的；根上 `tc -s qdisc show` 看到的是汇总，要分档就得看 `tc -s class show`。

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
| `tc qdisc add dev DEV root handle 1: tbf ...` | 设根队列、限速与句柄 |
| `tc qdisc add dev DEV parent 1:1 handle 10: ...` | 挂到 TBF 的子队列位置 |
| `tc class show dev DEV` | 看有类 qdisc 里的类与 `leaf`，对应 `parent` 指到哪 |
| `tc filter add ... flowid 1:10` | 按条件把流量分进某个类 |
| `tc filter add ... action drop` | 命中后执行动作，计数记在 action 上 |
| `tc -s class show dev DEV` | 按类看 `Sent` 与丢弃，不在根上看 |
| `tc -s filter show dev DEV` | 看规则与 action 的命中计数 |
| `tc -s qdisc show dev DEV` | 看规则和统计，`backlog`/`dropped` 在这 |
| `tc qdisc del dev DEV root` | 删掉出口规则，换子队列要连根一起删 |
| `ss -tinm` | 看 socket 一侧的未确认数据、窗口与发送队列 |
| `ping -c 10 -i 0.1 DEST` | 传输进行中量往返，`-i` 压低间隔好落在同一窗口 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| backlog | 当前还没被服务的排队数据 |
| 句柄（handle） | qdisc 的编号，形如 `1:`；冒号前是队列号，后面是类号 |
| 类（class） | 有类 qdisc 内部的分支，可挂子队列或由过滤器选中 |
| 过滤器（filter） | 按协议、地址、端口等条件把包指向某个类的规则 |
| 动作（action） | 挂在规则或 qdisc 上、对命中包执行的操作，如丢弃、限速、重定向 |
| bufferbloat | 缓冲过大导致持续高延迟 |
| 整形 | 通过安排发送时机控制出口速率 |
| AQM | 主动队列管理，用丢弃或标记压制持续积压 |
| clsact | 只提供 ingress/egress 挂点的特殊 qdisc，不是普通缓存队列 |
