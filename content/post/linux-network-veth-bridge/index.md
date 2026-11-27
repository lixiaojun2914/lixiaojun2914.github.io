---
title: "Linux 网络栈（九）：veth、bridge 与二层转发"
slug: linux-network-veth-bridge
date: "2026-11-22T16:09:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "一对 veth 把两个环境接起来，bridge 再把三个端口组织成广播域；学习靠源 MAC，转发靠 FDB。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

网络命名空间把状态分开了，两边却不会自动出现一条线。这条线是怎么接上的，接上之后帧又按什么规则走？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · **9** · [10](/p/linux-network-netfilter-conntrack/)

第八篇把环境分开了，这一篇给出通路。前八篇动手时用的 `a0`/`b0`、`pa`/`pb`/`pc` 本来就是 veth 和 bridge，这一篇把它们讲明白；第十篇的 NAT 也要建在这套二层拓扑上。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。

## 一、一对 veth 就是一条链路

### 1.1 创建

第一篇的 `lab.sh direct` 里是这条命令：

```bash
sudo ip link add a0 netns nslab-a type veth peer name b0 netns nslab-b
```

前提是两个 netns 已经存在。它造的是一对设备，不是两块互不相干的网卡：`a0` 发出的帧从 `b0` 的接收方向进来，反过来也一样。两端分在不同环境时，帧随之进入对端的协议处理。

设备建好还要配地址、拉起链路。veth 不替应用决定 IP、路由和防火墙——它只负责把帧交给另一端。

### 1.2 交接发生在内核的哪一步

veth 是一个 `net_device`，发送回调是 `veth_xmit()`：找到 peer，检查对端状态和条件，再把 skb 推进对端的接收路径。

```c
/* drivers/net/veth.c */
static netdev_tx_t veth_xmit(struct sk_buff *skb, struct net_device *dev)
```

