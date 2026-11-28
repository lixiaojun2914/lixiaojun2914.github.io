// SPDX-License-Identifier: GPL-2.0
/* 故意不做边界检查，用来观察 verifier 的拒绝理由。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

SEC("xdp")
int bad_access(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;

	return eth->h_proto;		/* 没有校验 data + sizeof(*eth) <= data_end */
}

char _license[] SEC("license") = "GPL";
