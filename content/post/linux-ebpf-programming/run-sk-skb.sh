#!/bin/bash
set -e
cd /home/lxj/nslab/ebpf
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c sk_skb_demo.bpf.c -o sk_skb_demo.bpf.o
gcc -O2 -o sk_skb_loader sk_skb_loader.c -lbpf -lelf -lz
echo "== 编译完成"

rm -f /tmp/trace_sk.txt
timeout 8 cat /sys/kernel/debug/tracing/trace_pipe > /tmp/trace_sk.txt 2>/dev/null &
sleep 0.3

./sk_skb_loader sk_skb_demo.bpf.o
echo "== printk"
grep -a 'verdict' /tmp/trace_sk.txt | head -6
echo "== 已清理"
