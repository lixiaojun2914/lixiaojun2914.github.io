---
title: "Linux 网络栈（四）：驱动、中断与 NAPI 接收处理"
slug: linux-network-driver-napi
date: "2026-11-22T16:04:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "设备通知、NAPI 调度与 poll 预算怎样把一次中断摊成一批处理；三个计数各自算什么，为什么它们不必相等。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

数据到了网卡，是中断处理函数一直读完，还是另有执行者接手？流量持续不断时，它又凭什么让出 CPU？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · **4** · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

第一篇的块 C 停在驱动把帧交给设备，块 D 从网卡把帧收进内存开始。中间这一段——谁先知道有数据、谁把数据取走、一次取多少就停下——就是这一篇。块 D 之后的路由与交付留给第五篇。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。这一篇用的是 NAT 网卡 `enp2s0`，它挂在真实的 e1000e 驱动上；第一篇实验流量走的 veth 对没有物理设备，看不到中断和 NAPI。

## 一、设备、队列与驱动

```bash
sudo ethtool -i enp2s0
```

```text
driver: e1000e
version: 7.0.0-44-generic
firmware-version: 1.8-0
bus-info: 0000:02:00.0
supports-statistics: yes
supports-test: yes
supports-eeprom-access: yes
supports-register-dump: yes
supports-priv-flags: yes
```

这是 Fusion 提供的虚拟网卡，虚拟机里运行的是真正的 e1000e 驱动，所以中断、NAPI、描述符这套机制都是真实存在的；但吞吐和延迟受宿主机调度影响，不能拿来估物理网卡的上限。

设备注册只让内核认识这个接口。真正收发还要三样东西：描述符环、缓冲区、事件处理。Rx 环告诉驱动哪些缓冲区已经有数据；Tx 环记录哪些提交的资源可以回收了。

`enp2s0` 有独立的接收与发送中断向量，说明它用的是 MSI-X 多向量。这两条命令能把"几个队列、几条中断线"看清楚：

```bash
ls /sys/class/net/enp2s0/queues/
grep enp2s0 /proc/interrupts
```

```text
rx-0
tx-0
 50:      46972          0 GICv2m-PCI-MSIX-0000:02:00.0   0 Edge      enp2s0-rx-0
 51:       8696          0 GICv2m-PCI-MSIX-0000:02:00.0   1 Edge      enp2s0-tx-0
 52:          2          0 GICv2m-PCI-MSIX-0000:02:00.0   2 Edge      enp2s0
```

![接收队列、MSI-X 向量、NAPI 实例与软中断额度的对应关系](/p/linux-network-driver-napi/vectors.svg)

两条队列、三条中断线：`rx-0`、`tx-0` 各一条，再一条给"其他"事件。真正走 NAPI 的只有接收那一条——一个接收队列、一条接收中断线、一个 `napi_struct` 绑在一起，收包的工作最后落到软中断里的 `napi_poll`，每轮最多取 64 个。发送完成和链路事件各有自己的处理函数，不进 poll，也就不受这个预算的约束。

一个设备可以有多个队列、多个 NAPI 实例，队列和中断怎样对应由驱动决定，不是内核统一规定；想知道这块网卡具体怎么分，只能看它自己的队列目录和中断表。

## 二、通知与预算

![设备中断、NAPI 调度、poll 预算和完成后恢复通知的循环](/p/linux-network-driver-napi/poll.svg)

### 2.1 中断只负责通知

如果每个包都要走一遍完整的中断处理，持续流量下的进出中断成本会吃掉大部分时间。常见做法是：设备通知"有事可做"，驱动调度一个 NAPI 实例，剩下的工作由 poll 回调批量取走。

```c
/* e1000_intr_msi() 与 e1000_intr_msix_rx() 的同一段收尾 */
if (napi_schedule_prep(&adapter->napi)) {
	adapter->total_rx_bytes = 0;
	adapter->total_rx_packets = 0;
	__napi_schedule(&adapter->napi);
}
```

`napi_schedule_prep()` 判断这个实例是不是已经在调度或运行中，是就不再重复入队；`__napi_schedule()` 把它挂到当前 CPU 的待处理列表，随后软中断调用它的 poll 回调。

通知在 e1000e 里不对称：中断入口只管调度，屏蔽靠硬件——读一次 `ICR` 就把这一路关掉了；重新打开在 poll 收尾，`napi_complete_done()` 返回真之后才 `ew32(IMS, rx_ring->ims_val)`。如果这一轮返回的是 `budget`，就压根走不到这里，中断保持关闭，下一轮软中断接着干。所以"关中断—收包—开中断"这种三段式只是示意，屏蔽与恢复分散在两头。

