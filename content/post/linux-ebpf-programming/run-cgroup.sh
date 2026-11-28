#!/bin/bash
set -e
cd /home/lxj/nslab/ebpf
clang -O2 -g -target bpf -D__TARGET_ARCH_arm64 -I. -Wno-missing-declarations \
      -c cgroup_connect4.bpf.c -o cgroup_connect4.bpf.o
echo "== 编译完成"
bash setup-netns.sh
mkdir -p /sys/fs/cgroup/ebpf-demo

server() {
	timeout 5 ip netns exec ns2 python3 -c "
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('10.0.0.2', 8080)); s.listen(4)
c, _ = s.accept(); print('server accepted'); c.close()
" &
	SRV=$!
	sleep 0.4
}

echo "== 未挂载时直连"
server
ip netns exec ns1 python3 -c "
import socket
s = socket.socket(); s.settimeout(2); s.connect(('10.0.0.2', 8080)); print('direct connect ok')
" || echo "direct connect failed"
wait $SRV 2>/dev/null || true

rm -rf /sys/fs/bpf/connect4
bpftool prog loadall cgroup_connect4.bpf.o /sys/fs/bpf/connect4
echo "== 已加载"
bpftool cgroup attach /sys/fs/cgroup/ebpf-demo connect4 pinned /sys/fs/bpf/connect4/connect4_block
echo "== 已挂到 cgroup"

echo "== 同一 cgroup 内再连"
server
sh -c 'echo $$ > /sys/fs/cgroup/ebpf-demo/cgroup.procs; exec ip netns exec ns1 python3 -c "
import socket
s = socket.socket(); s.settimeout(2)
try:
    s.connect((\"10.0.0.2\", 8080)); print(\"connect ok (unexpected)\")
except OSError as e:
    print(\"connect failed:\", e)
"'
wait $SRV 2>/dev/null || true

bpftool cgroup detach /sys/fs/cgroup/ebpf-demo connect4 pinned /sys/fs/bpf/connect4/connect4_block
rmdir /sys/fs/cgroup/ebpf-demo 2>/dev/null || true
echo "== 已清理"
