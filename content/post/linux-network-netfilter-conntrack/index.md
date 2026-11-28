---
title: "Linux 网络栈（十）：Netfilter、四表五链、连接跟踪与 NAT"
slug: linux-network-netfilter-conntrack
date: "2026-11-22T16:10:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "四张表分别挂在五条链上，包到底走过哪几条；连接跟踪在什么条件下才开始记、条目怎么读、什么时候消失；NAT 又怎么借用这份状态。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

一个包路由查得到，却在转发途中被扔掉；一条连接过了源地址转换，对端看到的地址又变了。规则在哪一层生效，连接记录又是谁建的？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · **10**

第一篇 1.3 那张表只标了五个 hook 的位置，说"过滤插在这里"。这一篇把四张表分别挂在哪几条链上、包实际走了哪几条、连接跟踪什么时候才开始记、条目怎么读、地址怎么改写讲完，最后把本机发送、本机接收、转发三条路径收拢成一张对照表。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。这一篇用到 `iptables`、`conntrack`、`nft` 三个命令，都是同一套内核设施的入口。

## 一、五个 hook 与五条链

![IPv4 本地发送、接收和转发分别经过不同 Netfilter hook](/p/linux-network-netfilter-conntrack/hooks.svg)

| hook | 在哪里 |
| --- | --- |
| prerouting | 收上来之后，输入路由判断之前 |
| input | 交给本机上层协议之前 |
| forward | 判定要转发之后，输出之前 |
| output | 本机产生的流量，出路由之前 |
| postrouting | 输出后段，本机发送和转发都经过 |

五个 hook 对应五条链的名字：`PREROUTING`、`INPUT`、`FORWARD`、`OUTPUT`、`POSTROUTING`。表是规则的容器，不同的表只挂在其中几条链上：

| 表 | 挂在哪几条链 | 主要用途 |
| --- | --- | --- |
| raw | PREROUTING、OUTPUT | 在连接跟踪之前处理，可标记"不跟踪" |
| mangle | 五条都有 | 改 TTL、TOS、mark 等报文字段 |
| nat | PREROUTING、INPUT、OUTPUT、POSTROUTING | 改地址，只对连接的首包起作用 |
| filter | INPUT、FORWARD、OUTPUT | 放行或丢弃 |

同一个 hook 上可以有多个处理者，priority 数值小的先跑。某条链里 accept 不代表后面所有 hook 都免检，drop 才终止这个包的正常交付。一条规则的结果不能当全路径通行证。

## 二、用计数器看包走了哪几条链

### 2.1 五条链上各放一个

转到路由拓扑，在 mangle 表的五条链上各加一条只带计数的 accept 规则：

```bash
sudo bash lab.sh clean
sudo bash lab.sh router
sudo ip netns exec nslab-r iptables -t mangle -A PREROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A INPUT -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A FORWARD -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A OUTPUT -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A POSTROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t filter -A INPUT -j ACCEPT
sudo ip netns exec nslab-r iptables -t filter -A FORWARD -j ACCEPT
sudo ip netns exec nslab-r iptables -t filter -A OUTPUT -j ACCEPT
```

`iptables v1.8.11 (nf_tables)` 是走 nft 后端的版本，这些命令建出来的就是 nft 表：

```bash
sudo ip netns exec nslab-r nft list tables
```

```text
table ip mangle
table ip filter
```

```bash
sudo ip netns exec nslab-r nft list table ip filter
```

```text
table ip filter {
	chain FORWARD {
		type filter hook forward priority filter; policy accept;
		counter packets 0 bytes 0 accept
	}
}
```

iptables 和 nftables 是两套前端，落到内核是同一套表和链。`counter` 就是 `-A` 时隐式建的那个计数器。

三种流量分开跑，每轮前用 `-Z` 清零，然后读五条链的包数：

```bash
sudo ip netns exec nslab-r iptables -t mangle -Z
sudo ip netns exec nslab-a ping -n -c 3 10.77.2.2
sudo ip netns exec nslab-r iptables -t mangle -L -v -n
```

`-L -v -n` 的 `pkts` 列就是每条规则的匹配包数。三种流量下的读数：

