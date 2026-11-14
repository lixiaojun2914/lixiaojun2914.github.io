---
title: "Ubuntu Server 虚拟机安装及配置"
date: "2026-11-13T12:00:00+08:00"
lastmod: "2026-11-14"
draft: false
description: "在 VMware Fusion 上安装 Ubuntu Server 24.04 LTS，配置 NAT 与仅主机双网卡，并验证静态 IP、代理和 SSH。"
slug: "ubuntu-server-vm-install"
categories:
    - Linux 与网络
tags:
    - Ubuntu
    - VMware
    - Linux
    - SSH
---

这篇文章记录在 VMware Fusion 中安装 Ubuntu Server 的过程，以及配套的双网卡配置：一块负责上网，另一块提供固定的 SSH 登录地址。同样的布局也能沿用到后面的多节点网络实验。

下面的步骤以 **Apple Silicon Mac + VMware Fusion + Ubuntu Server 24.04.5 ARM64** 为例。截图里的网卡名、IP 和磁盘容量都来自这套环境；你自己的机器上数值不同很正常，按文中的判断方法选，不必逐个照抄。

## 一、安装虚拟机软件

打开 [VMware 官方产品页](https://www.vmware.com/products/desktop-hypervisor/workstation-and-fusion)，macOS 选 **VMware Fusion**，Intel/AMD 架构的 Windows 电脑选 **VMware Workstation Pro**。

下载入口通常会转到 Broadcom Support Portal，登录后按页面提示下载。找不到入口时，可以参考 [Broadcom 下载说明](https://knowledge.broadcom.com/external/article/368734/downloading-vmware-desktop-hypervisor-products.html)。

装好并打开软件，能看到新建虚拟机的界面，就可以准备镜像了。

## 二、下载 Ubuntu Server 24.04 LTS 镜像

先确认宿主机的 CPU 架构，再决定下哪个 ISO：

| 宿主机 | 下载入口 | 文件名结尾 |
| --- | --- | --- |
| Apple Silicon Mac（M 系列） | [Ubuntu 官方 ARM64 镜像目录](https://cdimage.ubuntu.com/ubuntu/releases/24.04/release/) | `live-server-arm64.iso` |
| Intel Mac 或 Intel/AMD Windows 电脑 | [Ubuntu 官方 AMD64 镜像目录](https://releases.ubuntu.com/24.04/) | `live-server-amd64.iso` |

AMD64 就是 x86-64，Intel 的 64 位处理器同样适用。这里选 **Server** 镜像即可——整个实验都在命令行和 SSH 里完成，用不上桌面环境。

示例用的是 `ubuntu-24.04.5-live-server-arm64.iso`。想确认下载是否完整，可以在 Mac 上算一遍 SHA-256，再和官方目录里 `SHA256SUMS` 的同名条目对照：

```bash
shasum -a 256 ubuntu-24.04.5-live-server-arm64.iso
```

## 三、创建并安装虚拟机

### 1. 载入 ISO，给虚拟机分配资源

在 Fusion 新建虚拟机，选择从光盘或镜像安装，载入下载好的 ISO。Apple Silicon 上应该会识别为 **Ubuntu 64-bit Arm Server**；如果架构不对，回头检查 ISO 文件名。

![图 01：Fusion 选择 Ubuntu Server ARM64 ISO](01-select-iso.png)

创建前会显示配置摘要：

- **2 个虚拟 CPU、4 GB 内存**：截图中的配置足够完成基础安装。
- **20 GB 磁盘**：同样够用；若计划在这台机上缓存大量容器镜像，可以多留一些空间。

![图 02：虚拟机配置摘要：2 核、4 GB 内存、20 GB 磁盘](02-vm-summary.png)

### 2. 为上网和实验分别添加网卡

配两块网卡，是为了把两件事分开：虚拟机有独立的上网路径，Mac 又始终能用一个固定地址登进去。就算 NAT 那头出问题，只要实验网络还通，仍然可以 SSH 上去排查。

在 Fusion 的虚拟机设置里选择 **Add Device → Network Adapter**，然后给两个适配器分别指定：

- **Share with my Mac**：NAT，用 DHCP 拿到地址、默认路由和 DNS。
- **Private to my Mac**：仅主机网络，稍后为它配静态地址，供 Mac 登录和节点互通。

对照下面三张图完成操作：

![图 03：Fusion 虚拟机设置](03-vm-settings.png)

![图 04：通过 Add Device 添加 Network Adapter](04-add-network-adapter.png)

![图 05：Fusion 网络模式选项](05-network-modes.png)

图 05 只是帮助辨认选项，实际要分别确认两块网卡各自的设置。另外，安装器里的 **Create bond** 是用来聚合已有网卡的，新增虚拟硬件得在 Fusion 里做。

### 3. 启动 Ubuntu 安装器

启动虚拟机，在 GRUB 菜单选 **Try or Install Ubuntu Server**。进安装器后选语言、选与实际键盘对应的布局，安装类型取标准 **Ubuntu Server**。

![图 06：从 GRUB 启动 Ubuntu Server 安装器](06-grub-boot.png)

![图 07：选择安装器语言](07-installer-language.png)

![图 08：选择键盘布局](08-keyboard-layout.png)

![图 09：选择标准 Ubuntu Server 安装](09-install-type.png)

### 4. 识别网卡，再设置静态 IP

先让两块网卡都走 DHCP，看它们各自落在哪个网段。DHCP 只是分配地址的手段，仅主机网络同样可以提供 DHCP，所以别用 `enp2s0`、`enp10s0` 的数字大小去猜哪块是对外的。

![图 10：两块网卡通过 DHCP 获取的地址](10-network-dhcp.png)

截图里 NAT 网络是 `192.168.126.0/24`，落在其中的 `enp2s0` 保持 DHCP 不动；仅主机网络是 `192.168.190.0/24`，把临时拿到 `192.168.190.129` 的 `enp10s0` 改成固定地址。

选中仅主机那张网卡，进入 **Edit IPv4 → Manual**，填写：

![图 11：选择 Host-only 网卡的 Edit IPv4](11-edit-ipv4.png)

| 字段 | 示例值 |
| --- | --- |
| Subnet | `192.168.190.0/24` |
| Address | `192.168.190.10` |
| Gateway、Name servers、Search domains | 留空 |

![图 12：为实验网卡配置静态 IPv4](12-static-ipv4.png)

实际配置时，静态 IP 要和 Mac 的仅主机接口同网段，并且确认没被占用、不在 DHCP 地址池里。

本例中 Mac 的 `192.168.190.1` 与虚拟机的 `192.168.190.10` 同网段，可以直接通信，所以实验网卡不需要默认网关；访问互联网的默认路由和 DNS 仍由 NAT 网卡提供。

### 5. 配置软件源，按需使用代理

能直连软件源时，**Proxy address** 留空即可。需要用 Mac 的 HTTP 代理时，填虚拟机能到达的 Mac 地址，以及代理实际监听的端口。比如 NAT 网卡已拿到 `192.168.126.x`、Mac 在该网段的 HTTP 代理监听 `20809`，就填：

```text
http://192.168.126.1:20809/
```

还有几个容易填错的点：

- **代理要求认证**：格式是 `http://USERNAME:PASSWORD@192.168.126.1:20809/`，用户名或密码里的保留字符要做 URL 编码，例如 `@` 写成 `%40`。这类截图不要用带真实认证信息的。
- **代理地址写成 `127.0.0.1`**：在安装器里，它指的是 Ubuntu 自己；如果 Mac 上的代理也只监听回环地址，虚拟机访问不到。

**Mirror address** 填与架构匹配的仓库根地址。ARM64 走 Ubuntu Ports，例如 `https://cn.ports.ubuntu.com/ubuntu-ports/`，也可以换成 [中科大 Ubuntu Ports 镜像](https://mirrors.ustc.edu.cn/help/ubuntu-ports.html)。填好后等安装器把测试跑完。

![图 13：早期安装中镜像源未能通过检查](13-mirror-check-troubleshooting.png)

这张图是镜像检查没跑通的样子。遇到同样的提示，按顺序查三件事：

1. NAT 网卡有没有默认路由。
2. DNS 能不能解析镜像域名。
3. 代理地址是否可达。

> 换一个镜像并不能解决这三类问题。

### 6. 分区、创建用户并安装 SSH 服务

在 **Guided storage configuration** 里选中为这台虚拟机创建的 VMware 虚拟磁盘。示例用引导分区并开启了 LVM。确认磁盘名称、容量和分区摘要之后，再同意写入。

![图 14：在虚拟磁盘上使用引导分区与 LVM](14-guided-storage.png)

![图 15：确认根分区、boot 和 EFI 分区](15-storage-summary.png)

比较这两张图：磁盘总共 20 GB，根逻辑卷只有 10 GB。之后要查空间时，`lsblk` 看设备和分区，`df -h /` 看根文件系统的容量和用量。

接着建登录用户和主机名，示例用 `lxj` 和 `master`。

![图 16：设置主机名和登录用户](16-user-profile.png)

Ubuntu Pro 可以先跳过。

![图 17：暂时跳过 Ubuntu Pro](17-skip-ubuntu-pro.png)

到 **SSH configuration** 勾上 **Install OpenSSH server**，装完就能从 Mac 登录。

![图 18：勾选安装 OpenSSH Server](18-enable-openssh.png)

这套配置是允许密码登录的。以后想停掉密码认证，先在另一个终端确认公钥能登进去，再关。

### 7. 重启并验证

安装完成后重启。要是又进了安装菜单，在 Fusion 里把虚拟光驱上的 ISO 断开，再从虚拟磁盘启动。

![图 19：安装完成，准备重启](19-installation-complete.png)

![图 20：早期安装的首次控制台登录](20-first-console-login.png)

![图 21：调整网络后的控制台登录](21-console-login.png)

图 20 和图 21 来自不同的安装阶段，内核小版本不一样。实际版本以你自己机器的输出为准，别照抄截图：

```bash
cat /etc/os-release
uname -r
uname -m
ip -4 -brief addr
ip -4 route
```

需要确认四件事：

1. Ubuntu 是否为 24.04 LTS。
2. Apple Silicon 对应的客体架构是否为 `aarch64`。
3. 实验网卡有没有拿到设定的地址。
4. 默认路由是不是走 NAT 网卡。

然后在 **Mac 终端** 验证 SSH，用户名和 IP 换成你自己的：

```bash
ssh lxj@192.168.190.10
```

![图 22：从 Mac 通过静态地址登录 Ubuntu](22-verify-ssh.png)

能 SSH 进去就说明实验网络通了。再在 **Ubuntu 内** 跑 `curl -I --max-time 10 https://ubuntu.com/` 验证外网。如果代理只配在 APT 里，要另用 `sudo apt update` 验证，因为 curl 不读 APT 的代理配置。

## 为什么把上网和实验分到两块网卡？

单台虚拟机只接 NAT 也能上网，Mac 通常也能通过 NAT 虚拟网络里的地址连进去。这里加第二块网卡，主要是为了给多台实验机留一组固定的地址，并把登录路径和上网路径分开。这样，NAT 的地址或默认路由出了问题，只要仅主机网络和 SSH 服务还正常，就能从 Mac 登录排查。

### 先把 Mac 上的两个网桥对上号

| Fusion 网络 | Mac 上的网桥与地址 | Ubuntu 里的接口 | 用途 |
| --- | --- | --- | --- |
| `vmnet1` · Private to my Mac | `bridge100` · `192.168.190.1/24` | `enp10s0` · 静态 `192.168.190.10/24` | SSH、实验节点互通 |
| `vmnet8` · Share with my Mac | `bridge101` · `192.168.126.1/24` | `enp2s0` · DHCP，安装时取得 `192.168.126.135/24` | 访问外网，也可访问 Mac 的代理 |

这些是本例 Mac 的实际配置。可以在 **Mac 终端**用下面的命令核对；换一台 Mac 后，网桥编号、子网和接口名都可能不同：

```bash
ifconfig bridge100
ifconfig bridge101
grep -E 'VNET_[18]_(HOSTONLY_SUBNET|NAT)' '/Library/Preferences/VMware Fusion/networking'
```

![图 23：Mac 上查看 Fusion 虚拟网络子网](23-fusion-subnets.png)

`bridge100` 和 `bridge101` 各自把一组虚拟网卡端口接到同一个二层网络。相比之下，`192.168.190.1` 和 `192.168.126.1` 是 **Mac 自己在这两个网络里的地址**。它们可以作为 SSH 或代理连接的端点，但不能仅凭尾号是 `.1` 就认定它们是默认网关。Ubuntu 实际使用的网关以 `ip -4 route` 为准。

### 把三种访问路径分开看

| 操作 | 从哪里走 |
| --- | --- |
| Mac 用 SSH 登录 `192.168.190.10` | Mac → `bridge100` → Ubuntu 的 `enp10s0` |
| Ubuntu 不经 HTTP 代理访问外网 | `enp2s0` → DHCP 提供的默认网关 → Fusion NAT |
| Ubuntu 使用 `192.168.126.1:20809` 代理 | `enp2s0` → 同网段的 Mac 代理；再由代理连接目标站点 |

第一种连接里，Mac 和 Ubuntu 都在 `192.168.190.0/24`，通过 ARP 找到对方，再由网桥转发，所以实验网卡不需要填 Gateway。第三种连接同样先访问同网段的地址：Ubuntu 连接的是 Mac 上的代理端口，**这一步不需要经过默认网关**。换用 `192.168.190.1:20809` 也可以，但前提是代理监听在那个地址且虚拟机能连到它，此时连接代理的流量会改走实验网卡。

可以在 **Ubuntu 内**看内核会怎么选路：

```bash
ip route get 192.168.190.1   # 预期走实验网卡
ip route get 192.168.126.1   # 预期走 NAT 网卡，目标在直连子网
ip route get 1.1.1.1         # 预期经 NAT 网卡的默认网关
```

`ip route get` 只显示选路结果，不会测试对端是否可达。用 SSH 验证实验网络，用 `curl -I --noproxy '*' --max-time 10 https://ubuntu.com/` 验证不使用 curl 代理时能否访问外网，再用 `sudo apt update` 验证 APT 的软件源与代理配置。这样某一项失败时，就知道该查哪条路径。

出问题时，从报错判断该查哪一层：

| 报错 | 先检查 |
| --- | --- |
| `Network is unreachable` | 是否存在通往目标的路由 |
| `Connection refused` | 目标 IP 和端口是否正确，代理是否监听 |
| `407 Proxy Authentication Required` | 代理已响应，检查用户名、密码和 URL 编码 |

搭第二台虚拟机时，可以直接复用这套布局：各节点独立通过 NAT 上网，静态实验 IP 依次挑空闲的 `.10`、`.11`、`.12`。这样既不用把控制平面节点当作网关，后面搭 Kubernetes 时也能顺便确认节点有没有选中正确的实验网段。

### 可选：在 Mac 和虚拟机之间共享文件

要传实验文件，可以在 Fusion 的 **Sharing** 里添加共享目录，并确认客体里的 VMware Tools 已经跑起来。

这属于文件共享设置，不是双网卡或 SSH 生效的前提。

![图 24：Fusion 中的共享目录设置](24-optional-shared-folder.png)
