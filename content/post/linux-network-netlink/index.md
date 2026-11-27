---
title: "Linux 网络栈（七）：Netlink 怎样把配置交给内核"
slug: linux-network-netlink
date: "2026-11-22T16:07:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "一次 ip addr add 发出的消息、到达的处理函数、返回的 ACK，以及订阅者收到的通知，四者不是一回事。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

`ip addr add` 执行时，shell 并没有直接改写网卡结构体。它到底发了什么，内核又凭什么找到那个接口？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · **7** · [8](/p/linux-network-namespace/) · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

第一篇 1.3 那张图上，Netlink 画在地图旁边而不是包的路径上。这一篇展开那条线：一条管理命令怎样变成内核对象，回执和通知又各是什么。它不处理数据包，只改数据包要用的对象。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。这个版本把 rtnetlink 的注册从逐个 `rtnl_register()` 换成了表，下面的代码按新写法给。

## 一、一次地址新增

### 1.1 命令与结果

```bash
sudo bash lab.sh direct
sudo ip netns exec nslab-a strace -qq -v -s 160 -e trace=socket,sendmsg,sendto,recvmsg -o netlink-trace.txt ip addr add 10.77.1.9/24 dev a0
sudo ip -n nslab-a -4 -br addr show dev a0
```

```text
a0@if2           UP             10.77.1.1/24 10.77.1.9/24
```

`a0@if2` 里的 `@if2` 是 veth 的对端索引，不影响地址。地址确实多了一个。

### 1.2 请求消息的字段

```bash
grep 'nlmsg_type=RTM_NEWADDR' netlink-trace.txt
```

把这一行拆开：

| 层次 | 字段 | 值 |
| --- | --- | --- |
| socket | 协议族 / 协议 | `AF_NETLINK` / `NETLINK_ROUTE` |
| 消息头 | `nlmsg_type` | `RTM_NEWADDR` |
| 消息头 | `nlmsg_flags` | `REQUEST`、`ACK`、`EXCL`、`CREATE` |
| 固定载荷 | `ifa_family` / `ifa_prefixlen` | `AF_INET` / `24` |
| 固定载荷 | `ifa_index` | `if_nametoindex("a0")` 的返回值 |
| 属性 | `IFA_LOCAL` / `IFA_ADDRESS` | 都是 `10.77.1.9` |

请求要带 `ACK`，内核才会回执；`EXCL` 和 `CREATE` 合起来表示"不存在才建，已存在就报错"。`ifa_index` 不是人填的，是用户态先用 `RTM_GETLINK` 查出来的。

这也解释了一个容易看错的地方。跟踪里有 8 行，第 3 行是 `RTM_GETLINK` 请求，第 5 行的回复里 `nlmsg_type=RTM_NEWLINK`、`nlmsg_flags=0`、`nlmsg_pid=-1`——那是一条**查询回复**，不是"新建了设备"。看到 `RTM_NEWLINK` 就断言建了链路，会错在这里。

属性是长度、类型、值三段，习惯叫 TLV，可以嵌套。好处是加参数不用改大结构体；但哪些属性合法仍由具体子系统规定，不是随便塞键值都能被接受。

## 二、从消息到内核对象

![ip 请求经 Netlink 分发修改地址，再返回 ACK 和独立通知](/p/linux-network-netlink/netlink.svg)

### 2.1 两级分发

`NETLINK_ROUTE` 圈定网络配置这一块，消息类型加协议族再定位到具体函数。v7.0 里 IPv4 地址的注册是一张表：

```c
/* net/ipv4/devinet.c */
static const struct rtnl_msg_handler devinet_rtnl_msg_handlers[] __initconst = {
	{.protocol = PF_INET, .msgtype = RTM_NEWADDR, .doit = inet_rtm_newaddr,
	 .flags = RTNL_FLAG_DOIT_PERNET},
	{.protocol = PF_INET, .msgtype = RTM_DELADDR, .doit = inet_rtm_deladdr,
	 .flags = RTNL_FLAG_DOIT_PERNET},
	{.protocol = PF_INET, .msgtype = RTM_GETADDR, .dumpit = inet_dump_ifaddr, ...},
	...
};

void __init devinet_init(void)
{
	register_pernet_subsys(&devinet_ops);
	rtnl_register_many(devinet_rtnl_msg_handlers);
}
```

`inet_rtm_newaddr()` 解析前缀和属性，按 `ifa_index` 找到设备，校验之后把地址挂上去。接口是 `net_device`，IPv4 的接口配置记在 `in_device` 里，地址是 `in_ifaddr`——新增地址不会新建一个 `net_device`。