| 链 | A→B 转发（6 包） | R 自己 ping B（3 + 3） | B→A 转发（6 包） |
| --- | --- | --- | --- |
| mangle PREROUTING | 6 | 3 | 6 |
| mangle INPUT | 0 | 3 | 0 |
| mangle FORWARD | 6 | 0 | 6 |
| mangle OUTPUT | 0 | 3 | 0 |
| mangle POSTROUTING | 6 | 3 | 6 |
| filter INPUT | 0 | 3 | 0 |
| filter FORWARD | 6 | 0 | 6 |
| filter OUTPUT | 0 | 3 | 0 |

表格里每个数字都来自对应表同一条链的 `pkts` 列，原始读数在[四表五链的记录](/p/linux-network-netfilter-conntrack/nf-tables.txt)。

读法：

- 转发的 ping 只碰 `PREROUTING`、`FORWARD`、`POSTROUTING`，`INPUT` 和 `OUTPUT` 是 0——它既不是交给本机，也不是本机产生。
- R 自己 ping B 时，发出的 3 个请求走 `OUTPUT`、`POSTROUTING`，B 回来的 3 个回包走 `PREROUTING`、`INPUT`，`FORWARD` 是 0。同一轮里两份计数分别属于两个方向。
- B ping A 是纯转发，但一轮里两个方向各 3 个包，所以三条链都是 6。
- filter 表没有 `PREROUTING` 和 `POSTROUTING` 两行，不是漏了，是这个表不挂在那两条链上。

### 2.2 四张表不是同一批包

再换一条 100 KB 的 TCP 连接，四张表各放一条计数规则，nat 表上放一条真转换：

