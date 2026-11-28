---
title: "Linux eBPF 编程：从字节码到挂载点的接口"
slug: linux-ebpf-programming
date: "2026-11-28T20:30:00+08:00"
lastmod: "2026-11-28"
draft: false
description: "eBPF 的加载、校验、JIT、map 与 helper 怎么用，以及在 XDP、tc、cgroup、sockmap 四个挂载点上怎么写。"
categories:
    - Linux 与系统编程
tags:
    - Linux
    - eBPF
    - 网络
---

这一篇讲 eBPF 的接口：一段 C 代码要经过哪几步才能变成内核里一个正在跑的程序，每一步的输入输出是什么，map 和 helper 怎样在程序和用户态之间传递数据，以及同一段程序挂到不同挂载点时写法差在哪。

范围是应用层写 eBPF 程序会碰到的东西：`bpf()` 系统调用、程序的四类对象、校验器、JIT、map 类型、helper、开发工具链，最后落在四个挂载点上。内核内部怎样执行校验、JIT 怎样分配可执行内存，只在影响写法时提一句。

实验环境是 Ubuntu 26.04、内核 7.0.0-44、aarch64、2 vCPU，clang 21.1.8、bpftool v7.7.0（libbpf v1.7）。实验一律在 netns 和 veth 里做，不碰 SSH 用的物理网卡。各节用到的程序、脚本和输出都在同目录。

## 一、从 BPF 到 eBPF

### 1.1 最初的 BPF：给抓包用的过滤器

1992 年，Lawrence Berkeley 实验室给 `tcpdump` 写了一个内核里的过滤器：用户态把一串过滤表达式编译成简单的字节码，交给内核，每个报文到来时执行一遍，结果为真就收进抓包缓冲，为假就丢掉。这就是 BPF（Berkeley Packet Filter）。

它的设计目标很窄，也很清楚：

- 指令集小，只有几十条，一寄存器累加器式，没有函数调用。
- 只能读报文，不能写，不能调用内核函数。
- 必须在报文边界内读写，越界直接判失败。

这几条限制换来一个好处：内核敢让它在软中断里对每个包跑一遍，因为最坏情况可以静态算出来。

### 1.2 eBPF 改了什么

2014 年前后，这套虚拟机被扩展成 eBPF（extended BPF）。改动集中在四处：

- **寄存器从 2 个变成 11 个**（`r0`–`r10`），`r10` 只读，指向 512 字节的栈帧；支持函数调用和尾调用。
- **指令从几十条扩到上百条**，有 64 位算术、字节序转换、原子操作。
- **程序不再只能看报文**，可以访问 map、读取内核结构体字段（受 BTF 描述约束）、调用白名单里的 helper。
- **挂载点从抓包扩展到几十个**：网卡收包路径（XDP）、流量控制（tc）、系统调用（kprobe/tracepoint）、cgroup、LSM、socket 层等。

限制仍然在：不能解引用任意指针、循环必须有界、栈只有 512 字节、不能睡眠（少数程序类型例外）。校验器（verifier）在加载时把整个程序走一遍，确认这些条件成立才放行。

### 1.3 全景

一段 eBPF 程序的完整生命周期是：

1. `clang -target bpf` 把 C 编译成 eBPF 字节码，放进 ELF 文件的 `.text` 段，map 定义和 license 也在 ELF 里。
2. 加载器（`bpftool` 或 libbpf 写的程序）读 ELF，把字节码和 map 定义交给 `bpf()` 系统调用。
3. 内核校验器检查字节码，通过后由 JIT 编译成机器码，得到一个 `prog` 对象。
4. 加载器把这个 prog 挂到某个挂载点上，得到一个 `link` 对象。
5. 程序运行时通过 map 与用户态交换数据；卸载时销毁 link，prog 引用归零后释放。

![eBPF 程序的加载链路：编译、加载、校验、JIT、挂载](/p/linux-ebpf-programming/ebpf-lifecycle.svg)


四个对象贯穿全文，先记住它们的名字：

- **prog**：一份已校验、已编译的 eBPF 程序。
- **map**：程序和用户态共享的键值存储。
- **link**：prog 与挂载点的绑定关系，销毁 link 即卸载。
- **BTF**：类型信息，让程序能按名字访问内核结构体字段，也是 CO-RE 的基础。

## 二、bpf() 系统调用与四类对象

### 2.1 一个系统调用，几十个命令

所有 eBPF 操作都走同一个系统调用：

```c
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <unistd.h>

int bpf(int cmd, union bpf_attr *attr, unsigned int size);
```

`cmd` 决定这次要做什么，`attr` 是一个联合体，不同命令用其中不同字段。常用的几条：

| cmd | 作用 | 返回 |
| --- | --- | --- |
| `BPF_MAP_CREATE` | 创建 map | map fd |
| `BPF_MAP_LOOKUP_ELEM` | 查一个键 | 0 / -1 |
| `BPF_MAP_UPDATE_ELEM` | 写入一个键 | 0 / -1 |
| `BPF_MAP_DELETE_ELEM` | 删除一个键 | 0 / -1 |
| `BPF_PROG_LOAD` | 加载程序 | prog fd |
| `BPF_PROG_ATTACH` | 把程序挂到 cgroup 等挂载点 | 0 / -1 |
| `BPF_LINK_CREATE` | 创建 link 对象 | link fd |
| `BPF_OBJ_GET` / `BPF_OBJ_PIN` | 按路径取对象 / 把对象固定到 bpffs | fd / 0 |

实际写程序很少直接调 `bpf()`，libbpf 把每个命令包成了 `bpf_xxx()` 函数，参数也换成了 fd 和普通指针：

```c
int bpf_map_lookup_elem(int fd, const void *key, void *value);
int bpf_map_update_elem(int fd, const void *key, const void *value, __u64 flags);
int bpf_map_delete_elem(int fd, const void *key);
int bpf_prog_attach(int prog_fd, int target_fd, enum bpf_attach_type type,
		    unsigned int flags);
```

返回 0 或正数表示成功，`-1` 表示失败，`errno` 是具体原因。这一条贯穿全文：**所有 eBPF 接口都用 `-1` + `errno` 报错**，没有例外。

### 2.2 四类对象

**prog**。加载后得到 prog fd，生命周期跟着 fd 走。fd 关闭且没有其他地方引用（pin、link、map 里的引用），程序就卸载。`bpftool prog show` 能看到它的类型、指令数、JIT 后的字节数、tag。

