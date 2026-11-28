---
title: "Linux 网络栈（五）：IP 路由、下一跳与邻居解析"
slug: linux-network-route-neighbor
date: "2026-11-22T16:05:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "默认网关配好了，为什么还会直接去找目的 IP 的 MAC；路由查得到，为什么包还是发不出去。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

配了默认网关，为什么有些包仍然直接去找目的 IP 的 MAC？路由明明查得到，为什么还是发不出去？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · **5** · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

第五篇展开第一篇块 D 的前半：包从设备上来，内核先判断本地收还是转发，再选出口、选下一跳、解析邻居，最后交给块 C 发出去。这两步——"交给谁"和"给谁要 MAC"——是这一篇的两条线，也只讲 IPv4。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。

## 一、一台路由器，两段链路

用第一篇的[拓扑脚本](/p/linux-network-stack-map/lab.sh)，先清掉上一轮，再建路由拓扑：

```bash
sudo bash lab.sh clean
sudo bash lab.sh router
```

![A 经路由器 R 的两个接口到达 B，分别解析每段链路的邻居](/p/linux-network-route-neighbor/route.svg)

| 端点 | 接口与地址 | 到对端的路 |
| --- | --- | --- |
| A | `a0`：`10.77.1.1/24` | 默认下一跳 `10.77.1.254` |
| R | `ra`：`10.77.1.254/24`，`rb`：`10.77.2.254/24` | 两个直连网段，开着 IPv4 转发 |
| B | `b0`：`10.77.2.2/24` | 默认下一跳 `10.77.2.254` |

A 和 B 不在同一前缀，谁都不能把对方当成同链路的邻居。R 的转发开关只在 `nslab-r` 里生效，A 和 B 都不需要它。

## 二、路由先回答"交给谁"

### 2.1 一次 route get 的答案

```bash
sudo ip -n nslab-a route get 10.77.2.2
sudo ip -n nslab-r route get 10.77.2.2
```

```text
10.77.2.2 via 10.77.1.254 dev a0 src 10.77.1.1 uid 0
    cache
10.77.2.2 dev rb src 10.77.2.254 uid 0
    cache
```

A 的结果是从 `a0` 发，源地址选 `10.77.1.1`，下一跳是 R。R 对目的地有直连路由，出口就是 `rb`，不再经别的网关。

第二条是 R 自己发流量时的查询，源地址自然是 `10.77.2.254`。这不表示 R 转发 A 的包时会把源地址换成它——普通转发保留源、目的 IP 不动，改地址需要另外配 NAT。

### 2.2 规则在前，表在后

查路由不是"拿一张表从长到短比一遍"，而是先按规则挑表：

```bash
sudo ip -n nslab-a rule
sudo ip -n nslab-r rule
```

```text
0:	from all lookup local
32766:	from all lookup main
32767:	from all lookup default
```

A 和 R 都是这三条，前缀数字小的先查。`local` 装本机地址和广播地址，`main` 是 `ip route add/del` 默认动的那张表，`default` 空着。先看 `local`：

```bash
sudo ip -n nslab-a route show table local
```

```text
local 10.77.1.1 dev a0 proto kernel scope host src 10.77.1.1
broadcast 10.77.1.255 dev a0 proto kernel scope link src 10.77.1.1
local 127.0.0.0/8 dev lo proto kernel scope host src 127.0.0.1
local 127.0.0.1 dev lo proto kernel scope host src 127.0.0.1
broadcast 127.255.255.255 dev lo proto kernel scope link src 127.0.0.1
```

再看 `main`：

```bash
sudo ip -n nslab-a route show table main
```

```text
default via 10.77.1.254 dev a0
10.77.1.0/24 dev a0 proto kernel scope link src 10.77.1.1
```

目的地址落在 `local` 表里就是本机的，这决定了收上来的包走本地交付而不是转发，第五节的接收侧分支在这一层已经有答案。规则还能按源地址、防火墙标记等条件挑别的表，命中后是继续还是终止，看规则的动作。

选定表之后才轮到最长前缀匹配：`/32` 比 `/24` 具体，`/0` 最宽。规则优先级、同前缀候选的属性、下一跳是否可达，都是另外的维度，"数字小的赢"概括不了路由选择。