```bash
sudo ip netns exec nslab-r iptables -t raw -A PREROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -F
sudo ip netns exec nslab-r iptables -t mangle -A PREROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A FORWARD -j ACCEPT
sudo ip netns exec nslab-r iptables -t mangle -A POSTROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t nat -A PREROUTING -j ACCEPT
sudo ip netns exec nslab-r iptables -t nat -A POSTROUTING -o rb -j MASQUERADE
sudo ip netns exec nslab-r iptables -t filter -F
sudo ip netns exec nslab-r iptables -t filter -A FORWARD -j ACCEPT
```

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.2.2 18084
sudo ip netns exec nslab-a ./netdemo tcp-client 10.77.2.2 18084 100000
```

服务端打印 `received=100000 bytes`。这时四张表的计数：

| 表 / 链 | pkts | bytes |
| --- | --- | --- |
| raw PREROUTING | 64 | 103K |
| mangle PREROUTING | 64 | 103K |
| mangle FORWARD | 64 | 103K |
| mangle POSTROUTING | 64 | 103K |
| nat PREROUTING | 1 | 60 |
| nat POSTROUTING（MASQUERADE） | 1 | 60 |
| filter FORWARD | 64 | 103K |

同样是这条连接的包：

- raw 和 mangle 的每一条链都数到全部 64 个包。raw 排在连接跟踪之前，mangle 排在各个 hook 的常规位置，两者都按包走。
- filter 的 `FORWARD` 也是 64，因为它就是逐包判定放行还是丢弃。
- nat 只有 1。这条连接要不要换地址，在第一个包上就定了；后面的包按已有的跟踪状态直接改写，不再进 nat 链。那 60 字节正是 SYN：20 字节 IP 头、20 字节 TCP 头、20 字节选项。

所以"四表五链"不是四张表都过一遍。表是按 hook 挂的，链是按包的实际路径走的，nat 还额外受连接状态约束。看到 `iptables -t nat -L -v` 的计数远小于 `filter`，是正常现象，不是规则没生效。

清理这几张表：

```bash
sudo ip netns exec nslab-r iptables -t raw -F
sudo ip netns exec nslab-r iptables -t mangle -F
sudo ip netns exec nslab-r iptables -t nat -F
sudo ip netns exec nslab-r iptables -t filter -F
```

## 三、在 forward 上明确丢弃

### 3.1 规则只放进实验路由器

```bash
sudo bash lab.sh clean
sudo bash lab.sh router
```

上一轮的拓扑没拆就再建，`lab.sh` 会报 `nslab namespace exists` 直接退出，所以要带上前一条 `clean`。

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

装载规则，然后按顺序开三个终端：**终端二**先起 B 的接收端，它会在 `recvfrom` 里一直等；**终端一**开跟踪，`timeout 30` 让它在 30 秒后自己退出；**终端三**再从 A 发两个数据报。

```bash
sudo ip netns exec nslab-r nft -f rules.nft
```

```bash
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.2.2 18082
```

```bash
sudo ip netns exec nslab-r timeout 30 nft monitor trace
```

```bash
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.2.2 18082
sudo ip netns exec nslab-r nft list table inet demo
```

顺序不能反：跟踪窗口必须开在流量之前，否则这两个包根本不会出现在 trace 里；接收端先起着，B 那边才有一个真正准备好的 UDP socket，不至于让丢弃原因掺进别的分支。`timeout 30` 是为了让终端一自己收尾——`nft monitor trace` 本身不会退出，手按 Ctrl-C 也能停。发完两个数据报、等终端一退出，再在终端三读计数。

客户端的输出是：

```text
sent datagrams: 6 bytes, 2 bytes
```

B 那边什么也没有，只有 `ready UDP 10.77.2.2:18082`。这就是第二篇讲过的接口语义：UDP 的 `sendto` 返回只说明本机收下了。

顺带提醒一个显示差异：文件里写 `priority -301` 和 `priority 0`，`nft list` 会显示成 `priority raw - 1` 和 `priority filter`，是同一优先级的符号写法，不是规则被改了。

### 3.2 轨迹与计数

trace 里一个数据报走成这样：

| 位置 | 看到什么 |
| --- | --- |
| prerouting | `iif "ra"`，源 A、目的 B，TTL 64，`ip length 34`，`udp length 14` |
| pre 规则 | `meta nftrace set 1`，verdict continue |
| forward | `iif "ra" oif "rb"`，TTL 63，已经选定出口 |
| forward 规则 | `udp dport 18082 ... drop`，verdict drop |

TTL 从 64 变 63，说明它确实经过了转发，不是本机收下。

两条数据报的负载是 6 字节和 2 字节（`udp-client` 发的是 `ABCDEF` 和 `FG`），IP 长度分别是 34 和 30——20 字节 IP 头加 8 字节 UDP 头，再加负载。规则计数是 `packets 2 bytes 64`，正好 34 加 30。计数字节是 IP 长度之和，既不是业务负载，也不是以太帧全长。

表里这两个值分别来自 trace 行里的 `ip length` 和 `nft list table inet demo` 计数行里的 `bytes`，原始输出见[跟踪记录](/p/linux-network-netfilter-conntrack/nft-trace.txt)。

`nft monitor` 看的是对象变化，包路径要用 `nft monitor trace`，而且得先用 `meta nftrace` 把那部分流量圈出来。用法见 [nftables 跟踪文档](https://wiki.nftables.org/wiki-nftables/index.php/Ruleset_debug/tracing)。

收掉规则：

```bash
sudo ip netns exec nslab-r nft delete table inet demo
```

## 四、conntrack 记的是流，不是 socket

### 4.1 一行条目里有什么

路由器上没有为这条业务建过任何 socket，照样能记一条。一行条目长这样：

```text
tcp 6 431996 ESTABLISHED src=10.77.1.1 dst=10.77.2.2 sport=42864 dport=18084 src=10.77.2.2 dst=10.77.2.254 sport=18084 dport=42864 [ASSURED] mark=0 use=1
```

| 位置 | 含义 |
| --- | --- |
| `tcp` / `6` | 协议名与协议号，UDP 是 `udp 17`，ICMP 是 `icmp 1` |
| `431996` | 距这条条目到期还剩多少秒 |
| `ESTABLISHED` | 跟踪状态，不是 TCP 的 socket 状态 |
| 第一组 `src/dst/sport/dport` | 正向元组，最先看到的那个方向 |
| 第二组 | 回复方向元组，NAT 之后的值 |
| `[ASSURED]` | 两个方向都确认过，内存紧张时不会被优先淘汰 |
| `mark=0` `use=1` | 连接标记与引用计数 |

`431996` 不是随便一个数，它等于 `nf_conntrack_tcp_timeout_established` 减掉已存活的时间：

```bash
sudo ip netns exec nslab-r sysctl net.netfilter.nf_conntrack_tcp_timeout_established
```

```text
net.netfilter.nf_conntrack_tcp_timeout_established = 432000
```

432000 秒是 5 天。条目刚建立几秒，所以读到 431996 这种数。改这个 sysctl 会同时改新条目的剩余超时，已经在表里的条目按各自的值走。

ICMP 的条目没有端口，换成 type 和 id：

```text
icmp 1 29 src=10.77.1.1 dst=10.77.2.2 type=8 code=0 id=8264 src=10.77.2.2 dst=10.77.1.1 type=0 code=0 id=8264 mark=0 use=1
```

`type=8` 是 echo request，回复方向的 `type=0` 是 echo reply，同一个 `id` 把两个方向绑在一起。跟踪不区分"有没有 socket"，只看这组元组。

### 4.2 一条连接从建立到消失

`conntrack -E` 收的是事件，不是快照。开一个窗口，再让一对 TCP 和一对 UDP 跑一遍：

```bash
sudo ip netns exec nslab-r timeout 16 conntrack -E -o timestamp
```

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.2.2 18082
sudo ip netns exec nslab-b ./netdemo udp-server 10.77.2.2 18083
sudo ip netns exec nslab-a ./netdemo tcp-client 10.77.2.2 18082 20000
sudo ip netns exec nslab-a ./netdemo udp-client 10.77.2.2 18083
```

