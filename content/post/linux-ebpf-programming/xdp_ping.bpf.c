// SPDX-License-Identifier: GPL-2.0
/* 统计 ICMP echo request 并丢弃，验证 XDP 挂在 veth 的入向。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800		/* vmlinux.h 只有类型，没有这个宏 */
#endif

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

SEC("xdp")
int xdp_ping(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	struct ethhdr *eth = data;
	struct iphdr *iph;
	struct icmphdr *icmp;
	__u32 key = 0;
	__u64 *cnt;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;
	if (iph->protocol != IPPROTO_ICMP)
		return XDP_PASS;

	icmp = (void *)((__u8 *)iph + iph->ihl * 4);
	if ((void *)(icmp + 1) > data_end)
		return XDP_PASS;
	if (icmp->type != 8)			/* 只处理 echo request */
		return XDP_PASS;

	cnt = bpf_map_lookup_elem(&stats, &key);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);

	return XDP_DROP;
}

char _license[] SEC("license") = "GPL";