FIB 组织的是转发查找要用的信息，IPv4 的结果再包成路由结果对象，供输出和输入两条路径使用。`dst_entry` 是通用的目的缓存与操作基础，IPv4 的 `rtable` 把它包在里面；邻居表和 qdisc 并不都塞在这一个对象里。核查入口是 [route.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/route.c) 和 [dst.h](https://github.com/torvalds/linux/blob/v7.0/include/net/dst.h)。

## 三、下一跳还需要链路层地址

### 3.1 A 找的是网关的 MAC

```bash
sudo ip netns exec nslab-a ping -n -c 2 10.77.2.2
sudo ip -n nslab-a neigh
sudo ip -n nslab-r neigh
```

A 的邻居表只有一行：

```text
10.77.1.254 dev a0 lladdr 76:dd:b1:88:8d:af REACHABLE
```

R 那边有两行：

```text
10.77.2.2 dev rb lladdr ea:20:db:7a:09:5f REACHABLE
10.77.1.1 dev ra lladdr aa:0c:53:73:c3:89 DELAY
```

R 同时记着朝 A 和朝 B 的两个邻居，这就是"两段链路各自解析"：A 只有一个邻居，因为它只往 R 走，R 才是它的下一跳。

MAC 是当前链路的端点标识。A 的 IP 包目的地仍然是 B，但这一段的以太帧目的 MAC 是 R 的 `ra`；R 收下后换 `rb`，再解析 B 的 MAC，造出下一段链路的帧。veth 的 MAC 由系统分配，重建拓扑就会变，实际值记在[实验记录](/p/linux-network-route-neighbor/router.txt)里。

### 3.2 一次 ARP 请求

终端一抓 ARP，终端二清邻居再 ping：

```bash
sudo ip netns exec nslab-a tcpdump -nn -e -i a0 -c 2 arp
```

`-c 2` 收满一问一答两帧就自己退出，不用切回终端一按 Ctrl-C，也不用和窗口抢手速；`arp` 过滤掉 IPv6 邻居发现之类的无关帧。等抓包停在待命状态，再去终端二：

```bash
sudo ip -n nslab-a neigh flush dev a0
sudo ip netns exec nslab-a ping -n -c 1 10.77.2.2
```

两帧到手，tcpdump 末尾会自己打一行 `2 packets captured`（帧里的 MAC 与 3.1 同一轮拓扑，重建后会换）：

```text
14:49:32.047170 aa:0c:53:73:c3:89 > ff:ff:ff:ff:ff:ff, ethertype ARP (0x0806), length 42: Request who-has 10.77.1.254 tell 10.77.1.1, length 28
14:49:32.047186 76:dd:b1:88:8d:af > aa:0c:53:73:c3:89, ethertype ARP (0x0806), length 42: Reply 10.77.1.254 is-at 76:dd:b1:88:8d:af, length 28

2 packets captured
```

问的是 `who-has 10.77.1.254`，不是 `10.77.2.2`。这一句就把"为什么找网关的 MAC"钉死了。

邻居状态不止有和没有两种：`INCOMPLETE` 是解析还没完成，`REACHABLE` 是最近确认过可达，`DELAY` 是此前可达、稍后要再确认，`STALE` 是用到之前要重新确认。R 那边 `10.77.1.1` 正处在 `DELAY`，说明它并不每次发包都重新广播 ARP。

## 四、一条 /32 怎样盖住默认路径

加一条声称 B 就在 `a0` 链路上的主机路由：

```bash
sudo ip -n nslab-a route add 10.77.2.2/32 dev a0
sudo ip -n nslab-a route get 10.77.2.2
sudo ip netns exec nslab-a ping -n -c 1 -W 1 10.77.2.2
sudo ip -n nslab-a neigh
```

```text
10.77.2.2 dev a0 src 10.77.1.1 uid 0
    cache
```

结果里 `via` 没了：`/32` 比默认路由具体，A 认定 B 就在本链路。ping 一个回复也没有：

```text
1 packets transmitted, 0 received, 100% packet loss, time 0ms
```

紧接着看邻居表：

```text
10.77.2.2 dev a0 INCOMPLETE
10.77.1.254 dev a0 lladdr 76:dd:b1:88:8d:af REACHABLE
```

网关那行还是 `REACHABLE`，MAC 也没变。变的是路由结果本身——不再带 `via`，A 开始为 B 本身建邻居项，而 B 根本不在这段链路上，ARP 没人应答，条目停在 `INCOMPLETE`。

默认网关一直在，也一直可达，只是这一次没被选中。这就是"路由查到了"和"能发出去"的差别：前者由 FIB 回答，后者还要目的链路层地址能解析出来。

删掉错误路由：

```bash
sudo ip -n nslab-a route del 10.77.2.2/32 dev a0
```

拓扑先留着，下一节还要用它。全部做完再统一清理。

## 五、接收侧的本地交付与转发

发送方向的条件（源地址、出接口）由应用和路由共同决定；接收方向同样先查路由：普通 IPv4 接收从 `ip_rcv()` 进路由判断，目的地址落在 `local` 表就走 `ip_local_deliver()`，否则进转发，还要过转发开关、TTL、MTU、过滤这些条件。

转发开关在 R 自己的命名空间里，A 和 B 上都是关的：

```bash
sudo ip netns exec nslab-r sysctl net.ipv4.ip_forward
sudo ip netns exec nslab-a sysctl net.ipv4.ip_forward
sudo ip netns exec nslab-b sysctl net.ipv4.ip_forward
```

```text
net.ipv4.ip_forward = 1
net.ipv4.ip_forward = 0
net.ipv4.ip_forward = 0
```

包究竟是被 R 转发的，还是绕别处送到的，`/proc/net/snmp` 的 `Ip:` 行给了直接答案。这一行里 `Forwarding` 是第一个数值、`InReceives` 第三个、`ForwDatagrams` 第六个；`awk` 把行名算作 `$1`，所以后两者落在 `$4`、`$7`：

```bash
sudo ip netns exec nslab-r awk '/^Ip:/{getline; print "InReceives="$4, "ForwDatagrams="$7}' /proc/net/snmp
sudo ip netns exec nslab-a ping -n -c 3 10.77.2.2
sudo ip netns exec nslab-r awk '/^Ip:/{getline; print "InReceives="$4, "ForwDatagrams="$7}' /proc/net/snmp
sudo ip netns exec nslab-a awk '/^Ip:/{getline; print "InReceives="$4, "ForwDatagrams="$7}' /proc/net/snmp
```

```text
InReceives=4 ForwDatagrams=4
64 bytes from 10.77.2.2: icmp_seq=1 ttl=63 time=0.056 ms
64 bytes from 10.77.2.2: icmp_seq=2 ttl=63 time=0.066 ms
64 bytes from 10.77.2.2: icmp_seq=3 ttl=63 time=0.069 ms
3 packets transmitted, 3 received, 0% packet loss, time 2048ms
InReceives=10 ForwDatagrams=10
InReceives=5 ForwDatagrams=0
```

三来三回，R 上 `InReceives` 和 `ForwDatagrams` 各涨 6，A 上 `ForwDatagrams` 从头到尾是 0。包经 R 转发这件事，到这里不是推断。计数是累计值，起头那两个数随这套命名空间之前跑过多少流量而变，要看的是前后差值。

`ttl=63` 是另一条独立线索：A 的请求以默认 TTL 64 发出，过 R 掉一；B 的回复以自己的 64 起跳，回程再过 R 又掉一，于是 A 看到 63。它只说明回程路上有一跳，不指认是哪一台，转发计数补的正是这一格。

收尾：

```bash
sudo bash lab.sh clean
```

IPv6 用邻居发现而不是 ARP，对象和实现都不一样，这里的 IPv4 函数名不能直接套过去。源码分工见 [ip_input.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/ip_input.c) 和 [fib_rules.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/fib_rules.c)。

## 六、命令与接口速查

| 命令 | 用途 |
| --- | --- |
| `ip route get DEST` | 按给定条件查路由结果，附加条件要显式给 |
| `ip rule` / `ip route show table NAME` | 看策略规则与指定表 |
| `ip neigh` / `ip neigh flush dev DEV` | 看邻居与状态，清掉以复现解析 |
| `tcpdump -nn -e -i DEV -c N arp` | 同时看链路层头和 ARP，收满 N 帧自停 |
| `sysctl net.ipv4.ip_forward` | 看某个命名空间的转发开关 |
| `/proc/net/snmp` 的 `Ip:` 行 | `InReceives`、`ForwDatagrams` 等转发计数 |
| `ping` 回复里的 `ttl` | 跳数线索，`64` 减去实际跳数 |

## 七、名词解释

| 名词 | 含义 |
| --- | --- |
| 下一跳 | 当前链路上负责继续送达的端点，直连时就是目的端本身 |
| 最长前缀匹配 | 在多条匹配路由中优先选前缀更具体的 |
| 邻居项 | 某个设备上"网络层地址 → 链路层地址"的可达状态 |
| 转发 | 内核为非本机的目的地选路并再次发送 |
| 转发开关 | `net.ipv4.ip_forward`，非 0 时内核才做转发 |
| `INCOMPLETE` | 邻居项已建、ARP 还没有应答 |
