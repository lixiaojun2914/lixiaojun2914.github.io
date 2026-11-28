#!/bin/bash
set -e
cd /home/lxj/nslab/ebpf
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c tc_ping.bpf.c -o tc_ping.bpf.o
echo "== 编译完成"
bash setup-netns.sh
ip netns exec ns2 tc qdisc add dev veth2 clsact
ip netns exec ns2 tc filter add dev veth2 egress bpf da obj tc_ping.bpf.o sec tc
echo "== filter 已挂"
ip netns exec ns2 tc filter show dev veth2 egress | head -4
echo "== ns2 向 ns1 ping"
ip netns exec ns2 ping -c 3 -W 1 10.0.0.1 || true
echo "== map"
bpftool map dump name tc_stats -p
ip netns exec ns2 tc qdisc del dev veth2 clsact
echo "== 已清理"