换成 `ip link add ... type veth`，主消息是 `RTM_NEWLINK`，走的是链路创建那条分发，再交给 veth 类型的操作。两条路完全不是一个函数。核对入口：[rtnetlink.c](https://github.com/torvalds/linux/blob/v7.0/net/core/rtnetlink.c)、[devinet.c](https://github.com/torvalds/linux/blob/v7.0/net/ipv4/devinet.c)。

### 2.2 对象不是一行文本

`ip addr show` 每次重新向内核查询，再把二进制结果格式化，不是读上次命令留下的文本。地址变化还可能连带更新相关路由。

而机器重启后地址没了，是另一件事：Netlink 只改运行态，持久化由发行版的网络管理服务负责。排查"执行成功了，重启就没了"，要把运行态对象和持久配置分开看。

## 三、ACK、错误与通知

### 3.1 回复里才看得见结果

请求里带了 `ACK`，内核就用 `NLMSG_ERROR` 这个类型装结果：

```text
recvmsg(3, {... [{nlmsg_len=36, nlmsg_type=NLMSG_ERROR, nlmsg_flags=NLM_F_CAPPED,
  nlmsg_seq=1795791043, nlmsg_pid=15333}, {error=0, msg={nlmsg_len=40,
  nlmsg_type=RTM_NEWADDR, ...}}] ...})
```

`error=0` 才是成功。类型叫 `NLMSG_ERROR`，装的成功回执也走它，名字不代表失败。

`nlmsg_seq` 把请求和回复对上；`nlmsg_pid` 是通信端点的标识，不要当成应用 PID。大查询还会拆成 multipart，用结束消息收尾。

再执行一次同一条命令：

```bash
sudo ip -n nslab-a addr add 10.77.1.9/24 dev a0
```

```text
Error: ipv4: Address already assigned.
```

退出码是 2。这个错误是内核回的，`ip` 只是转述；脚本里判断成败用退出码，不要去匹配这段英文。

### 3.2 订阅者收到的是事件

终端一订阅，终端二删地址：

```bash
sudo ip netns exec nslab-a ip monitor address
```

```bash
sudo ip -n nslab-a addr del 10.77.1.9/24 dev a0
```

监听者不是那条配置命令的调用者，它订阅的是状态变化。通知来得及时，但不能代替初始查询和错误处理：订阅之前发生了什么它不知道，事件也可能丢，缓存只能当近似视图。

## 四、底座与其中的一种接口

| 工具 / 接口 | 常见协议或组织 | 操作对象 |
| --- | --- | --- |
| `ip`、`tc` | `NETLINK_ROUTE` / rtnetlink | 链路、地址、路由、队列 |
| `nft` | `NETLINK_NETFILTER` | Netfilter 规则与对象 |
| `ss` | socket diagnostic | socket 状态 |
| Generic Netlink | 动态家族 + 命令 + 属性 | 各类可扩展子系统 |

它们都发 Netlink 消息，但不能都画进 `rtnl_newlink()`。Generic Netlink 还有家族发现和动态编号，和这里的经典 `NETLINK_ROUTE` 不是同一条路。总体模型见 [Netlink 文档](https://docs.kernel.org/7.0/userspace-api/netlink/intro.html)。

Netlink 在配置这块的价值就一句：用户态说意图，内核校验并维护对象，查询和通知把状态带回来。数据包走的是这些对象，不会为每个包再发一次管理命令。

收尾：

```bash
sudo bash lab.sh clean
```

## 五、命令与接口速查

| 命令 / 字段 | 用途 |
| --- | --- |
| `strace -e trace=socket,sendmsg,sendto,recvmsg` | 看管理程序真正发了什么 |
| `ip monitor address` | 订阅地址变化 |
| `RTM_NEWADDR` / `RTM_NEWLINK` | 地址新增、链路操作，要分清请求和回复 |
| `NLMSG_ERROR` | 装错误，也装成功回执 |
| `rtnl_register_many()` | v7.0 里按表注册 rtnetlink 处理函数 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| rtnetlink | 基于 Netlink 的路由、链路等网络配置接口 |
| 属性（TLV） | 消息里按长度、类型、值组织的可扩展参数 |
| dump | 对一组对象发起的批量查询 |
| 组播通知 | 发布给订阅了相关事件的用户态端点的消息 |
| `in_device` / `in_ifaddr` | IPv4 的接口配置与地址对象 |