**map**。创建时指定类型、键大小、值大小、最大条目数。map 可以被多个程序共享，也可以 pin 到 bpffs 让另一个进程打开。map 是唯一能让程序和用户态双向传数据的东西。

**link**。老接口用 `BPF_PROG_ATTACH` 把 prog 挂到 cgroup 的 fd 上，卸载要再调一次 detach，容易漏。新接口 `BPF_LINK_CREATE` 返回一个 link fd，fd 关闭即卸载，天然跟随进程生命周期。能返回 link 的挂载点优先用 link。

**BTF**。内核在编译时把类型信息写进 `vmlinux`，运行时暴露在 `/sys/kernel/btf/vmlinux`。有了它，程序可以直接写 `skb->protocol` 而不必硬编码偏移，换内核版本也不用重编。`bpftool btf dump` 可以把它导出成 C 头文件：

```bash
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

这个文件有 8 MB 左右，只包含类型定义，不含宏。

### 2.3 一个最小闭环

下面这段程序统计进入某网卡的 ICMP echo request，并丢掉它们。它包含一个 map、一个 XDP 程序、一个 `SEC()` 标记：

```c
// SPDX-License-Identifier: GPL-2.0
/* 统计 ICMP echo request 并丢弃，验证 XDP 挂在 veth 的入向。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800		/* vmlinux.h 只有类型，没有这个宏 */
#endif

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

SEC("xdp")
int xdp_ping(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	struct ethhdr *eth = data;
	struct iphdr *iph;
	struct icmphdr *icmp;
	__u32 key = 0;
	__u64 *cnt;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;
	if (iph->protocol != IPPROTO_ICMP)
		return XDP_PASS;

	icmp = (void *)((__u8 *)iph + iph->ihl * 4);
	if ((void *)(icmp + 1) > data_end)
		return XDP_PASS;
	if (icmp->type != 8)			/* 只处理 echo request */
		return XDP_PASS;

	cnt = bpf_map_lookup_elem(&stats, &key);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);

	return XDP_DROP;
}

char _license[] SEC("license") = "GPL";
```

编译、加载、挂载、读 map、卸载：

```bash
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. \
      -Wno-missing-declarations -c xdp_ping.bpf.c -o xdp_ping.bpf.o

bpftool prog load xdp_ping.bpf.o /sys/fs/bpf/xdp_ping type xdp
ip link set dev veth2 xdp obj xdp_ping.bpf.o sec xdp
bpftool map dump name stats
ip link set dev veth2 xdp off
rm -f /sys/fs/bpf/xdp_ping
```

在 `ns2` 里 ping `ns1`，三个包全部丢失：

```
3 packets transmitted, 0 received, 100% packet loss
```

`bpftool map dump name stats` 会打出两个同名 map：一个是上一轮残留的（计数 0），一个是本次挂载的（计数 3）。按名字 dump 命中多个 map 的情况在 5.4 节展开。

`bpftool prog show` 能看到程序已经 JIT：

```
449: xdp  name xdp_ping  tag 9f3c...  gpl
	loaded_at ...  uid 0
	xlated 288B  jited 176B  memlock 4096B  map_ids 12
