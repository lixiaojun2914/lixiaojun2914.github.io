// SPDX-License-Identifier: GPL-2.0
/* sockmap 两件套：sock_ops 把 socket 放进 sockhash，
 * sk_skb/stream_verdict 决定入方向数据是 SK_PASS 还是 SK_DROP。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define SK_PASS 1
#define SK_DROP 0

struct {
	__uint(type, BPF_MAP_TYPE_SOCKHASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} sock_hash SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} stats SEC(".maps");

static __always_inline void bump(__u32 idx)
{
	__u64 *p = bpf_map_lookup_elem(&stats, &idx);

	if (p)
		__sync_fetch_and_add(p, 1);
}

SEC("sockops")
int sockops_track(struct bpf_sock_ops *skops)
{
	__u32 key = skops->local_port;

	switch (skops->op) {
	case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:
	case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:
	{
		long r = bpf_sock_hash_update(skops, &sock_hash, &key,
					      BPF_NOEXIST);

		bpf_printk("op=%d port=%u ret=%ld", skops->op, key, r);
		if (r == 0)
			bump(0);
	}
		break;
	default:
		break;
	}
	return 1;
}

SEC("sk_skb/stream_verdict")
int sk_skb_verdict(struct __sk_buff *skb)
{
	char *data = (void *)(long)skb->data;
	char *data_end = (void *)(long)skb->data_end;

	bpf_printk("verdict len=%u seg=%u", skb->len,
		   (unsigned int)(data_end - data));
	/* 只处理首段里的前 5 字节，其余一律放行 */
	if (data + 5 > data_end) {
		bpf_printk("verdict short");
		return SK_PASS;
	}
	bpf_printk("verdict b=%d%d", data[0], data[1]);
	if (data[0] == 'H' && data[1] == 'E' && data[2] == 'L' &&
	    data[3] == 'L' && data[4] == 'O') {
		bump(1);
		return SK_DROP;
	}
	return SK_PASS;
}

char _license[] SEC("license") = "GPL";