TCP 那一对产生的事件：

```text
[NEW] tcp 6 120 SYN_SENT src=10.77.1.1 dst=10.77.2.2 sport=36578 dport=18082 [UNREPLIED] src=10.77.2.2 dst=10.77.1.1 sport=18082 dport=36578
[UPDATE] tcp 6 60 SYN_RECV src=10.77.1.1 ...
[UPDATE] tcp 6 431999 ESTABLISHED src=10.77.1.1 ... [ASSURED]
[UPDATE] tcp 6 120 FIN_WAIT src=10.77.1.1 ...
[UPDATE] tcp 6 30 LAST_ACK src=10.77.1.1 ...
[UPDATE] tcp 6 120 TIME_WAIT src=10.77.1.1 ...
```

中间行省略了回复方向的元组，完整七行（含 UDP 一条）在[生命周期记录](/p/linux-network-netfilter-conntrack/ct-life.txt)。

三件事可以从这串事件读出来：

- `[NEW]` 只有一个，后面都是 `[UPDATE]`。状态变了才发事件，不是每个包都发。
- 每一行的数字是这条条目此刻的剩余超时，`120`、`60`、`431999`、`30` 分别对应 `tcp_timeout_syn_sent`、`syn_recv`、`established`、`last_ack`。状态一换，超时也跟着换。
- `[UNREPLIED]` 只出现在第一行，等回复方向也看到了才变成 `[ASSURED]`。UDP 那一对只发不收，所以整条一直是 `UNREPLIED`，超时停在 29（`udp_timeout` 是 30）。

事件都挤在同一毫秒里，是因为两端都在本机、路径只有一对 veth。真实网络里 `SYN_SENT` 到 `SYN_RECV` 会隔一个往返。

窗口结束后还能查到两条：

```bash
sudo ip netns exec nslab-r conntrack -L
```

```text
udp 17 17 src=10.77.1.1 dst=10.77.2.2 sport=33727 dport=18083 [UNREPLIED] src=10.77.2.2 dst=10.77.1.1 sport=18083 dport=33727 mark=0 use=1
tcp 6 107 TIME_WAIT src=10.77.1.1 dst=10.77.2.2 sport=36578 dport=18082 src=10.77.2.2 dst=10.77.1.1 sport=18082 dport=36578 [ASSURED] mark=0 use=1
```

TCP 那条在 `TIME_WAIT`，剩 107 秒，会挂到 120 秒到期；UDP 那条剩 17 秒，到 0 就消失，不需要谁手动清。