```

到这里四类对象都出现了：prog（449）、map（12）、挂载关系（`ip link set ... xdp` 创建的 link）、类型信息（vmlinux.h 来自 BTF）。

## 三、加载器与校验器

### 3.1 加载分三步

libbpf 把加载拆成三个可分开的调用，出问题时能定位在哪一步：

```c
struct bpf_object *obj = bpf_object__open_file("x.bpf.o", NULL);  /* 读 ELF */
int err = bpf_object__load(obj);                                   /* 调 bpf() */
struct bpf_link *link = bpf_program__attach(prog);                 /* 挂载 */
```

- `open` 只解析 ELF，不碰内核。ELF 格式错、`SEC()` 名字不认识，在这一步报错。
- `load` 为每个 map 调 `BPF_MAP_CREATE`，为每个 prog 调 `BPF_PROG_LOAD`。校验器在这一步工作，失败时 `errno` 是 `EACCES`，详细原因在 verifier log 里。
- `attach` 把 prog 挂到挂载点。不同程序类型走不同的 attach 函数。

`bpftool prog load` 相当于把前两步合起来，第三步要另外用 `bpftool net attach`、`ip link set`、`bpftool cgroup attach` 等命令。

加载时有两个容易踩的点：

- **license**。程序里必须有 `char _license[] SEC("license") = "GPL";`。写成 `"GPL"` 才能调用标记为 GPL-only 的 helper（如 `bpf_probe_read_kernel`、`bpf_skb_load_bytes` 的一部分）；写成 `"Dual BSD/GPL"` 也行。不写 license，加载时报 `license` 相关错误。
- **`type` 参数不要乱传**。`bpftool prog load x.bpf.o DIR type cgroup_sock_addr` 会让 bpftool 按这个名字去猜 section，报 `failed to guess program type from ELF section 'cgroup_sock_addr/'`。让 `SEC("cgroup/connect4")` 自己决定类型，不传 `type`。

### 3.2 校验器检查什么

校验器是 eBPF 安全模型的核心。它对每条指令做符号执行，维护寄存器的类型（`pkt`、`map_value`、`ctx`、`scalar`、`stack`）和取值范围，检查：

- **内存访问在界内**。读报文前必须有 `data + n <= data_end` 的分支；读 map 值前必须确认 key 存在；栈访问不能越过 512 字节。
- **指针来源合法**。不能把标量当指针用，不能做指针算术越界，不能把内核指针泄露给用户态。
- **循环有界**。老内核只允许展开的循环，新内核允许有界 `for`，但上界必须是编译期常量或校验器能证明的常量。
- **helper 调用参数类型匹配**。`bpf_map_lookup_elem` 的第一个参数必须是 map 指针，第二个必须是栈上或 map 里的键。
- **程序会终止**。不能有不可达的无限循环，不能有未初始化就返回的路径。
- **指令数上限**。当前是 100 万条（含所有分支展开），超过报 `processed N insns (limit 1000000)`。

### 3.3 读拒绝日志

把 2.3 的边界检查删掉，只留读以太网头：

```c
SEC("xdp")
int bad_access(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;

	return eth->h_proto;		/* 没有校验 data + sizeof(*eth) <= data_end */
}
```

`bpftool prog load` 直接拒绝：

```
libbpf: prog 'bad_access': BPF program load failed: -EACCES
libbpf: prog 'bad_access': -- BEGIN PROG LOAD LOG --
0: R1=ctx() R10=fp0
; void *data = (void *)(long)ctx->data; @ bad_access.bpf.c:9
0: (61) r1 = *(u32 *)(r1 +0)          ; R1=pkt(r=0)
; return eth->h_proto;		/* 没有校验 data + sizeof(*eth) <= data_end */ @ bad_access.bpf.c:12
1: (69) r0 = *(u16 *)(r1 +12)
invalid access to packet, off=12 size=2, R1(id=0,off=12,r=0)
R1 offset is outside of the packet
processed 2 insns (limit 1000000) max_states_per_insn 0 total_states 0 peak_states 0 mark_read 0
-- END PROG LOAD LOG --
libbpf: prog 'bad_access': failed to load: -EACCES
```


这段日志值得逐行读：

- `0: R1=ctx() R10=fp0` 是进入程序时的寄存器状态。`ctx` 是上下文指针，`fp0` 是栈帧。
- `(61) r1 = *(u32 *)(r1 +0)` 是第一条指令：从 ctx 偏移 0 读 4 字节，得到 `data` 指针。后面的 `; R1=pkt(r=0)` 是校验器给它的新类型：报文指针，当前偏移 0，`r=0` 表示还没做范围检查。
- `(69) r0 = *(u16 *)(r1 +12)` 试图从报文偏移 12 读 2 字节。校验器只知道这个指针的偏移是 0，不知道 12 之后还有没有数据，于是报 `invalid access to packet`。
- 最后一行 `processed 2 insns` 说明它检查了两条就停了，没有继续往下走。

修法就是 2.3 里那两行 `data + sizeof(*eth) > data_end` 的判断。校验器不理解“以太网头一定是 14 字节”这种业务常识，它只认分支。

### 3.4 谁能加载

内核有两个开关控制非特权用户：

```bash
sysctl kernel.unprivileged_bpf_disabled
```

值 `2` 表示只有 root（或有 `CAP_BPF`）能加载，且不能改回去；`1` 表示禁止但可改回；`0` 表示允许非特权加载（只允许 socket filter 等少数类型）。实验机是 `2`，所以下面所有命令都要 root。

另外 `kernel.kptr_restrict`、`kernel.perf_event_paranoid` 会影响 `bpftool prog show` 能否看到内核地址，不影响程序运行。

## 四、JIT

### 4.1 字节码不直接解释执行

eBPF 字节码是固定长度的 8 字节指令，字段是 `opcode`、`dst_reg`、`src_reg`、`off`、`imm`。内核里有一个解释器可以直接跑它，但默认路径是 JIT：加载时把字节码逐条翻译成宿主机的机器指令，之后每次触发都直接执行机器码。

x86-64、aarch64、riscv 等架构都有 JIT。开关在：

```bash
cat /proc/sys/net/core/bpf_jit_enable     # 1 表示启用
cat /proc/sys/net/core/bpf_jit_harden    # 0/1/2，对非特权程序做常量盲化
```

大多数发行版把 `bpf_jit_enable` 编成默认 1，且不允许关。实验机上 `CONFIG_BPF_JIT_ALWAYS_ON=y`，解释器根本不参与。

### 4.2 怎么确认走了 JIT

`bpftool prog show` 的 `xlated` 和 `jited` 两个字段就是证据：

```
xlated 288B  jited 176B
```

`xlated` 是校验器重写后的字节码长度（校验器会插入一些指令、改写部分跳转），`jited` 是最终机器码长度。`jited` 不为 0 就说明 JIT 生效。加上 `-j` 还能看反汇编：

```bash
bpftool prog dump jited id 449
```

有的发行版 bpftool 编译时没带反汇编支持，会报 `No JIT disassembly support`，这时只能靠上面两个字节数判断。

### 4.3 JIT 带来的两个限制

- **不能任意跳转**。字节码里没有间接跳转指令，JIT 生成的机器码也不会有。所有分支在校验阶段就已经确定，这是“程序一定会终止”能被证明的前提。
- **常量可能被盲化**。`bpf_jit_harden=2` 时，JIT 会把程序里的立即数拆成“异或一个随机数再异或回来”，防止攻击者用已知常量在机器码里定位 gadget。这只影响非特权加载的程序。

JIT 还决定了一件事：eBPF 程序的执行成本接近一段普通内核代码，而不是“解释一条指令再分派一次”。这也是它能在 XDP 这种每包路径上用的原因。

## 五、maps

### 5.1 定义与创建

map 在 BPF 源码里用 BTF 风格定义，`SEC(".maps")` 把它登记到 ELF 的 maps section：

```c
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u32);
	__type(value, struct flow_key);
} flows SEC(".maps");
```

`__uint` 描述数值属性，`__type` 描述键值类型。加载器读到这段定义后调 `BPF_MAP_CREATE`，之后程序里对 `&flows` 的引用会被重定位成 map fd。

可选的属性还有：

- `__uint(map_flags, BPF_F_NO_PREALLOC)`：hash map 不预分配元素，省内存但插入可能失败。
- `__uint(pinning, LIBBPF_PIN_BY_NAME)`：加载时把 map pin 到 bpffs，同名 map 复用。
- `__uint(numa_node, 0)`：指定 NUMA 节点。

### 5.2 常用类型

| 类型 | 键 | 语义 | 典型用途 |
| --- | --- | --- | --- |
| `BPF_MAP_TYPE_ARRAY` | `0..n-1` | 预分配，读不加锁 | 计数器、配置 |
| `BPF_MAP_TYPE_HASH` | 任意 | 预分配桶，插入可能失败 | 流表、连接跟踪 |
| `BPF_MAP_TYPE_LRU_HASH` | 任意 | 满了淘汰最久未用 | 大流量下的采样表 |
| `BPF_MAP_TYPE_PERCPU_ARRAY` | `0..n-1` | 每个 CPU 一份 | 无竞争的计数 |
| `BPF_MAP_TYPE_PERCPU_HASH` | 任意 | 每个 CPU 一份 | 分 CPU 统计 |
| `BPF_MAP_TYPE_RINGBUF` | 无键 | 单生产者多消费者环形缓冲 | 往用户态送事件 |
| `BPF_MAP_TYPE_SOCKMAP` | `0..n-1` | 存 socket 引用 | socket 层重定向 |
| `BPF_MAP_TYPE_SOCKHASH` | 任意 | 存 socket 引用 | 按四元组查 socket |

`ARRAY` 和 `PERCPU_ARRAY` 的区别在并发：前者所有 CPU 共享一份，计数器要用原子加；后者每个 CPU 各有一份，`bpf_map_lookup_elem` 拿到的是本 CPU 的副本，普通加法即可，用户态读的时候把各 CPU 的值加起来。

### 5.3 程序里怎么访问

三个函数：

```c
void *bpf_map_lookup_elem(void *map, const void *key);
long bpf_map_update_elem(void *map, const void *key, const void *value, __u64 flags);
long bpf_map_delete_elem(void *map, const void *key);
```

`lookup` 返回指向值的指针，键不存在返回 `NULL`。**返回的指针只在本次程序执行内有效**，不能存到 map 里下次用，也不能传给另一个程序。

`update` 的 `flags`：

- `BPF_ANY`：存在就覆盖，不存在就插入。
- `BPF_NOEXIST`：只在键不存在时插入，已存在返回 `-EEXIST`。
- `BPF_EXIST`：只在键存在时更新，不存在返回 `-ENOENT`。

`BPF_NOEXIST` 在并发场景下有用：多个 CPU 同时插同一个键，只有一个成功，其余拿到 `-EEXIST`，可以据此判断“我是第一个”。

### 5.4 用户态怎么访问

同一个 `bpf_map_lookup_elem(fd, &key, &value)`，只是第一个参数从 map 指针换成了 fd。批量操作有 `bpf_map_lookup_batch`、`bpf_map_update_batch`、`bpf_map_delete_batch`，一次系统调用处理多个键，适合大表同步。

`bpftool` 是最快的检查手段：

```bash
bpftool map list                       # 所有 map 的 id、类型、名字
bpftool map dump id 12                 # 按 id dump
bpftool map dump name stats            # 按名字 dump，可能命中多个同名 map
bpftool map lookup id 12 key 0         # 查单个键
bpftool map update id 12 key 0 value 0
```

`dump name` 会匹配所有同名 map。实验里曾经同时存在两个叫 `stats` 的 map（一个来自上一轮没卸载的程序），`dump name stats` 把两个都打出来，其中一个全是 0。要精确定位，先用 `bpftool prog show -f` 看程序的 `map_ids`，再按 id dump。

### 5.5 一个记录不到的情况

sockmap 这类存放 socket 引用的 map，`bpftool map dump` 不返回条目。实验里 `sock_ops` 把两个 socket 插进 `SOCKHASH`，`bpf_sock_hash_update` 返回 0，但 `bpftool map dump name sock_hash` 输出 `Found 0 elements`。这不是插入失败，而是这类 map 的 dump 路径不提供遍历。要确认插入成功，得在程序里加一个计数 map：

```c
if (bpf_sock_hash_update(skops, &sock_hash, &key, BPF_NOEXIST) == 0)
	bump(0);	/* stats[0]++ */
