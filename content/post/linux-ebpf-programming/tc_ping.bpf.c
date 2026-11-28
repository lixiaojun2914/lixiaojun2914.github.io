// SPDX-License-Identifier: GPL-2.0
/* tc egress：统计并丢弃 ICMP echo request，验证 clsact 的挂载位置。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800
#endif
#define TC_ACT_OK	0
#define TC_ACT_SHOT	2

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} tc_stats SEC(".maps");

SEC("tc")
int tc_ping(struct __sk_buff *skb)
{
	void *data = (void *)(long)skb->data;
	void *data_end = (void *)(long)skb->data_end;
	struct ethhdr *eth = data;
	struct iphdr *iph;
	struct icmphdr *icmp;
	__u32 key = 0;
	__u64 *cnt;

	if ((void *)(eth + 1) > data_end)
		return TC_ACT_OK;
	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return TC_ACT_OK;

	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return TC_ACT_OK;
	if (iph->protocol != IPPROTO_ICMP)
		return TC_ACT_OK;

	icmp = (void *)((__u8 *)iph + iph->ihl * 4);
	if ((void *)(icmp + 1) > data_end)
		return TC_ACT_OK;
	if (icmp->type != 8)
		return TC_ACT_OK;

	cnt = bpf_map_lookup_elem(&tc_stats, &key);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);

	return TC_ACT_SHOT;
}

char _license[] SEC("license") = "GPL";