### 4.3 没有规则要它，就不记

这一节能解释很多"`conntrack -L` 为什么是空的"。

先什么都不装，转发一对 ping：

```bash
sudo ip netns exec nslab-r conntrack -L
sudo ip netns exec nslab-a ping -n -c 2 10.77.2.2
sudo ip netns exec nslab-r conntrack -L
```

```text
conntrack v1.4.9 (conntrack-tools): 0 flow entries have been shown.
```

ping 是通的（`ttl=63`，说明经过了转发），条目数仍然是 0。`conntrack -E` 在这段时间也收不到任何事件。

装一条读 ct 的规则，再跑同样的 ping：

```bash
sudo ip netns exec nslab-r iptables -t filter -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
sudo ip netns exec nslab-a ping -n -c 2 10.77.2.2
sudo ip netns exec nslab-r conntrack -L
```

```text
icmp 1 29 src=10.77.1.1 dst=10.77.2.2 type=8 code=0 id=8264 src=10.77.2.2 dst=10.77.1.1 type=0 code=0 id=8264 mark=0 use=1
```

条目出现了，`nf_conntrack_count` 也从 0 变成 1。

差别就在于有没有规则来读它。`-m conntrack`、`ct state`、`snat`/`dnat`/`masquerade` 这些表达式会要求这个命名空间启用连接跟踪；没有任何规则引用时，包照样转发，但没有人需要状态，内核也就不为这个环境维护表。`nf_conntrack` 本身是全局模块，表按网络命名空间各维护一份，没有规则请求，这份表就不会建起来。所以"`conntrack -L` 是空的"往往不是 bug，而是这台机器上还没有规则用到它。

网关和 NAT 设备上几乎一定有这类规则，那里的表总是热的；只做静态转发的环境可能一条都没有。

### 4.4 条目也会老、也会塞满

每条条目按自己的超时倒计时，到期就删。几个常用的值：

```bash
sudo ip netns exec nslab-r sysctl net.netfilter.nf_conntrack_tcp_timeout_established net.netfilter.nf_conntrack_tcp_timeout_time_wait net.netfilter.nf_conntrack_udp_timeout
```

```text
net.netfilter.nf_conntrack_tcp_timeout_established = 432000
net.netfilter.nf_conntrack_tcp_timeout_time_wait = 120
net.netfilter.nf_conntrack_udp_timeout = 30
```

表的容量和当前占用：

```bash
sudo ip netns exec nslab-r sysctl net.netfilter.nf_conntrack_max net.netfilter.nf_conntrack_count
```

```text
net.netfilter.nf_conntrack_max = 65536
net.netfilter.nf_conntrack_count = 1
```

65536 个条目对一台网关不算多，短连接密集时容易打满。打满之后不是一句"丢包"能说清的，计数器会告诉你卡在哪一步：

```bash
sudo ip netns exec nslab-r conntrack -S
```

```text
cpu=0 found=0 invalid=0 insert=0 insert_failed=0 drop=0 early_drop=0 error=0 search_restart=0 clash_resolve=0 chaintoolong=0
cpu=1 found=0 invalid=0 insert=0 insert_failed=0 drop=0 early_drop=0 error=0 search_restart=0 clash_resolve=0 chaintoolong=0
```

| 字段 | 说明什么 |
| --- | --- |
| `invalid` | 收到对不上任何状态的包，常见于状态过期后的迟到包 |
| `insert_failed` | 两个包同时要建同一条条目，后到的那个没插进去 |
| `drop` | 已达上限、又淘汰不掉，包被丢掉 |
| `early_drop` | 表快满时先淘汰未确认的条目腾位置 |
| `search_restart` | 查表期间表被改动，重新查的次数 |

`early_drop` 涨说明开始提前淘汰，`drop` 涨说明已经真的丢包。`[ASSURED]` 的条目不在提前淘汰的候选里，这就是这个标记的实际作用。

### 4.5 它和 socket 是两回事

| 对象 | 管什么 | 怎么看 |
| --- | --- | --- |
| 本机 TCP socket | 端到端的字节流、序号、重传、应用队列 | `ss -tinm` |
| conntrack 条目 | 经过这台机器的流的双向关联、状态、超时 | `conntrack -L`、`conntrack -E` |
| nft 规则 | 按条件匹配、执行策略 | `nft list`、trace、counter |

