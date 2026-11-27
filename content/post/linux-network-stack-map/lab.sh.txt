#!/bin/bash
# lab.sh —— 一键搭 / 拆网络栈实验用的 network namespace 拓扑
#
# 需要 root（ip netns、ip link 都要 CAP_SYS_ADMIN）：
#   sudo bash lab.sh direct
#
# 四种模式：
#   direct  A(10.77.1.1) ==veth== B(10.77.1.2)          一对 veth 直连，文中主线用的就是它
#   router  A --veth-- R --veth-- B                     中间放一个转发节点，用来观察 FORWARD
#   bridge  A / B / C 接在同一网桥                        同网段三点，观察 ARP 广播与转发库
#   clean   删掉所有 nslab-* 命名空间                      连带杀掉里面还活着的进程
#
# 约定：
#   * 所有对象都以 nslab- 为前缀，不碰宿主机已有网络，也不改默认路由。
#   * 每个模式下地址都是写死的，方便抓包时一眼认出方向；
#     MAC 也固定成 02:00:00:77:01:0X，02 开头是本地管理地址，不会和真实网卡冲突。
#   * 脚本不输出成功信息，set -e 下任何一步失败就直接退出并打印原因。

set -euo pipefail
# 清场：先杀掉命名空间里的进程，再删命名空间。
# 顺序不能反——ns 里还有活进程时 ip netns del 会留下 /var/run/netns 下的残骸，
# 下一轮 ip netns add 就会报 "File exists"。
cleanup() {
    for ns in nslab-a nslab-b nslab-r nslab-c; do
        if ip netns list | cut -d' ' -f1 | grep -qx "$ns"; then
            ip netns pids "$ns" | xargs -r kill
            ip netns del "$ns"
        fi
    done
}
case ${1:-} in
clean) cleanup; exit;;
# 只校验模式名，真正的建网在下面分三支走。
direct|router|bridge) ;;
*) echo 'usage: sudo bash lab.sh direct|router|bridge|clean' >&2; exit 2;;
esac
# 拒绝在已有拓扑上再建一套。
# 上一轮没 clean 就再跑，大概率是想"重来一次"，但直接覆盖会把正在观察的
# ns、计数器和抓包现场一起冲掉，所以宁可报错让人先看一眼或显式 clean。
if ip netns list | cut -d' ' -f1 | grep -q '^nslab-'; then
    echo 'nslab namespace exists: inspect it or run lab.sh clean first' >&2; exit 1
fi
# 新建的 ns 里 lo 默认是 down。不 up 的话连 127.0.0.1 都不通，
# 后面做 hook 实验（只在 lo 上 ping）会一个包都发不出去。
for ns in a b; do ip netns add nslab-$ns; ip -n nslab-$ns link set lo up; done

if [ "$1" = direct ]; then
    # 一对 veth：A 的 a0 与 B 的 b0 是同一根"虚拟网线"的两端，
    # 一端发出的帧直接进另一端的接收路径，中间没有交换机、没有物理层。
    ip link add a0 netns nslab-a type veth peer name b0 netns nslab-b
    ip -n nslab-a addr add 10.77.1.1/24 dev a0
    ip -n nslab-b addr add 10.77.1.2/24 dev b0
    ip -n nslab-a link set a0 address 02:00:00:77:01:01 up
    ip -n nslab-b link set b0 address 02:00:00:77:01:02 up
elif [ "$1" = router ]; then
    # 三节点两跳：A 和 B 在不同网段，必须经过 R 转发。
    # R 上开 ip_forward，A/B 各加一条默认路由指向各自的 R 侧地址，
    # 这样进出的包一定会经过 FORWARD 这个 hook。
    ip netns add nslab-r; ip -n nslab-r link set lo up
    ip link add a0 netns nslab-a type veth peer name ra netns nslab-r
    ip link add b0 netns nslab-b type veth peer name rb netns nslab-r
    ip -n nslab-a addr add 10.77.1.1/24 dev a0
    ip -n nslab-r addr add 10.77.1.254/24 dev ra
    ip -n nslab-b addr add 10.77.2.2/24 dev b0
    ip -n nslab-r addr add 10.77.2.254/24 dev rb
    # 四个 veth 端逐个 up：veth 两端都要 up，链路才算通。
    for pair in 'a a0' 'b b0' 'r ra' 'r rb'; do read -r n d <<< "$pair"; ip -n nslab-$n link set "$d" up; done
    ip -n nslab-a route add default via 10.77.1.254
    ip -n nslab-b route add default via 10.77.2.254
    ip netns exec nslab-r sysctl -qw net.ipv4.ip_forward=1
else
    # 网桥模式：三个 ns 各一根 veth 接到 R 里的 br0，同一个 10.77.1.0/24。
    # 目的不是互通，而是看二层行为：ARP 请求从 br0 泛洪到所有端口，
    # br0 学到 MAC 之后（bridge fdb show）后续就只往一个端口转发。
    ip netns add nslab-r; ip -n nslab-r link set lo up
    ip netns add nslab-c; ip -n nslab-c link set lo up
    ip -n nslab-r link add br0 type bridge
    ip -n nslab-r link set br0 up
    for n in a b c; do
        ip link add ${n}0 netns nslab-$n type veth peer name p$n netns nslab-r
        # 把 R 侧的那一端挂到 br0 上并 up；挂之前 br0 不认这个端口。
        ip -n nslab-r link set p$n master br0
        ip -n nslab-r link set p$n up
        ip -n nslab-$n link set ${n}0 up
    done
    ip -n nslab-a addr add 10.77.1.1/24 dev a0
    ip -n nslab-b addr add 10.77.1.2/24 dev b0
    ip -n nslab-c addr add 10.77.1.3/24 dev c0
    ip -n nslab-a link set a0 address 02:00:00:77:01:01
    ip -n nslab-b link set b0 address 02:00:00:77:01:02
    ip -n nslab-c link set c0 address 02:00:00:77:01:03
fi
