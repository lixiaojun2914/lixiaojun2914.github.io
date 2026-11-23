#!/bin/bash
set -euo pipefail
# Run with sudo. All objects stay inside nslab-* namespaces.
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
direct|router|bridge) ;;
*) echo 'usage: sudo bash lab.sh direct|router|bridge|clean' >&2; exit 2;;
esac
# Refuse to overwrite a live experiment.
if ip netns list | cut -d' ' -f1 | grep -q '^nslab-'; then
    echo 'nslab namespace exists: inspect it or run lab.sh clean first' >&2; exit 1
fi
for ns in a b; do ip netns add nslab-$ns; ip -n nslab-$ns link set lo up; done
if [ "$1" = direct ]; then
    ip link add a0 netns nslab-a type veth peer name b0 netns nslab-b
    ip -n nslab-a addr add 10.77.1.1/24 dev a0
    ip -n nslab-b addr add 10.77.1.2/24 dev b0
    ip -n nslab-a link set a0 address 02:00:00:77:01:01 up
    ip -n nslab-b link set b0 address 02:00:00:77:01:02 up
elif [ "$1" = router ]; then
    ip netns add nslab-r; ip -n nslab-r link set lo up
    ip link add a0 netns nslab-a type veth peer name ra netns nslab-r
    ip link add b0 netns nslab-b type veth peer name rb netns nslab-r
    ip -n nslab-a addr add 10.77.1.1/24 dev a0
    ip -n nslab-r addr add 10.77.1.254/24 dev ra
    ip -n nslab-b addr add 10.77.2.2/24 dev b0
    ip -n nslab-r addr add 10.77.2.254/24 dev rb
    for pair in 'a a0' 'b b0' 'r ra' 'r rb'; do read -r n d <<< "$pair"; ip -n nslab-$n link set "$d" up; done
    ip -n nslab-a route add default via 10.77.1.254
    ip -n nslab-b route add default via 10.77.2.254
    ip netns exec nslab-r sysctl -qw net.ipv4.ip_forward=1
else
    ip netns add nslab-r; ip -n nslab-r link set lo up
    ip netns add nslab-c; ip -n nslab-c link set lo up
    ip -n nslab-r link add br0 type bridge
    ip -n nslab-r link set br0 up
    for n in a b c; do
        ip link add ${n}0 netns nslab-$n type veth peer name p$n netns nslab-r
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
