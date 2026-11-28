#!/bin/bash
cd /home/lxj/nslab/ebpf
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c bad_access.bpf.c -o bad_access.bpf.o
echo "== 编译完成"
bpftool prog load bad_access.bpf.o /sys/fs/bpf/bad type xdp 2>&1 | head -20
echo "== 加载结束 rc=$?"
ls /sys/fs/bpf/bad 2>/dev/null || echo "pin 未创建"