`ct state established` 不是"本机有个 TCP socket 处于 ESTABLISHED"，UDP 一样会被跟踪，转发的流量根本没有 socket。要不要放行 established/related 是策略问题，不是必须照抄的模板。

元组查找和状态组织见 [nf_conntrack_core.c](https://github.com/torvalds/linux/blob/v7.0/net/netfilter/nf_conntrack_core.c)。规则系统会读跟踪结果来匹配，但它们是两个对象。

## 五、SNAT 让两侧看到不同的源地址

### 5.1 只转 A 到 B 的流量

先收掉 4.3 那条 state 规则，再建转换：

```bash
sudo ip netns exec nslab-r iptables -t filter -F
sudo ip netns exec nslab-r nft add table ip natdemo
sudo ip netns exec nslab-r nft 'add chain ip natdemo post { type nat hook postrouting priority srcnat; policy accept; }'
sudo ip netns exec nslab-r nft add rule ip natdemo post ip saddr 10.77.1.0/24 oifname rb snat to 10.77.2.254
```

在 `rb` 出口把来自 `10.77.1.0/24` 的源地址换成 R 的 `10.77.2.254`。

先在一个终端起 B，第二个参数 20000 让它在 `accept` 之后先睡 20 秒再读：

```bash
sudo ip netns exec nslab-b ./netdemo tcp-server 10.77.2.2 18084 20000
```

再在另一个终端起 A，`tcp-fill` 写满发送缓冲后再挂 30 秒才关：

```bash
sudo ip netns exec nslab-a ./netdemo tcp-fill 10.77.2.2 18084 30000
```

这两个时长是给观察留的窗口：服务端 20 秒内不取数据，客户端 30 秒内不退出，中间这 20 秒里查 conntrack 和 `ss` 都来得及。

传输进行中，回到第一个终端的前台或另开一个终端，查两处：

```bash
sudo ip netns exec nslab-r conntrack -L -p tcp --dport 18084
sudo ip netns exec nslab-b ss -tn '( sport = :18084 )'
```

```text
tcp 6 431996 ESTABLISHED src=10.77.1.1 dst=10.77.2.2 sport=42864 dport=18084 src=10.77.2.2 dst=10.77.2.254 sport=18084 dport=42864 [ASSURED] mark=0 use=1
```

```text
State Recv-Q Send-Q Local Address:Port  Peer Address:Port
ESTAB 120560 0      10.77.2.2:18084     10.77.2.254:42864
```

B 看到的对端是 `10.77.2.254:42864`，不是 A 的 `10.77.1.1`。临时端口和剩余超时每次都不一样。两条命令的原始输出见[conntrack 条目](/p/linux-network-netfilter-conntrack/conntrack.txt)和[B 上的 ss](/p/linux-network-netfilter-conntrack/nat-peer.txt)。

### 5.2 两组元组

![SNAT 前后的正向地址以及回复方向元组](/p/linux-network-netfilter-conntrack/nat.svg)

| 位置 / 方向 | 源 | 目的 |
| --- | --- | --- |
| 转换前，A 发出 | `10.77.1.1:42864` | `10.77.2.2:18084` |
| B 看到的连接 | `10.77.2.254:42864` | `10.77.2.2:18084` |
| 条目里的回复方向 | `10.77.2.2:18084` | `10.77.2.254:42864` |

表里的 `42864` 是这一轮 A 用的临时端口，每次重跑都不一样；换成新的一轮，三行的端口要一起变。

条目里这两组地址是原始方向和回复方向，不是两条连接，也不是一个包里出现过的所有地址。有状态 NAT 在首包上建立关联，后续包复用状态并改写，回复方向按同一份状态还原。这正好接上 2.2 那个"nat 只数到 1 个包"的现象：地址转换的决策在首包做完，剩下的包只是照着状态改。

所以 NAT 是"规则选谁、状态记什么、改写在哪做"三件事的配合，不能简化成"conntrack 自己决定过滤并执行 NAT"。源码见 [nf_nat_core.c](https://github.com/torvalds/linux/blob/v7.0/net/netfilter/nf_nat_core.c)。

这个拓扑本来就有完整回程路由，不配 NAT 也能通。NAT 在这里只是让地址可见性变化，不是互通的必要条件。跟踪状态要占资源、有超时，地址转换也不等于身份认证或安全边界。

## 六、回到三条路径

| 场景 | 主线 | 能看的证据 |
| --- | --- | --- |
| 本机发送 | fd/socket → TCP/UDP → IP 输出与路由 → output/postrouting → 邻居、qdisc、设备 | strace、ss、`ip route get`、nft trace、tc、抓包 |
| 本机接收 | 设备与 NAPI → prerouting 与路由判断 → input → TCP/UDP 查找 → socket 队列与唤醒 | 驱动与 NAPI 计数、抓包、nft、ss |
| 转发 | 接收入口 → prerouting → 路由与转发检查 → forward → postrouting → 邻居与设备输出 | 两侧抓包、路由与邻居、nft、设备统计 |

对着第二节那张矩阵看：转发落在 `PREROUTING/FORWARD/POSTROUTING`，本机发送落在 `OUTPUT/POSTROUTING`，本机接收落在 `PREROUTING/INPUT`。三个场景合起来正好铺满五条链，没有第六条。

这张表对应职责，不承诺每条路径都按同一串函数执行。Netfilter 改了路由相关字段还可能触发重新判定，bridge 和隧道也各有边界。

排查时沿交接点问就行：调用接受了多少字节？对端协议栈收到没有？路由选中了谁？邻居解析出来了没有？队列有没有积压？在哪个 hook 得到了什么 verdict？每个问题都要有对应的证据，别从一句"ping 不通"直接跳到某个内核函数。

清理：

```bash
sudo ip netns exec nslab-r nft delete table ip natdemo
sudo bash lab.sh clean
```

## 七、命令与接口速查

| 命令 / 表达式 | 用途 |
| --- | --- |
| `iptables -t 表 -A 链 -j ACCEPT` | 在某张表的某条链上挂规则，隐式带计数器 |
| `iptables -t 表 -L -v -n` | 看规则与 `pkts`/`bytes`，按链分段 |
| `iptables -t 表 -Z` | 清零计数器，便于一轮一读 |
| `iptables -t 表 -F` | 清空某张表的规则 |
| `nft list tables` / `nft list table ip filter` | 看 iptables 建出来的 nft 表 |
| `meta nftrace set 1` + `nft monitor trace` | 圈定流量并接收规则轨迹 |
| `counter` | 累计匹配的包数与字节数 |
| `conntrack -L` | 列出现有跟踪条目 |
| `conntrack -E -o timestamp` | 跟随状态变化的事件流 |
| `conntrack -S` | 看 `insert_failed`、`drop`、`early_drop` 等统计 |
| `ct state established,related` | 用跟踪状态做匹配 |
| `nf_conntrack_max` / `nf_conntrack_count` | 表容量与当前占用 |
| `nf_conntrack_tcp_timeout_established` | 已建立连接条目的存活时间 |
| `snat to ADDR` | 为匹配的连接建立源地址转换 |
| `nft delete table FAMILY NAME` | 只删指定的实验表 |

## 八、名词解释

| 名词 | 含义 |
| --- | --- |
| hook | 内核路径上可接入处理的位置 |
| 表（table） | 规则的容器，按用途分 raw、mangle、nat、filter |
| 链（chain） | 挂在某个 hook 上的一组规则，内置五条 |
| verdict | 对当前包作出的继续、接受、丢弃等判定 |
| 元组 | 用协议、地址、端口识别一个方向流量的组合 |
| `NEW` | 只看到一个方向、还没被确认的新流 |
| `ESTABLISHED` | 两个方向都出现过，连接已经成立 |
| `UNREPLIED` | 还没看到回复方向的条目 |
| `[ASSURED]` | 双向都已确认的条目，内存紧张时不会被优先淘汰 |
| 有状态 NAT | 维护双向转换关联、供后续流量复用的地址转换 |
