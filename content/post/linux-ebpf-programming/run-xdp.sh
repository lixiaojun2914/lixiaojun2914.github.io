#!/bin/bash
set -e
cd /home/lxj/nslab/ebpf
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c xdp_ping.bpf.c -o xdp_ping.bpf.o
echo "== 编译完成"
bash setup-netns.sh

ip netns exec ns2 ip link set dev veth2 xdp off 2>/dev/null || true
if ip netns exec ns2 ip link set dev veth2 xdp obj xdp_ping.bpf.o sec xdp 2>/dev/null; then
	echo "== 挂载模式 native"
else
	ip netns exec ns2 ip link set dev veth2 xdpgeneric obj xdp_ping.bpf.o sec xdp
	echo "== 挂载模式 generic"
fi
ip netns exec ns2 ip -d link show veth2 | grep -A1 xdp | head -4

echo "== ping"
ip netns exec ns1 ping -c 3 -W 1 10.0.0.2 || true

echo "== map"
bpftool map dump name stats

ip netns exec ns2 ip link set dev veth2 xdp off
echo "== 已卸载"
