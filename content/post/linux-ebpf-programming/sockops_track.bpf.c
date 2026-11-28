// SPDX-License-Identifier: GPL-2.0
/* sock_ops：把该 cgroup 内建立的 TCP socket 按本地端口插入 sockhash，
 * 并用一个数组 map 记录插入成功的次数。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_SOCKHASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} sock_hash SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} hits SEC(".maps");

SEC("sockops")
int sockops_track(struct bpf_sock_ops *skops)
{
	__u32 key = skops->local_port;
	long ret;

	switch (skops->op) {
	case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:	/* 被动建立完成 */
	case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:	/* 主动建立完成 */
		ret = bpf_sock_hash_update(skops, &sock_hash, &key,
					   BPF_NOEXIST);
		bpf_printk("op=%d port=%u ret=%ld", skops->op, key, ret);
		if (ret == 0) {
			__u32 zero = 0;
			__u64 *cnt = bpf_map_lookup_elem(&hits, &zero);

			if (cnt)
				__sync_fetch_and_add(cnt, 1);
		}
		break;
	default:
		break;
	}
	return 1;
}

char _license[] SEC("license") = "GPL";
