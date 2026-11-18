---
title: "Linux 文件系统（七）：用 /proc 查看进程和系统状态"
slug: linux-proc-filesystem
date: "2026-11-18T13:08:27+08:00"
draft: false
description: "从一个打开了两个文件的进程出发，查看文件描述符、环境变量和常用系统状态。"
categories:
    - Linux 与网络
tags:
    - Linux
    - 文件系统
    - procfs
---

想知道一个进程打开了哪些文件、从哪个目录运行，或者启动时带了哪些环境变量，可以从 `/proc/<pid>/` 查起。第一篇用 `findmnt` 确认了 `/proc` 是一个单独的挂载，第三篇也用 `fdinfo` 看过文件偏移。这次启动一个容易辨认的进程，把这些入口放在一起看。 [目录树与挂载](/p/linux-root-filesystem/) · [文件描述符与 fdinfo](/p/linux-file-object-model/)

从使用者的角度看，**procfs 是一个通过回调提供内容的虚拟文件系统**。程序仍然可以用熟悉的 `cat`、`ls` 和 `readlink`。VFS 把操作交给对应入口的实现。例如，读取 `/proc/<pid>/status` 时，内核查询这个进程并把状态组织成文本。查看 `/proc/<pid>/fd/3` 时，内核根据这个进程的描述符表解析它指向的目标。不同入口各有自己的行为。 [Linux v6.8 的 procfs 文档](https://docs.kernel.org/6.8/filesystems/proc.html)

## 一、启动进程，查看它打开的文件

在 Ubuntu 虚拟机的 Bash 中执行下面的命令。先准备两个临时文件，再启动一个后台进程，用 fd 3 和 fd 4 分别以只读方式打开它们。随后用 `exec sleep 300` 让同一个 PID 保持运行，两个描述符也随这次 `exec` 保留下来。

```bash
lab=$(mktemp -d)
printf 'hello procfs\n' > "$lab/note.txt"
printf 'mode=demo\n' > "$lab/config.txt"

PROC_DEMO=training bash -c 'exec 3< "$1/note.txt" 4< "$1/config.txt"; exec sleep 300' bash "$lab" &
pid=$!
```

`$!` 是刚启动的后台进程 PID。等 `cat "/proc/$pid/comm"` 显示 `sleep`，就可以查看两个描述符了。

```bash
readlink "/proc/$pid/fd/3"
readlink "/proc/$pid/fd/4"
cat "/proc/$pid/fd/3" "/proc/$pid/fd/4"
```

虚拟机上的一次实测输出如下，临时目录名每次会变化。

```text
/tmp/tmp.ja4cUehw1g/note.txt
/tmp/tmp.ja4cUehw1g/config.txt
hello procfs
mode=demo
```

`readlink` 显示描述符关联的目标。`cat` 则打开这两个 procfs 入口来读取背后的普通文件。在本例中，它从自己的打开位置读取，目标进程的 `f_pos` 保持独立。想看目标进程自己的偏移和打开标志，可以读取 `/proc/$pid/fdinfo/3`。实际排查时，描述符也可能指向管道或 socket，访问还要通过相应的权限检查。 [procfs 的进程条目](https://docs.kernel.org/6.8/filesystems/proc.html)

## 二、查看同一个进程的环境与状态

刚才启动进程时设置了 `PROC_DEMO=training`。环境变量在 `environ` 里用 NUL 字节分隔，可以先转换成换行，再只取实验变量。

```bash
tr '\0' '\n' < "/proc/$pid/environ" | grep '^PROC_DEMO='
grep -E '^(Name|State|Pid|PPid|Threads):' "/proc/$pid/status"
```

对应的实测输出如下。

```text
PROC_DEMO=training
Name:	sleep
State:	S (sleeping)
Pid:	1532470
PPid:	1532468
Threads:	1
```

`environ` 展示的是当前程序启动时的环境变量。程序后来添加或更改的变量未必能在这里看到。实际进程的环境可能包含凭据，排查时可以像这个例子一样只筛选需要的键。 [Linux procfs 文档](https://docs.kernel.org/6.8/filesystems/proc.html)

| 路径 | 可以查什么 |
| --- | --- |
| `/proc/<pid>/status` | 进程名、状态、父 PID、线程数、UID 和 GID 等。 |
| `/proc/<pid>/cmdline` | 命令行参数，通常以 NUL 字节分隔。 |
| `/proc/<pid>/cwd`、`exe` | 工作目录和可执行文件，可用 `readlink` 查看。 |
| `/proc/<pid>/fd/`、`fdinfo/` | 已打开的描述符及各自的状态。 |
| `/proc/<pid>/maps` | 进程的虚拟地址范围及其映射对象。 |
| `/proc/<pid>/mountinfo` | 该进程看到的挂载。 |

### 2.1 cmdline、cwd 和 exe 怎么一起看？

沿用同一个 `$pid`。把 `cmdline` 中的 NUL 分隔符换成换行，就能逐行看到参数。另两个入口可以用 `readlink` 查看。

```bash
tr '\0' '\n' < "/proc/$pid/cmdline"
readlink "/proc/$pid/cwd"
readlink "/proc/$pid/exe"
```

虚拟机上的实测输出如下。后两行依次是进程的工作目录和可执行文件路径，按实际环境会不同。

```text
sleep
300
/home/lxj
/usr/lib/cargo/bin/coreutils/sleep
```

`cmdline` 告诉我们程序带了什么参数，`cwd` 告诉我们它从哪个工作目录处理相对路径，`exe` 则给出正在执行的程序文件。这三个入口放在一起，能看出同名进程具体运行的是哪个程序、带了什么参数。

### 2.2 读 fd 入口会推进目标进程的偏移吗？

对本例的普通文件，可以在读取前后各看一次目标进程的 `fdinfo`。

```bash
grep '^pos:' "/proc/$pid/fdinfo/3"
cat "/proc/$pid/fd/3"
grep '^pos:' "/proc/$pid/fdinfo/3"
```

实测两次 `pos` 都是 `0`，中间的 `cat` 读到了 `hello procfs`。这里 `cat` 通过 procfs 入口另行打开了这个普通文件，读的是自己的打开位置。它的行为可以和第三篇的 `dup` 实验对照，那里两个 fd 引用同一个打开对象，读取才会推进共享偏移。 [第三篇的偏移实验](/p/linux-file-object-model/)

### 2.3 /proc/self 指的是谁？

`/proc/self` 会解析为访问它的进程。比如 `cat /proc/self/status` 看到的是 `cat` 自己的状态。要查当前 Bash，可以用 `/proc/$$/status`。要查本实验的后台进程，就沿用 `$pid`。

检查完可以结束后台进程并清理临时文件。

```bash
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f -- "$lab/note.txt" "$lab/config.txt"
rmdir -- "$lab"
```

## 三、从 /proc 根目录查看系统状态

前面的路径先选定一个 PID。`/proc` 根目录还提供不必指定 PID 的入口。按要查的问题选文件即可。

| 想查什么 | 命令 | 先看哪里 |
| --- | --- | --- |
| 内存还有多少可用 | `head /proc/meminfo` | `MemTotal`、`MemAvailable`。 |
| 系统最近的负载 | `cat /proc/loadavg` | 开头三个数是 1、5、15 分钟平均负载。 |
| CPU 时间累计到了哪里 | `head /proc/stat` | `cpu` 是汇总行，`cpu0` 等是各 CPU 的累计值。 |
| 当前运行的内核 | `cat /proc/version` | 内核版本及构建信息。 |
| 内核启动时接到哪些参数 | `cat /proc/cmdline` | 这一行是启动参数。 |
| 当前视角有哪些挂载 | `findmnt` | 需要查看原始字段时，可以读 `/proc/self/mountinfo`。 |

例如，要看某个服务目前打开了什么，先找 PID，再查 `fd`。要看整台机器的内存状况，就直接查 `meminfo`。两个入口都在 `/proc` 下，关注的是不同范围的状态。 [Linux v6.8 的 procfs 系统信息文档](https://docs.kernel.org/6.8/filesystems/proc.html)

## 四、命令速查

| 命令 | 用途 |
| --- | --- |
| `readlink "/proc/$pid/fd/3"` | 查看目标进程的 fd 3 关联什么。 |
| `cat "/proc/$pid/fd/3"` | 在本例中，通过 procfs 入口打开并读取背后的普通文件。 |
| `tr '\0' '\n' < /proc/…/environ` | 把 NUL 分隔的环境变量转换成逐行显示。 |
| `grep '^pos:' "/proc/$pid/fdinfo/3"` | 查看目标进程中 fd 3 的当前偏移。 |
| `readlink "/proc/$pid/cwd"` | 查看目标进程的当前工作目录。 |
| `head /proc/meminfo` | 查看系统内存统计。 |

## 五、名词解释

| 术语 | 含义 |
| --- | --- |
| `procfs` | 通常挂在 /proc，通过文件接口提供进程和系统状态的虚拟文件系统。 |
| `PID` | 进程标识符，用于定位 /proc 中的进程目录。 |
| `NUL` | 数值为 0 的字节，用于分隔 cmdline 的参数和 environ 的环境条目。 |
| `/proc/self` | 解析为访问这个路径时的当前进程。 |
| `fdinfo` | 按描述符提供偏移、打开标志等信息的入口。 |
