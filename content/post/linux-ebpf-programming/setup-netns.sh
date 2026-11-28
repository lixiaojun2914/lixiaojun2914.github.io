#!/bin/bash
# 两个 netns，一条 veth 相连，用于 XDP/tc 实验
set -e
ip netns del ns1 2>/dev/null || true
ip netns del ns2 2>/dev/null || true
ip netns add ns1
ip netns add ns2
ip link add veth1 type veth peer name veth2
ip link set veth1 netns ns1
ip link set veth2 netns ns2
ip netns exec ns1 ip addr add 10.0.0.1/24 dev veth1
ip netns exec ns2 ip addr add 10.0.0.2/24 dev veth2
ip netns exec ns1 ip link set veth1 up
ip netns exec ns2 ip link set veth2 up
ip netns exec ns1 ip link set lo up
ip netns exec ns2 ip link set lo up
echo "ns1=10.0.0.1  ns2=10.0.0.2"
