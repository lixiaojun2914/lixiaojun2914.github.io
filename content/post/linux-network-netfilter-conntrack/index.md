---
title: "Linux 网络栈（十）：Netfilter、连接跟踪与 NAT"
slug: linux-network-netfilter-conntrack
date: "2026-11-22T16:10:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "路由通过、规则丢弃、NAT 改地址：三件事分别由谁决定，以及本机发送、接收与转发三条路径最后怎样收拢。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

一个包路由查得到，却在转发途中被扔掉；一条连接过了源地址转换，对端看到的地址又变了。路由、规则、连接记录各管什么？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · **10**

第一篇 1.3 那张表只标了五个 hook 的位置，说"过滤插在这里"。这一篇把规则怎么命中、连接记录怎么建立、地址怎么改写讲完，最后把本机发送、本机接收、转发三条路径收拢成一张对照表，作为整个系列的结尾。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。

## 一、五个 hook 的位置

![IPv4 本地发送、接收和转发分别经过不同 Netfilter hook](/p/linux-network-netfilter-conntrack/hooks.svg)

| hook | 在哪里 |
| --- | --- |
| prerouting | 收上来之后，输入路由判断之前 |
| input | 交给本机上层协议之前 |
| forward | 判定要转发之后，输出之前 |
| output | 本机产生的流量，出路由之前 |
| postrouting | 输出后段，本机发送和转发都经过 |

Netfilter 只提供挂点；nftables 用表、链、规则把条件和动作接上去。base chain 靠类型、hook、priority 参与，普通链靠 jump 进去。

同一个 hook 上可以有多个处理者，priority 数值小的先跑。某条链里 accept 不代表后面所有 hook 都免检，drop 才终止这个包的正常交付。一条规则的结果不能当全路径通行证。

## 二、在 forward 上明确丢弃

### 2.1 规则只放进实验路由器

```bash
sudo bash lab.sh router
```

把下面这段存成 `rules.nft`：

```nft
table inet demo {
    chain pre {
        type filter hook prerouting priority -301; policy accept;
        ip saddr 10.77.1.1 meta nftrace set 1;
    }
    chain forward {
        type filter hook forward priority 0; policy accept;
        udp dport 18082 counter drop;
    }
}
```

```bash
sudo ip netns exec nslab-r nft -f rules.nft
sudo ip netns exec nslab-r nft monitor trace
```

最后一条留在终端一。终端二起 B 的 UDP 服务，终端三从 A 发两个数据报：

```bash
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.2.2 18082
```

```bash
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.2.2 18082
sudo ip netns exec nslab-r nft list table inet demo
```

客户端会打印发送成功，B 那边什么也没有。这就是第二篇讲过的接口语义：UDP 的 `sendto` 返回只说明本机收下了。

顺带提醒一个显示差异：文件里写 `priority -301` 和 `priority 0`，`nft list` 会显示成 `priority raw - 1` 和 `priority filter`，是同一优先级的符号写法，不是规则被改了。

### 2.2 轨迹与计数

trace 里一个数据报走成这样：

| 位置 | 看到什么 |
| --- | --- |
| prerouting | `iif "ra"`，源 A、目的 B，TTL 64，`ip length 33` |
| pre 规则 | `meta nftrace set 1`，verdict continue |
| forward | `iif "ra" oif "rb"`，TTL 63，已经选定出口 |
| forward 规则 | `udp dport 18082 ... drop`，verdict drop |

TTL 从 64 变 63，说明它确实经过了转发，不是本机收下。

两条数据报的负载是 5 字节和 2 字节，IP 长度分别是 33 和 30。规则计数为 `packets 2 bytes 63`，正好是 33 加 30。计数字节是 IP 长度之和，既不是业务负载，也不是以太帧全长。