```

计数从 0 变 1，才说明真的进去了。这一点在后面 sockmap 一节还会用到。

## 六、helpers

### 6.1 helper 是什么

eBPF 程序不能直接调用任意内核函数，只能调用一张白名单里的 helper。每个 helper 有一个固定编号，调用指令的 `imm` 字段就是编号，校验器按编号查表确认参数类型和返回类型。

helper 的定义在内核的 `kernel/bpf/helpers.c`、`net/core/filter.c` 等文件里，用 `BPF_CALL_x` 宏声明：

```c
const struct bpf_func_proto bpf_map_lookup_elem_proto = {
	.func		= bpf_map_lookup_elem,
	.gpl_only	= false,
	.ret_type	= RET_PTR_TO_MAP_VALUE_OR_NULL,
	.arg1_type	= ARG_CONST_MAP_PTR,
	.arg2_type	= ARG_PTR_TO_MAP_KEY,
};
```

`ret_type` 和 `argN_type` 是校验器用的类型签名。`RET_PTR_TO_MAP_VALUE_OR_NULL` 意味着返回值可能是空指针，调用方必须先判空再解引用——这就是 5.3 里那句“键不存在返回 NULL”的来源。

### 6.2 常用的几组

| 组 | 函数 | 用途 |
| --- | --- | --- |
| map | `bpf_map_lookup_elem` / `update` / `delete` | 读写 map |
| 报文 | `bpf_skb_load_bytes` / `bpf_skb_pull_data` / `bpf_xdp_adjust_head` | 读写、移动报文数据 |
| 时间 | `bpf_ktime_get_ns` / `bpf_jiffies64` | 取时间戳 |
| 输出 | `bpf_trace_printk` / `bpf_printk` | 写 trace_pipe，仅调试用 |
| 环形缓冲 | `bpf_ringbuf_output` / `bpf_ringbuf_reserve` | 往用户态送事件 |
| socket | `bpf_sock_hash_update` / `bpf_msg_redirect_hash` / `bpf_sk_redirect_hash` | socket 层操作 |
| 校验和 | `bpf_csum_diff` / `bpf_l4_csum_replace` | 改包后重算校验和 |

`bpf_printk` 是宏，展开成 `bpf_trace_printk`，最多三个参数，输出到 `/sys/kernel/debug/tracing/trace_pipe`。它每次调用都要格式化字符串，成本高，只适合调试；生产环境用 ringbuf。

`sock_ops` 实验里用它确认回调确实触发：

```
python3-97974  [001] ...11 101453.427904: bpf_trace_printk: op=4 port=33516 ret=0
python3-97974  [001] ..s31 101453.427958: bpf_trace_printk: op=5 port=9091 ret=0
```

`op=4` 是 `BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB`（主动连接建立），`op=5` 是 `BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB`（被动建立），`ret=0` 是 `bpf_sock_hash_update` 的返回值。

### 6.3 可用性由程序类型决定

同一个 helper 编号，在 XDP 程序里可能可用，在 socket filter 里可能不可用。原因是每种程序类型注册了自己的 `get_func_proto` 回调，按编号逐个放行。加载时报：

```
unknown func bpf_msg_redirect_hash#59
```

说明这个 helper 不在当前程序类型的白名单里。这时要么换程序类型，要么换实现方式。`bpftool feature probe` 可以列出当前内核支持的 helper 和程序类型组合。

### 6.4 helper 与 kfunc

新内核引入了 kfunc：内核里某些函数被打上 `__bpf_kfunc` 标记，eBPF 程序可以直接调用，不需要包一层 helper。kfunc 用 BTF ID 标识，调用前要在程序里用 `bpf_kfunc_call` 声明原型，加载器负责解析。它比 helper 灵活（能传结构体指针），但可用性更依赖内核版本。这里不展开，知道有这么个东西即可。

## 七、开发工具链

### 7.1 编译

```bash
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. \
      -Wno-missing-declarations -c prog.bpf.c -o prog.bpf.o
