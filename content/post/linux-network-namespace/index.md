---
title: "Linux 网络栈（八）：网络命名空间的状态与生命周期"
slug: linux-network-namespace
date: "2026-11-22T16:08:00+08:00"
lastmod: "2026-11-27"
draft: false
description: "一个只有 lo 的网络环境从哪来、名字为什么不是对象本身、删掉名字之后对象为什么还活着。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 网络栈
---

`ip netns list` 里已经看不到那个名字了，进程怎么还在原来的网络环境里跑？"删掉命名空间"到底删了什么？

系列导航：[1](/p/linux-network-stack-map/) · [2](/p/linux-network-socket-transport/) · [3](/p/linux-network-skbuff/) · [4](/p/linux-network-driver-napi/) · [5](/p/linux-network-route-neighbor/) · [6](/p/linux-network-qdisc/) · [7](/p/linux-network-netlink/) · **8** · [9](/p/linux-network-veth-bridge/) · [10](/p/linux-network-netfilter-conntrack/)

前七篇的实验一直跑在 `nslab-a`、`nslab-b`、`nslab-r` 里，读者照着做却没问过这些环境是怎么来的。这一篇补上：一台机器上能并存几套网络状态，名字和对象是什么关系，引用怎么决定生死。后面第九篇的 veth、bridge 都建在这些环境之间。

实验机是 `7.0.0-44`，源码对照上游 `v7.0`。

## 一、一个只有 lo 的环境

### 1.1 创建

```bash
sudo ip netns add nslab-a
sudo ip -n nslab-a -br link
sudo ip -n nslab-a route
```

```text
lo               DOWN           00:00:00:00:00:00 <LOOPBACK>
```

路由表是空的。

新的网络命名空间不会继承宿主机的网卡、默认路由和外网。这里的 `lo` 是它自己的回环设备，而且默认是 DOWN 的——所以新环境里 ping 127.0.0.1 也不通。

`ip netns exec` 改变的只是这个程序看到的网络环境，不是把它塞进一台独立内核的虚拟机。CPU、内存、调度器都还是同一套。

### 1.2 隔离哪些状态

![两个 task 指向不同 netns，各自持有设备、路由与网络协议状态](/p/linux-network-namespace/netns.svg)

| 状态 | netns 的作用 |
| --- | --- |
| 设备与地址 | 各自的设备视图和配置，设备可按规则搬进搬出 |
| 路由与邻居 | 各自维护 |
| socket 与端口空间 | 普通 socket 创建时就归属某个环境 |
| 防火墙等子系统 | 各子系统自己的 per-net 状态 |
| CPU、内存、内核代码 | 不因为 netns 变成另一台机器 |

`struct net` 是内核里的表示，各子系统通过 per-net 注册挂自己的私有状态。所以不能说"所有隔离表都内嵌在 `struct net` 里"，更不能说 netns 提供了独立调度或独立内存。定义见 [net_namespace.h](https://github.com/torvalds/linux/blob/v7.0/include/net/net_namespace.h) 与 [net_namespace.c](https://github.com/torvalds/linux/blob/v7.0/net/core/net_namespace.c)。

## 二、拉起 lo 只换来回环

```bash
sudo ip -n nslab-a link set lo up
sudo ip netns exec nslab-a ping -n -c 1 127.0.0.1
```

通了。但也仅此而已：没有对外的接口，没有默认网关。拉起 lo 和接通外网是两件事。

顺带一个常被忽略的点：`127.0.0.1:18080` 这样的地址可以在多个环境里同时被监听，各自属于各自的 socket。要跨环境通信，就得建链路、配地址、写路由——第九篇用 veth 和 bridge 做这件事。

## 三、名字不是对象本身

### 3.1 `/run/netns` 是持久入口

`ip netns add NAME` 不只是写一个文本文件。它创建命名空间，再在 `/run/netns/NAME` 上做一次绑定挂载，留下一个入口。有了这个挂载引用，创建命令退出之后环境还活着，后面的工具也能按名字打开它。

| 入口 | 能看到什么 |
| --- | --- |
| `/proc/PID/ns/net` | 某个进程当前关联的网络命名空间 |
| `/run/netns/NAME` | iproute2 管理的命名入口 |
| `ip netns list` | 命名入口列表，不是所有活着的 netns |
| `nsenter -t PID -n CMD` | 按进程进入它的网络环境 |

入口不是唯一的引用来源。进程、打开的 namespace fd、别的绑定挂载都能撑着它。删掉一个名字，未必结束对象的生命周期。

### 3.2 名字删掉之后

```bash
sudo ip netns del nslab-a
sudo ip netns add nslab-a
sudo ip netns exec nslab-a sleep 60 &
PID=$(sudo ip netns pids nslab-a | head -n 1)
sudo readlink /proc/$PID/ns/net
sudo ip netns del nslab-a
sudo ip netns list
sudo readlink /proc/$PID/ns/net
sudo nsenter -t "$PID" -n ip -br link
```

这台机器上只有这一个命名入口，删除后列表是空的；两次 `readlink` 和最后的接口输出：

```text
net:[4026532692]
net:[4026532692]
lo               DOWN           00:00:00:00:00:00 <LOOPBACK>
```

两次标识一样，说明 `sleep` 还撑着原来那个环境。数字由运行时分配，每台机器、每次运行都可能不同。[原始记录](/p/linux-network-namespace/netns-lifetime.txt) 里有 PID 和命令顺序。

链条很清楚：删掉命名入口 → 列表不再显示名字 → 进程的引用还在 → 还能经进程进去。而且进去看到的 `lo` 还是 DOWN，这个环境从头到尾没被改过。

清理：

```bash
sudo kill "$PID"
wait
```

进程退出只是放掉它那一份引用。如果还有别的引用，对象仍然不会立即销毁；最终释放和各子系统的退出由引用计数驱动，不是工具"把里面的东西一次删干净"。

## 四、已有 socket 的归属

普通 socket 在创建时绑定到某个网络命名空间，这个归属不会因为线程后来 `setns()` 就改掉。进程还可以通过继承或 fd 传递，持有别的环境里创建的 socket。

所以"当前线程进了 netns B"和"我手里所有通信对象都属于 B"是两回事。排查时要分清创建时的归属和当前的视图。安全隔离也同理：只看命名空间名字不够，还要看能力、可传递的对象和现有引用。

`setns()` 进入已有的命名空间，`unshare(CLONE_NEWNET)` 给调用者造一个新的。两者都受权限和其他命名空间约束。

## 五、命令与接口速查

| 命令 / 接口 | 用途 |
| --- | --- |
| `ip netns add/del/list` | 管理命名入口及其环境 |
| `ip netns pids NAME` | 列出属于该环境的进程 |
| `readlink /proc/PID/ns/net` | 看进程关联的身份 |
| `nsenter -t PID -n` | 按进程进入网络环境 |
| `setns()` / `unshare(CLONE_NEWNET)` | 进入已有、创建新的环境 |

## 六、名词解释

| 名词 | 含义 |
| --- | --- |
| per-net | 子系统按网络命名空间分别组织状态和生命周期 |
| 命名入口 | 用户态通过路径再次定位内核命名空间的入口 |
| 绑定挂载引用 | 靠挂载维持入口，让对象在创建者退出后仍可访问 |
| 生命周期 | 从创建、被引用到最终释放的全过程 |