`nft monitor` 看的是对象变化，包路径要用 `nft monitor trace`，而且得先用 `meta nftrace` 把那部分流量圈出来。用法见 [nftables 跟踪文档](https://wiki.nftables.org/wiki-nftables/index.php/Ruleset_debug/tracing)。

收掉规则：

```bash
sudo ip netns exec nslab-r nft delete table inet demo
```

## 三、conntrack 记的是流，不是 socket

conntrack 用协议、地址、端口这些元组识别双向流，维护状态、超时和扩展信息。路由器上没有为这条业务建过任何 socket，照样能记一条。

| 对象 | 管什么 | 怎么看 |
| --- | --- | --- |
| 本机 TCP socket | 端到端的字节流、序号、重传、应用队列 | `ss -tinm` |
| conntrack 条目 | 经过这台机器的流的双向关联、状态、超时 | `conntrack -L` |
| nft 规则 | 按条件匹配、执行策略 | `nft list`、trace、counter |

`ct state established` 不是"本机有个 TCP socket 处于 ESTABLISHED"，UDP 一样会被跟踪。要不要放行 established/related 是策略问题，不是必须照抄的模板。

元组查找和状态组织见 [nf_conntrack_core.c](https://github.com/torvalds/linux/blob/v7.0/net/netfilter/nf_conntrack_core.c)。规则系统会读跟踪结果来匹配，但它们是两个对象。

## 四、SNAT 让两侧看到不同的源地址

### 4.1 只转 A 到 B 的流量

确认上一轮的过滤表已删，再建转换：

```bash
sudo ip netns exec nslab-r nft add table ip natdemo
sudo ip netns exec nslab-r nft 'add chain ip natdemo post { type nat hook postrouting priority srcnat; policy accept; }'
sudo ip netns exec nslab-r nft add rule ip natdemo post ip saddr 10.77.1.0/24 oifname rb snat to 10.77.2.254
```

在 `rb` 出口把来自 `10.77.1.0/24` 的源地址换成 R 的 `10.77.2.254`。

终端一跑 B：

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.2.2 18084 1500
```

终端二跑 A：

```bash
sudo ip netns exec nslab-a ./netdemo tcp-fill 10.77.2.2 18084
```

传输进行中查两处：

```bash
sudo ip netns exec nslab-r conntrack -L -p tcp --dport 18084
sudo ip netns exec nslab-b ss -tn '( sport = :18084 )'
```

```text
tcp 6 431998 ESTABLISHED src=10.77.1.1 dst=10.77.2.2 sport=45466 dport=18084
    src=10.77.2.2 dst=10.77.2.254 sport=18084 dport=45466 [ASSURED] mark=0 use=1
```

```text
State Recv-Q Send-Q Local Address:Port  Peer Address:Port
ESTAB 120560 0      10.77.2.2:18084     10.77.2.254:45466
```

B 看到的对端是 `10.77.2.254:45466`，不是 A 的 `10.77.1.1`。临时端口和剩余超时每次都不一样。

### 4.2 两组元组

![SNAT 前后的正向地址以及回复方向元组](/p/linux-network-netfilter-conntrack/nat.svg)

| 位置 / 方向 | 源 | 目的 |
| --- | --- | --- |
| 转换前，A 发出 | `10.77.1.1:45466` | `10.77.2.2:18084` |
| B 看到的连接 | `10.77.2.254:45466` | `10.77.2.2:18084` |
| 条目里的回复方向 | `10.77.2.2:18084` | `10.77.2.254:45466` |

条目里这两组地址是原始方向和回复方向，不是两条连接，也不是一个包里出现过的所有地址。有状态 NAT 在首包上建立关联，后续包复用状态并改写，回复方向按同一份状态还原。

所以 NAT 是"规则选谁、状态记什么、改写在哪做"三件事的配合，不能简化成"conntrack 自己决定过滤并执行 NAT"。源码见 [nf_nat_core.c](https://github.com/torvalds/linux/blob/v7.0/net/netfilter/nf_nat_core.c)。

这个拓扑本来就有完整回程路由，不配 NAT 也能通。NAT 在这里只是让地址可见性变化，不是互通的必要条件。跟踪状态要占资源、有超时，地址转换也不等于身份认证或安全边界。

## 五、回到三条路径

| 场景 | 主线 | 能看的证据 |
| --- | --- | --- |
| 本机发送 | fd/socket → TCP/UDP → IP 输出与路由 → output/postrouting → 邻居、qdisc、设备 | strace、ss、`ip route get`、nft trace、tc、抓包 |
| 本机接收 | 设备与 NAPI → prerouting 与路由判断 → input → TCP/UDP 查找 → socket 队列与唤醒 | 驱动与 NAPI 计数、抓包、nft、ss |
| 转发 | 接收入口 → prerouting → 路由与转发检查 → forward → postrouting → 邻居与设备输出 | 两侧抓包、路由与邻居、nft、设备统计 |

这张表对应职责，不承诺每条路径都按同一串函数执行。Netfilter 改了路由相关字段还可能触发重新判定，bridge 和隧道也各有边界。

排查时沿交接点问就行：调用接受了多少字节？对端协议栈收到没有？路由选中了谁？邻居解析出来了没有？队列有没有积压？在哪个 hook 得到了什么 verdict？每个问题都要有对应的证据，别从一句"ping 不通"直接跳到某个内核函数。

清理：

```bash
sudo ip netns exec nslab-r nft delete table ip natdemo
sudo bash lab.sh clean
```

## 六、命令与接口速查

| 命令 / 表达式 | 用途 |
| --- | --- |
| `meta nftrace set 1` + `nft monitor trace` | 圈定流量并接收规则轨迹 |
| `counter` | 累计匹配的包数与字节数 |
| `conntrack -L` | 列出现有跟踪条目 |
| `snat to ADDR` | 为匹配的连接建立源地址转换 |
| `nft delete table FAMILY NAME` | 只删指定的实验表 |

## 七、名词解释

| 名词 | 含义 |
| --- | --- |
| hook | 内核路径上可接入处理的位置 |
| verdict | 对当前包作出的继续、接受、丢弃等判定 |
| 元组 | 用协议、地址、端口识别一个方向流量的组合 |
| 有状态 NAT | 维护双向转换关联、供后续流量复用的地址转换 |
| `[ASSURED]` | 双向都已确认的条目，内存紧张时不会被优先淘汰 |