```

- `-target bpf` 生成 eBPF 字节码，不是宿主机指令。
- `-g` 保留调试信息，btf 段靠它生成。
- `-D__TARGET_ARCH_arm64` 让 `vmlinux.h` 里的架构相关类型（如 `pt_regs`）对上。x86 上换成 `x86`。
- `-O2` 必须开。`-O0` 生成的代码往往过不了校验器（栈使用超标、寄存器状态不收敛）。
- `-Wno-missing-declarations` 只是压掉 `vmlinux.h` 生成时的噪音，不影响正确性。

`vmlinux.h` 从当前内核导出，换内核要重新导，否则字段偏移可能对不上。CO-RE 能缓解这个问题，但不能完全消除（结构体被删字段时无法重定位）。

### 7.2 bpftool

`bpftool` 是内核源码 `tools/bpf/bpftool` 编出来的，几乎所有操作都能做：

```bash
bpftool prog list                      # 列出所有程序
bpftool prog show id 449 -f            # 详情，含 map_ids
bpftool prog dump xlated id 449        # 校验后的字节码
bpftool prog dump jited id 449         # JIT 后的机器码
bpftool prog load prog.bpf.o /sys/fs/bpf/prog
bpftool map list
bpftool map dump id 12
bpftool net list                       # 网卡上的 XDP/tc 挂载
bpftool cgroup tree                    # cgroup 上的挂载
bpftool btf dump file /sys/kernel/btf/vmlinux format c
bpftool feature probe                  # 内核支持哪些类型和 helper
```

`bpftool prog load` 只加载，不挂载。挂载要用对应挂载点的命令：XDP 用 `ip link set ... xdp obj`，tc 用 `tc filter add ... bpf da`，cgroup 用 `bpftool cgroup attach` 或加载器里的 `bpf_prog_attach`。

### 7.3 libbpf 与 CO-RE

手写 `bpf()` 调用能跑，但 map 重定位、license 检查、BTF 处理、attach 类型推断这些琐事太多。libbpf 是内核源码 `tools/lib/bpf` 编出来的用户态库，把这些都包了：

```c
struct bpf_object *obj;
struct bpf_program *prog;
struct bpf_link *link;

obj = bpf_object__open_file("prog.bpf.o", NULL);
bpf_object__load(obj);
prog = bpf_object__find_program_by_name(obj, "my_prog");
link = bpf_program__attach(prog);	/* 按 SEC 推断挂载方式 */
```

CO-RE（Compile Once – Run Everywhere）是 libbpf 的一个特性：程序里访问内核结构体字段时，不写死偏移，而是记录“字段名 + 所属类型”，加载时由 libbpf 读目标内核的 BTF，把偏移重定位成正确的值。源码里用 `BPF_CORE_READ` 或 `bpf_core_read.h` 的宏：

```c
__u32 pid = BPF_CORE_READ(task, pid);
```

它展开成 `bpf_probe_read_kernel` 加一段重定位信息。这样同一份 .bpf.o 能在 5.10 到 7.0 的内核上跑，只要字段还在。

### 7.4 调试

三个层次：

- **编译期**。`clang -Wall -Wextra` 抓类型错误。`vmlinux.h` 缺宏的问题（`ETH_P_IP`、`TC_ACT_OK` 都不在里面）在编译期报未定义，自己 `#define`。
- **加载期**。`bpf_object__load` 失败时，把 log level 开到 2，能看到完整 verifier log。`bpftool prog load` 默认就打印。
- **运行期**。`bpf_printk` 写 `trace_pipe`；ringbuf 送事件到用户态；`bpftool prog show` 看 `run_time_ns`、`run_cnt` 判断有没有被调用。

`trace_pipe` 需要 root 读，普通用户 `cat` 会得到 `Permission denied`。实验里一开始没注意，`grep` 出来是空的，误以为程序没触发，加 `sudo` 后才看到 `bpf_trace_printk` 的行。

## 八、挂载点

### 8.1 为什么选这四个

eBPF 的程序类型有三十多种，对应的挂载点更多。全讲一遍会变成清单，讲了也记不住。这一章选四个位置，理由是它们串起来正好是内核网络路径的一条线，而且每一站能拿到的数据和能做的事都不一样：

| 位置 | 时机 | 手上是什么 | 能做什么 |
| --- | --- | --- | --- |
| XDP | 驱动收包后、协议栈之前 | 裸以太网帧 | 丢、改、转回网卡 |
| tc | qdisc 层 | `sk_buff` | 丢、改、重定向到别的设备 |
| cgroup/connect4 | `connect()` 进协议栈之前 | socket 地址元数据 | 放行或拒绝，改目标地址 |
| sockmap | TCP 建立后、收发路径 | socket 与字节流 | 换 socket、丢数据段 |

![四个挂载点在内核收发路径上的位置](/p/linux-ebpf-programming/hooks.svg)

越靠前，能拦下的东西越多、成本越低，但知道的越少：XDP 只看得到帧，不知道它属于哪个进程、哪个 socket。越靠后，上下文越丰富，但已经进了协议栈，前面花掉的 CPU 收不回来。四个位置刚好把这组权衡摊开。

选它们还有一个现实理由：这是 Cilium 的主干。XDP 做快速丢弃和 DDoS 防护，tc 做 L3/L4 策略与 NAT，cgroup/connect4 做出站准入和透明代理，sockmap 做同节点 Pod 之间的 socket 层转发。看懂这四个，读 Cilium 的 datapath 不会卡在“这段代码挂在哪”。

没有展开的三类：kprobe/tracepoint 是观测点，只读不改，接口走 `perf_event_open`，写法与数据路径程序差别很大；LSM 是安全钩子，返回值直接决定系统调用是否继续；struct_ops 用 BPF 实现内核结构体的函数指针（比如拥塞控制算法），本质是“用 BPF 写内核模块”。它们属于另一个主题，这里只在 8.6 交代差别。

### 8.2 XDP