### 2.2 budget 限定一次 poll

`e1000e_poll()` 的核心几行：

```c
/* 省略前面的 adapter 初始化与 Tx 回收 */
adapter->clean_rx(adapter->rx_ring, &work_done, budget);

if (!tx_cleaned || work_done == budget)
	return budget;

/* 之后调用 napi_complete_done()，按结果恢复设备通知 */
```

`work_done` 是这次 poll 实际处理的工作量，`budget` 是给它定的上限。返回 `budget` 表示"还有活没干完"，实例会留在调度队列里，软中断结束时再来一轮；返回小于 `budget` 才收尾并恢复通知。

预算只约束 Rx 这一侧。Tx 完成回收走的是另一套条件，`work_done == budget` 时也会提前返回，两者不能混成一句"每次最多处理这么多事件"。

`struct napi_struct` 上挂着状态、poll 回调、所属设备和链表节点，驱动把它关联到 `net_device`。代码见 [e1000e/netdev.c](https://github.com/torvalds/linux/blob/v7.0/drivers/net/ethernet/intel/e1000e/netdev.c) 与 [netdevice.h](https://github.com/torvalds/linux/blob/v7.0/include/linux/netdevice.h)。

### 2.3 软中断自己的额度

一个 poll 有 64 的预算，可一轮软中断能把好几个实例的 poll 都跑一遍。它也有限度：

```bash
cat /proc/sys/net/core/netdev_budget
cat /proc/sys/net/core/netdev_budget_usecs
head -2 /proc/net/softnet_stat
```

```text
300
2000
0005484a 00000000 00000206 00000000 00000000 00000000 00000000 ...
00034fa0 00000000 00000001 00000000 00000000 00000000 00000000 ...
```

`net_rx_action()` 一轮最多处理 `netdev_budget` 个包、最多花 `netdev_budget_usecs` 微秒，哪个先到就跳出循环；没跑完的实例留在待处理链上，软中断结束时重新触发一次。所以"让出 CPU"不止发生在 poll 返回 `budget` 时，也发生在这一层。

`/proc/net/softnet_stat` 每行一个 CPU，数字是十六进制。第一列是处理过的包数，第二列是超预算丢弃，第三列 `time_squeeze` 是"预算或时间用尽但还有活没干完"的次数。换算过来，CPU0 的 `time_squeeze` 是 `0x206`（518），CPU1 是 `0x1`：CPU0 承担了绝大部分收包，挤压次数也是 518 对 1。

再往下还有一层：软中断积压到一定程度，`NET_RX` 会转给 `ksoftirqd` 线程执行，那时它和普通线程一样参与调度。`ps -eLo comm | grep -c ksoftirqd` 在这台 2 vCPU 的机器上是 2，每个 CPU 一个。

三层额度叠在一起：每次 poll 的 `budget`（按实例）、每轮软中断的 300 包 / 2 ms（按 CPU）、以及 `ksoftirqd` 的调度权重。只记住其中一层，就会把"为什么让出"归到一个数字上。

## 三、低负载下的一次接收

### 3.1 三个计数各算什么

先取三条基线：

```bash
grep -E 'NET_RX' /proc/softirqs
grep enp2s0-rx-0 /proc/interrupts
ip -s link show enp2s0 | sed -n '/RX:/,+1p'
```

然后向默认网关发 15 个间隔 0.03 秒的 ping：

```bash
ping -n -I enp2s0 -c 15 -i 0.03 192.168.126.2
```

ping 返回后，把上面三条原样再跑一遍。计数器只增不减，两轮相减才是这一轮的增量：

| 计数 | 前 | 后 | 差值 | 算的是什么 |
| --- | ---: | ---: | ---: | --- |
| CPU0 `NET_RX` | 401066 | 401083 | +17 | 这台 CPU 上 `NET_RX` 软中断被执行了多少次 |
| `enp2s0-rx-0` | 47296 | 47312 | +16 | 这个中断向量触发了多少次 |
| 设备 `RX packets` | 22152 | 22167 | +15 | 设备收了多少帧 |

三个数分别取自：`/proc/softirqs` 里 `NET_RX:` 那一行的第一列（CPU0）；`/proc/interrupts` 里 `enp2s0-rx-0:` 那一行的第一列；`ip -s link show` 输出里 `RX:` 段的 `packets` 列。

帧数是干净的：15 个请求、15 个应答，设备 `RX packets` 正好 +15。中断多一次，软中断又比中断多一次。连着跑四轮，这个差距还会变：

| 轮次 | `NET_RX` | `enp2s0-rx-0` | `RX packets` |
| --- | ---: | ---: | ---: |
| 1 | +17 | +16 | +15 |
| 2 | +17 | +16 | +15 |
| 3 | +15 | +15 | +15 |
| 4 | +17 | +16 | +15 |

帧数每轮都稳在 15，中断在 15 和 16 之间，软中断在 15 和 17 之间。多出来的那几次并非每条都能对上账：一个来源是别的网卡——跑实验的这条 SSH 会话在 `enp10s0` 上就有 1 次中断（`grep enp10s0 /proc/interrupts` 能看前后差值），它同样计入 `NET_RX`；同一向量上也可能有不是收包的事件。所以这三个数只能看量级和趋势，不能拿等式去验算。

要对账，用抓包裁决。另开一个终端：

```bash
sudo timeout 8 tcpdump -nn -i enp2s0 -c 30 icmp
```

`-c 30` 收满 30 个就自己退出，正好是 15 个请求加 15 个应答；ping 结束后它的最后一行是 `30 packets captured`，入向 15 个，和 `RX packets` 的 +15 严丝合缝。计数器的差值和抓到的帧数一致，说明"帧"这一层没漏也没多；不一致的是中断和软中断，那属于通知层面的统计。

`NET_RX` 的计数是全局的，不分设备：这个 SSH 会话本身在 `enp10s0` 上产生的流量也算在里面。它记录的是"这个软中断被执行了几次"，不是"收了多少帧"。

所以三个数可以相等，但那只是巧合，不是恒等式。背景流量、批处理、中断合并都能让它们分开；反过来用它们估算包数，只在负载足够低、设备足够单一时才成立。

顺带试了"把中断合并调大，能不能让中断少于帧"。这块网卡只认 `rx-usecs` 一档：

```bash
sudo ethtool -c enp2s0             # rx-usecs: 3
sudo ethtool -C enp2s0 rx-usecs 10000   # 10 ms，实测能写进去的最大值
```

100 个间隔 5 ms 的 ping，三档设置的对照：

| `rx-usecs` | 收帧 | 中断 |
| --- | ---: | ---: |
| 3（默认） | 100 | 100 |
| 1000 | 100 | 101 |
| 10000 | 100 | 100 |

参数回读得到 10000，中断次数却一动不动。也就是说这台虚拟网卡的 ITR 并没有真正参与合并，"调大就该少中断"在这里不成立。所以看到中断数明显少于帧数，再去考虑合并；差一两个数的时候，别拿合并当解释。这组对照换到物理网卡上才更有意义。[完整记录](/p/linux-network-driver-napi/napi-coalesce.txt)

### 3.2 poll 的工作量与预算

在专属实例里跟踪 poll。整块一次贴进去，不用先 `sudo -i` 另开 shell：

```bash
sudo bash -c '
rmdir /sys/kernel/tracing/instances/nslab 2>/dev/null
mkdir /sys/kernel/tracing/instances/nslab
cd /sys/kernel/tracing/instances/nslab
echo 1 > events/napi/napi_poll/enable
ping -n -I enp2s0 -c 15 -i 0.03 192.168.126.2
sleep 0.5
echo 0 > events/napi/napi_poll/enable
grep -c "device enp2s0" trace
grep "device enp2s0" trace | head -n 1
grep "device enp2s0" trace | grep -o "work [0-9]*" | sort | uniq -c
'
```

三条 `grep` 分别给条数、第一条原文和工作量分布。本机这一轮的输出：

```text
16
          <idle>-0       [000] ..s1. 80455.529207: napi_poll: napi poll on napi struct 0000000052ea44aa for device enp2s0 work 1 budget 64
      1 work 0
     15 work 1
```

16 条里 15 条 `work 1`，1 条 `work 0`。`work` 是这次回调的返回值，也就是处理了几个工作单元；`budget 64` 是允许的上限，绝不等于"这次收了 64 个包"。

条数比 ping 的 15 多一条是常态：多出来的可能是收尾那次空转（`work 0`），也可能是别的流量捎带的一个帧。所以别把"条数等于 ping 数"当规律，要看的是**分布**——绝大多数是 `work 1`，没有一条触到 `budget`，说明低负载下每次只取一个就收尾，批处理的优势用不上。

相邻两条 `enp2s0` 记录的时间差约 31 ms（这一轮从 80455.529 到 80455.561），和 ping 的 0.03 秒对得上；最后那条 `work 0` 落在 80456.424，比前一条晚了近半秒，是流量停了之后的收尾。

这轮的上下文都是 `<idle>-0`，即软中断在空闲任务里执行；持续流量下同样的回调可能落在别的上下文，这与 NAPI 的调度方式有关，不是固定的。跟踪里还能看到一条 `enp10s0` 的记录，那是 SSH 自己产生的流量——实例记录的是整机 NAPI，不是某块网卡独占。

[完整记录](/p/linux-network-driver-napi/napi-trace.txt)、[前后计数](/p/linux-network-driver-napi/napi-counters.txt) 可以直接对照。

清理：

```bash
sudo bash -c 'cd /; rmdir /sys/kernel/tracing/instances/nslab'
```

## 四、压力判断的四个落点

低负载这一轮的作用是确认路径和观察方法。加大负载后，比较 poll 返回量、软中断次数、中断频率和驱动统计，才有意义。

这一篇的实测只到 15 个 ping 的量级：poll 每次都只处理 1 个包，既碰不到 64 的实例预算，也碰不到每轮 300 包的软中断额度。下表是负载上去以后的查法，本篇不给这些数字，真压出 `work 64` 和 `time_squeeze` 增长需要另一套实验。

| 现象 | 往下查什么 |
| --- | --- |
| poll 多次返回 `budget` | 实例一直有活，结合 CPU、队列和丢弃看是否跟得上 |
| 接收中断变少而吞吐正常 | 是否发生了更有效的批处理或中断合并 |
| 驱动 `rx_no_buffer_count`、`rx_missed_errors` 增长 | 接收缓冲供应与驱动处理压力 |
| socket 丢弃增长而设备计数正常 | 协议栈或应用消费速度、内存限制 |

驱动统计的字段名由驱动定义，见[本机输出](/p/linux-network-driver-napi/napi-driver-stats.txt)，换一块网卡就未必还有这些名字。也不能因为这一轮 `dropped` 全是 0，就说路径上任何位置都不会丢。

NAPI 通常在软中断上下文跑，但也支持 busy polling 和线程化处理，所以它不等于 `NET_RX_SOFTIRQ` 本身。驱动接口的一般规则（Rx 预算、Tx 完成、budget 为 0 的特殊约束）见 [NAPI 文档](https://docs.kernel.org/7.0/networking/napi.html)。

## 五、命令与接口速查

| 命令 / 接口 | 用途 |
| --- | --- |
| `ethtool -i DEV` | 驱动身份与固件版本 |
| `ethtool -S DEV` | 驱动定义的统计字段 |
| `ls /sys/class/net/DEV/queues/` | 队列与中断向量的命名 |
| `/proc/interrupts` | 按 CPU 看中断累计次数 |
| `/proc/softirqs` | 按 CPU 看软中断累计次数 |
| `/proc/net/softnet_stat` | 按 CPU 看处理量、`dropped`、`time_squeeze` |
| `/proc/sys/net/core/netdev_budget`、`netdev_budget_usecs` | 一轮软中断最多处理多少包、最多跑多久 |
| `ethtool -c DEV` / `ethtool -C DEV rx-usecs N` | 查看、设置中断合并参数 |
| `ps -eLo comm` 配 `grep -c ksoftirqd` | 线程化软中断的实例数，通常每 CPU 一个 |
| `napi_schedule_prep()` / `__napi_schedule()` | 判断并申请一次 NAPI 处理 |
| `napi_complete_done()` | 结束本轮，按结果恢复通知 |
| `napi:napi_poll` | 观察实例、设备、返回工作量与预算 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| poll | 一次主动检查并处理已有工作的回调 |
| 接收环 | 驱动与设备用描述符组织的接收缓冲区结构 |
| 中断合并 | 设备侧按策略合并通知，改变通知频率与延迟 |
| 处理预算 | 限制一次接收工作量，避免单个实例长期占住处理机会 |
| `time_squeeze` | 预算或时间用尽、但还有活没干完的次数 |
| `ksoftirqd` | 软中断积压时接手执行的线程，每个 CPU 一个 |
| MSI-X | 允许设备用多个独立中断向量的机制，队列多时常用 |
