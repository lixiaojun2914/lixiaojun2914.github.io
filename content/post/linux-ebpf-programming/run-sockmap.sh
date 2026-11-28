#!/bin/bash
set -e
cd /home/lxj/nslab/ebpf
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c sockops_track.bpf.c -o sockops_track.bpf.o
echo "== 编译完成"
mkdir -p /sys/fs/cgroup/ebpf-demo

timeout 8 python3 -c "
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 9090)); s.listen(4)
c, _ = s.accept(); print('server accepted'); c.close()
" &
SRV=$!
sleep 0.4

rm -rf /sys/fs/bpf/sockops
bpftool prog load sockops_track.bpf.o /sys/fs/bpf/sockops
bpftool cgroup attach /sys/fs/cgroup/ebpf-demo sock_ops pinned /sys/fs/bpf/sockops
echo "== sock_ops 已挂到 cgroup"

timeout 6 cat /sys/kernel/debug/tracing/trace_pipe > /tmp/trace_sockops.txt 2>/dev/null &
sleep 0.3

sh -c 'echo $$ > /sys/fs/cgroup/ebpf-demo/cgroup.procs; exec python3 -c "
import socket
s = socket.socket(); s.connect((\"127.0.0.1\", 9090)); s.close(); print(\"client connected\")
"'
sleep 0.3
echo "== sockhash 内容"
bpftool map dump name sock_hash 2>&1 | head -20
echo "== hits"
bpftool map dump name hits 2>&1 | head -6
echo "== printk"
grep -a sockops /tmp/trace_sockops.txt | head -6
wait $SRV 2>/dev/null || true

bpftool cgroup detach /sys/fs/cgroup/ebpf-demo sock_ops pinned /sys/fs/bpf/sockops
rm -rf /sys/fs/bpf/sockops
rmdir /sys/fs/cgroup/ebpf-demo 2>/dev/null || true
echo "== 已清理"