**位置**。网卡驱动收到包、分配完 `xdp_buff`、还没进协议栈。这时没有 `sk_buff`，只有一段连续的帧数据。

**上下文**是 `struct xdp_md`：

```c
struct xdp_md {
	__u32 data;		/* 帧起始，相对 ctx 的偏移 */
	__u32 data_end;		/* 帧结束 */
	__u32 data_meta;
	__u32 ingress_ifindex;
	__u32 rx_queue_index;
	__u32 egress_ifindex;
};
```

`data` 和 `data_end` 是 32 位偏移，程序里用 `(void *)(long)ctx->data` 转成指针。

**返回值**决定这个包的命运：

| 值 | 名字 | 含义 |
| --- | --- | --- |
| 0 | `XDP_ABORTED` | 丢弃，并记一次错误 |
| 1 | `XDP_DROP` | 丢弃，正常行为 |
| 2 | `XDP_PASS` | 交给协议栈 |
| 3 | `XDP_TX` | 从同一网卡发回去 |
| 4 | `XDP_REDIRECT` | 转到另一网卡或 CPU（配合 `bpf_redirect_map`） |

**三种运行模式**：

- native：驱动直接调用，包还在 DMA 缓冲里，零拷贝。需要驱动支持，`veth` 支持。
- generic：在协议栈入口用 `skb` 模拟，功能等价但多一次拷贝，任何网卡都能用。
- offload：编译进网卡固件，主机 CPU 完全不参与，需要特定网卡。

挂载脚本先试 native，失败再退 generic：

```bash
if ip link set dev veth2 xdp obj xdp_ping.bpf.o sec xdp 2>/dev/null; then
	echo "native"
else
	ip link set dev veth2 xdpgeneric obj xdp_ping.bpf.o sec xdp
	echo "generic"
fi
```

`ip -d link show veth2` 的输出里能看到 `prog/xdp id 449 name xdp_ping ... jited`，`jited` 说明走的是 JIT 后的机器码。

**能力边界**：

- 能读整个帧，能 `bpf_xdp_adjust_head` 在帧头前加减空间（做隧道封装），能 `bpf_xdp_adjust_tail` 截断。
- 不能调用任何依赖 `sk_buff` 的 helper，比如 `bpf_skb_load_bytes`、`bpf_skb_store_bytes`。
- 看不到 socket、进程、conntrack，因为它跑在协议栈之前，这些对象还不存在或还没关联。

**实验**。`xdp_ping.bpf.c` 挂在 `veth2` 的入向，`ns1` ping `ns2`，三个 echo request 全被丢，`stats[0]` 从 0 变 3。这个程序同时演示了两件事：每包路径上的计数要用 map，丢包只用改返回值。

### 8.3 tc

**位置**。qdisc 层，有 ingress 和 egress 两个方向。`clsact` 是一个只有分类器、没有队列的 qdisc，专门给 eBPF 用。

**上下文**是 `struct __sk_buff`，字段比 `xdp_md` 多得多：

```c
struct __sk_buff {
	__u32 len;
	__u32 mark;		/* skb->mark，可读写 */
	__u32 priority;
	__u32 ingress_ifindex;
	__u32 ifindex;
	__u32 cb[5];		/* 控制块，可用来在程序间传值 */
	...
};
```

`mark` 和 `cb` 是关键：上一个程序（比如 XDP 或另一个 tc 程序）可以写 `mark`，下一个程序读它，用来传递决策。

**返回值**：

| 值 | 名字 | 含义 |
| --- | --- | --- |
| 0 | `TC_ACT_OK` | 放行 |
| 2 | `TC_ACT_SHOT` | 丢弃 |
| 3 | `TC_ACT_PIPE` | 交给下一个分类器 |
| 7 | `TC_ACT_REDIRECT` | 重定向（配合 `bpf_redirect`） |

挂载时必须加 `da`（direct-action），否则返回值被解释成 classid，需要再配一个 class 才能生效。

```bash
tc qdisc add dev veth2 clsact
tc filter add dev veth2 egress bpf da obj tc_ping.bpf.o sec tc
tc filter show dev veth2 egress
```

`tc filter show` 的输出里能看到 `direct-action not_in_hw id 467 name tc_ping ... jited`。`not_in_hw` 说明程序跑在主机 CPU 上，没有卸载到网卡。

**与 XDP 的差别**：

- 晚。包已经过驱动、可能已经过分片重组，`sk_buff` 已分配。
- 信息多。能看 `mark`、`priority`、`skb->cb`，能做 DNAT/SNAT（改 `iph->daddr` 后要重算校验和）。
- 能重定向到另一台设备（`bpf_redirect(ifindex, 0)`），XDP 也能但限制更多。
- 代价高。每包一次 `sk_buff` 分配，还有 qdisc 锁。

**实验**。`tc_ping.bpf.c` 挂在 `veth2` 的 egress，统计并丢弃 ICMP echo request。`ns2` ping `ns1` 的结果是 100% 丢包，`stats[0]` 是 4——比 XDP 那次多一个，因为 egress 方向还经过了一次 ICMP 差错报文或重传。丢包时本机 `ping` 会打印：

```
ping: sendmsg: No buffer space available
```

这不是内存不足，是 egress 丢包后本机 socket 的错误表现。

### 8.4 cgroup/connect4

**位置**。进程调 `connect()`，地址已经确定、还没进协议栈。程序挂在一个 cgroup 上，只对该 cgroup 内的进程生效，子 cgroup 继承。

**上下文**是 `struct bpf_sock_addr`：

```c
struct bpf_sock_addr {
	__u32 user_family;
	__u32 user_ip4;		/* 用户态传的地址，主机字节序 */
	__u32 user_ip6[4];
	__u32 user_port;	/* 主机字节序 */
	__u32 family;
	__u32 type;
	__u32 protocol;
	__u32 msg_src_ip4;
	...
};
```

`user_` 前缀表示“用户态原本要连的地址”。程序可以改它，把连接导向本地代理——service mesh 的透明劫持就是这么做的。

**返回值**只有两种：

- `1`：放行。
- `0`：拒绝，`connect()` 返回 `EPERM`。

**实验**。`cgroup_connect4.bpf.c` 拒绝连向 8080 的连接：

```c
SEC("cgroup/connect4")
int connect4_block(struct bpf_sock_addr *ctx)
{
	if (ctx->user_port == bpf_htons(8080))
		return 0;
	return 1;
}
```