一部分配置会走 NAPI/XDP 那条分支，所以不能把任何版本、任何配置都画成一次固定的 `netif_rx()` 调用。`struct veth_priv` 存设备私有信息，包括 peer 关联和接收资源；它和 socket 队列是两回事，前者管虚拟设备的交接，后者属于协议端点。源码见 [veth.c](https://github.com/torvalds/linux/blob/v7.0/drivers/net/veth.c)。

## 二、三个端点接到同一个 bridge

### 2.1 br0 与三个端口

```bash
sudo bash lab.sh clean
sudo bash lab.sh bridge
```

A、B、C 的地址是 `10.77.1.1`、`.2`、`.3`，掩码 `/24`。每个端点有一对 veth 的一端，另一端 `pa`、`pb`、`pc` 放在 `nslab-r` 里，都加进 `br0`。

| 端点接口 | bridge 侧端口 | 端点 MAC |
| --- | --- | --- |
| A 的 `a0` | `pa` | `02:00:00:77:01:01` |
| B 的 `b0` | `pb` | `02:00:00:77:01:02` |
| C 的 `c0` | `pc` | `02:00:00:77:01:03` |

脚本给端点固定了 MAC，图、FDB、抓包才能对得上。`br0` 这次不配 IP，也不需要开 IPv4 转发——它只做二层交换，`nslab-r` 这个名字在这里不代表三层路由。

![三对 veth 将 A、B、C 接入同一 bridge，FDB 记录各 MAC 所在端口](/p/linux-network-veth-bridge/bridge.svg)

### 2.2 先有流量，再读 FDB

```bash
sudo ip netns exec nslab-a ping -n -c 1 10.77.1.2
sudo ip netns exec nslab-c ping -n -c 1 10.77.1.1
sudo ip netns exec nslab-r bridge fdb show br br0 | grep -E '02:00:00:77'
```

```text
02:00:00:77:01:01 dev pa master br0
02:00:00:77:01:02 dev pb master br0
02:00:00:77:01:03 dev pc master br0
```

FDB 是转发数据库，一条表项的意思是"到这个 MAC，从哪个端口出去"。它不是路由表，也不是 ARP 表。两次 ping 都有回应，TTL 是 64——二层转发不消耗 IP 跳数。

## 三、学习与转发

### 3.1 学习看入端口和源 MAC

`pa` 上收到源 MAC 是 A 的帧，bridge 就记下"A 在 `pa` 后面"。依据只有入端口和源 MAC，不需要问谁。动态表项会老化，端点换了位置，后续的源流量会把表项改过来。

| 目的情况 | 默认行为 | 原因 |
| --- | --- | --- |
| 已知单播 | 只发到 FDB 指出的端口 | 位置已知 |
| 未知单播 | 向允许的其他端口泛洪 | 还不知道在哪 |
| 广播 | 向允许的其他端口泛洪 | 目的就是整个广播域 |

这是默认配置、没有 VLAN 过滤的实验桥。端口状态、STP、VLAN、隔离、组播侦听都会进一步限制，不能把"bridge 见谁都发"当定律。

### 3.2 C 端能看到什么

在 C 上抓包，清掉 A 的邻居，再从 A ping B：

```bash
sudo ip netns exec nslab-c tcpdump -nn -e -i c0
```

```bash
sudo ip -n nslab-a neigh flush dev a0
sudo ip netns exec nslab-a ping -n -c 2 10.77.1.2
```

C 侧看到的是：

```text
14:51:30.511333 02:00:00:77:01:01 > ff:ff:ff:ff:ff:ff, ethertype ARP (0x0806), length 42: Request who-has 10.77.1.2 tell 10.77.1.1, length 28
```

一条 ARP 广播，`ff:ff:ff:ff:ff:ff` 是广播地址佐证。后面那几条 `33:33:00:00:00:02` 是 IPv6 路由请求，和本实验无关，属于环境噪音。

没有看到 ARP 应答，也没有看到后面的 ICMP 单播。原因就在 FDB：A 在 `pa`、B 在 `pb`，应答按单播只发到 `pb`，C 的 `pc` 上自然看不到。广播能看到、单播看不到，这就是学习的直接后果。

[完整记录](/p/linux-network-veth-bridge/bridge.txt) 里有两次 ping、FDB 和这段抓包。

## 四、bridge 收帧不走本机发送回调

从桥端口进来的帧走的是注册的接收处理：`br_handle_frame()` 判断，`br_handle_frame_finish()` 做学习和转发决策，再交给转发代码送出。它不需要先变成这台机器的 IP 数据，再从 `br0` 的发送回调绕一圈出去。

`br0` 配了地址就是另一回事了：那时它自己也是这个二层网络里的一个端点，发给它的帧和要转发的帧是两条不同的路。源码入口：[br_input.c](https://github.com/torvalds/linux/blob/v7.0/net/bridge/br_input.c)、[br_forward.c](https://github.com/torvalds/linux/blob/v7.0/net/bridge/br_forward.c)。

## 五、其他虚拟设备的位置

| 类型 | 解决什么 | 和本例的区别 |
| --- | --- | --- |
| macvlan | 在下层设备上做独立 MAC 端点 | 依赖下层与模式，不只是换个 MAC |
| ipvlan | 在下层上按 IP 组织端点 | MAC 用法和分流方式与 macvlan 不同 |
| VXLAN | 用 IP/UDP 承载虚拟以太网 | 有内外层地址和隧道端点映射 |
| TUN/TAP | 用户态经设备接口收发三层包/二层帧 | 数据要经过用户态读写，不是纯内核交接 |

它们都表现为网络设备，但创建接口、接收路径、下层关系、生命周期都不一样，差异远不止 `ndo_start_xmit` 一个回调。`rtnl_link_ops` 组织部分链路类型的创建参数，也不能因此把 TUN/TAP 的创建画成同一套流程。

收尾：

```bash
sudo bash lab.sh clean
```

## 六、命令与接口速查

| 命令 | 用途 |
| --- | --- |
| `ip link add ... type veth peer ...` | 造一对虚拟链路 |
| `ip link set PORT master br0` | 把端口加入 bridge |
| `bridge fdb show br br0` | 看转发表 |
| `tcpdump -e -i DEV` | 带链路层地址，方便对上 FDB |

## 七、名词解释

| 名词 | 含义 |
| --- | --- |
| peer | veth 的配对端设备 |
| bridge | 按二层地址学习和转发帧的虚拟交换 |
| 泛洪 | 不选单一端口，而向多个允许的出口分发 |
| 广播域 | 二层广播能到达的范围 |
| FDB | 记录"MAC → 端口"的表，老化后重新学习 |