`user_port` 是主机字节序，所以要拿 `bpf_htons(8080)` 比。未挂载时 `ns1` 直连 `10.0.0.2:8080` 成功，服务端打印 `server accepted`；挂到 `/sys/fs/cgroup/ebpf-demo` 后，同一个 cgroup 内的进程再连：

```
connect failed: [Errno 1] Operation not permitted
```

**两个坑**：

- `ip netns exec` 会 unshare mount namespace 并 remount `/sys`，netns 里看不到 host 的 cgroup2 挂载，写 `cgroup.procs` 会报 `Directory nonexistent`。正确做法是在 host 侧先写 `cgroup.procs`，再 `exec ip netns exec`，靠 fork 继承 cgroup 归属：

```bash
sh -c 'echo $$ > /sys/fs/cgroup/ebpf-demo/cgroup.procs; exec ip netns exec ns1 python3 client.py'
```

- 服务端 `accept()` 没有超时。连接被拒绝时，服务端永远等不到，脚本里的 `wait` 会挂死。实验脚本给 server 套了 `timeout 5`。

### 8.5 sockmap：sock_ops、sk_msg、sk_skb

前面三个位置都在协议栈的“包”这一层。sockmap 换了维度：它管的是 **socket**，数据以字节流为单位在 socket 之间搬。

一张 `SOCKMAP`（按下标存）或 `SOCKHASH`（按任意键存，通常用四元组）里放的是 socket 引用。三种程序围着这张 map 工作：

| 程序类型 | 触发点 | 典型动作 |
| --- | --- | --- |
| `sock_ops` | TCP 状态变化（建立、关闭、重传） | `bpf_sock_hash_update` 把 socket 放进 map |
| `sk_msg` | `sendmsg` 路径 | `bpf_msg_redirect_hash` 把数据直接投进另一个 socket 的接收队列 |
| `sk_skb/stream_verdict` | 入方向数据到达 | 返回 `SK_PASS` 或 `SK_DROP`，可改包 |

![sock_ops 把两端 socket 登记进 sockmap，sk_msg 与 sk_skb 分别处理出方向和入方向](/p/linux-ebpf-programming/sockmap-path.svg)


`sock_ops` 的写法：

```c
SEC("sockops")
int sockops_track(struct bpf_sock_ops *skops)
{
	__u32 key = skops->local_port;

	switch (skops->op) {
	case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:
	case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:
		bpf_sock_hash_update(skops, &sock_hash, &key, BPF_NOEXIST);
		break;
	}
	return 1;
}
```

`skops->op` 的取值里，`4` 是主动建立完成，`5` 是被动建立完成。`bpf_sock_hash_update` 返回 0 表示插入成功。挂载走 cgroup：

```bash
bpftool prog load sockops_track.bpf.o /sys/fs/bpf/sockops
bpftool cgroup attach /sys/fs/cgroup/ebpf-demo sock_ops pinned /sys/fs/bpf/sockops
```

`sock_ops` 的返回值语义与 connect4 不同：返回 `1` 表示“程序看过这条连接”，不是“放行/拒绝”的开关。

**实验 A：socket 进 map**。cgroup 内起一个客户端连本地服务端，`bpf_printk` 看到两次回调：

```
bpf_trace_printk: op=4 port=33516 ret=0
bpf_trace_printk: op=5 port=9091 ret=0
```

`ret=0` 是 `bpf_sock_hash_update` 的返回值。但 `bpftool map dump name sock_hash` 显示 `Found 0 elements`——5.5 节说过，这类 map 不提供遍历。在程序里加一个计数 map，`stats[0]` 从 0 变成 1，才能确认插入真的发生了。

**实验 B：入方向丢弃**。`sk_skb/stream_verdict` 程序挂到 map 上，`bpftool` 不支持这个 attach，要写一个几十行的 C 加载器：

```c
map_fd = bpf_object__find_map_fd_by_name(obj, "sock_map");
prog = bpf_object__find_program_by_name(obj, "sk_skb_verdict");
bpf_prog_attach(bpf_program__fd(prog), map_fd, BPF_SK_SKB_STREAM_VERDICT, 0);

/* 服务端 accept 后把连接 fd 插进 map，value 直接传 fd */
bpf_map_update_elem(map_fd, &key, &cs, BPF_ANY);
```

第一版程序直接读 `skb->data`：

```c
char *data = (void *)(long)skb->data;
char *data_end = (void *)(long)skb->data_end;
if (data + 5 > data_end)
	return SK_PASS;
```

结果 `bpf_printk` 打印 `verdict len=5 seg=0`（这一轮的完整输出在 `ebpf-sk-skb-before.txt`）：`skb->len` 是 5，但线性段长度是 0。数据不在 `skb->data` 指向的地方，边界检查直接判“太短”，程序返回 `SK_PASS`，服务端照常收到：

```
server recv 5 bytes: HELLO
```

改成先拉数据：

```c
if (bpf_skb_pull_data(skb, 5) < 0)
	return SK_PASS;
data = (void *)(long)skb->data;
data_end = (void *)(long)skb->data_end;
```

再跑，服务端收到的是 EOF（输出在 `ebpf-sk-skb.txt`）：

```
server recv: EOF
```

`SK_DROP` 不是“丢一个包”。这条流在 socket 层被终止，上层 `recv` 看到的是连接结束。

**sk_msg 的写法**。出方向的转发，代码比 verdict 短：

```c
SEC("sk_msg")
int sk_msg_redirect(struct sk_msg_md *msg)
{
	__u32 key = target_key(msg);

	return bpf_msg_redirect_hash(msg, &sock_hash, &key, BPF_F_INGRESS);
}
```

`BPF_F_INGRESS` 表示把数据投进目标 socket 的接收队列，而不是发送队列。它要求目标 socket 已经在 map 里，且 key 能对上——这就是为什么 `sock_ops` 要先跑，把两端 socket 都登记进去。Cilium 的同节点 Pod 互访就走这条路径：源 socket 的数据不经过 TCP/IP 协议栈，直接出现在目的 socket 的接收队列里。

这一段没有配实验：要跑通需要同时控制两端 socket、key 映射和 attach 顺序，代码量是 verdict 的三倍。写在这里是为了让读者知道 `sk_msg` 的接口形状，以及它与 `sk_skb` 的分工——一个管出，一个管入。

### 8.6 没有展开的挂载点

| 类型 | 挂载方式 | 与前面四个的差别 |
| --- | --- | --- |
| kprobe / kretprobe | `perf_event_open` 或 `BPF_LINK_CREATE` | 函数入口/返回，上下文是 `pt_regs`，参数要 `bpf_probe_read_kernel` 或 CO-RE 读 |
| tracepoint | 同上，挂到 tracefs 的静态点 | 参数布局固定，比 kprobe 稳定，但只能挂已有的点 |
| perf_event | 采样，按频率触发 | 不是每事件触发，适合 profiling |
| LSM | `BPF_LSM_MAC` | 安全钩子，返回 0 拒绝操作，权限要求最高 |
| struct_ops | `BPF_MAP_TYPE_STRUCT_OPS` | 用 BPF 实现内核结构体的函数指针，如拥塞控制 |

它们的共同点是：不处理“每个包”，而是处理“每次调用/每个事件”，写法与数据路径程序的分歧大于共性。观测类程序在下一篇单独讲。

## 九、命令与接口速查

### 9.1 工具链

```bash
# 导出当前内核的类型定义
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# 编译
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. \
      -Wno-missing-declarations -c prog.bpf.c -o prog.bpf.o

# 加载（只加载，不挂载）
bpftool prog load prog.bpf.o /sys/fs/bpf/prog
bpftool prog loadall prog.bpf.o /sys/fs/bpf/dir    # 带 map 一起 pin

# 查看
bpftool prog list
bpftool prog show id N -f
bpftool prog dump xlated id N
bpftool prog dump jited id N
bpftool map list
bpftool map dump id N
bpftool map dump name NAME
bpftool map lookup id N key 0
bpftool net list
bpftool cgroup tree
bpftool feature probe
```

### 9.2 挂载与卸载

```bash
# XDP
ip link set dev eth0 xdp obj prog.bpf.o sec xdp            # native
ip link set dev eth0 xdpgeneric obj prog.bpf.o sec xdp     # generic
ip link set dev eth0 xdp off

# tc
tc qdisc add dev eth0 clsact
tc filter add dev eth0 egress bpf da obj prog.bpf.o sec tc
tc filter del dev eth0 egress
tc qdisc del dev eth0 clsact

# cgroup
bpftool cgroup attach /sys/fs/cgroup/DIR connect4 pinned /sys/fs/bpf/prog
bpftool cgroup detach /sys/fs/cgroup/DIR connect4 pinned /sys/fs/bpf/prog

# sockmap（verdict 挂到 map，需 C 加载器）
bpf_prog_attach(prog_fd, map_fd, BPF_SK_SKB_STREAM_VERDICT, 0);
```

### 9.3 返回值

| 位置 | 值 | 含义 |
| --- | --- | --- |
| XDP | 0 / 1 / 2 / 3 / 4 | ABORTED / DROP / PASS / TX / REDIRECT |
| tc | 0 / 2 / 3 / 7 | OK / SHOT / PIPE / REDIRECT |
| cgroup/connect4 | 1 / 0 | 放行 / 拒绝（EPERM） |
| sk_skb verdict | 1 / 0 | SK_PASS / SK_DROP |
| sk_msg | 1 / 0 | SK_PASS / SK_DROP |
| sock_ops | 1 | 程序已处理（非开关） |

### 9.4 常用 map 操作

| 函数 | 返回 | 注意 |
| --- | --- | --- |
| `bpf_map_lookup_elem` | 值指针或 NULL | 指针只在本次执行内有效 |
| `bpf_map_update_elem` | 0 / -1 | flags：`BPF_ANY` / `BPF_NOEXIST` / `BPF_EXIST` |
| `bpf_map_delete_elem` | 0 / -1 | 键不存在返回 `-ENOENT` |
| `bpf_sock_hash_update` | 0 / 负错误码 | socket 必须已建立 |
| `bpf_msg_redirect_hash` | 0 / 负错误码 | 目标 socket 必须在同一 map |

### 9.5 调试

```bash
# 程序内的 bpf_printk 输出
cat /sys/kernel/debug/tracing/trace_pipe        # 需要 root

# 确认程序被调用
bpftool prog show id N -f | grep -E 'run_cnt|run_time_ns'

# 完整 verifier 日志
bpftool prog load prog.bpf.o /sys/fs/bpf/p 2>&1 | head -40
```

## 十、名词解释

- **eBPF**：内核里的一个受限虚拟机，程序经校验后 JIT 成机器码，挂到事件点上执行。
- **校验器（verifier）**：加载时逐指令做符号执行，证明内存访问在界内、循环有界、指针来源合法。
- **JIT**：把 eBPF 字节码翻译成宿主机机器码，`bpftool prog show` 的 `jited` 字段非零即生效。
- **BTF**：内核类型信息，暴露在 `/sys/kernel/btf/vmlinux`，是 CO-RE 和 `vmlinux.h` 的来源。
- **CO-RE**：编译一次、多内核运行，靠加载时按目标内核 BTF 重定位结构体字段偏移。
- **map**：程序与用户态共享的键值存储，类型决定并发语义与访问方式。
- **helper**：内核提供给 eBPF 调用的白名单函数，按编号调用，可用性由程序类型决定。
- **kfunc**：带 `__bpf_kfunc` 标记的内核函数，可被 eBPF 直接调用，用 BTF ID 标识。
- **link**：prog 与挂载点的绑定对象，fd 关闭即卸载。
- **pin / bpffs**：把对象固定到 `/sys/fs/bpf` 下的路径，进程退出后仍存在。
- **XDP**：驱动收包后、协议栈之前的挂载点，上下文是裸帧，零拷贝。
- **clsact / tc**：qdisc 层的挂载点，上下文是 `sk_buff`，能做 NAT 和重定向。
- **cgroup/connect4**：`connect()` 进协议栈前的挂载点，可放行、拒绝或改写目标地址。
- **sockmap / sockhash**：存放 socket 引用的 map，前者按下标，后者按任意键。
- **sock_ops**：TCP 状态变化时触发的程序，用来把 socket 登记进 sockmap。
- **sk_msg**：`sendmsg` 路径上的程序，把数据重定向到另一个 socket。
- **sk_skb**：socket 层收包路径上的程序，`stream_verdict` 决定 `SK_PASS` 还是 `SK_DROP`。
- **SK_PASS / SK_DROP**：socket 层程序的放行/丢弃返回值，`SK_DROP` 会让上层看到连接结束。
