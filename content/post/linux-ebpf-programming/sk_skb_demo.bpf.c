// SPDX-License-Identifier: GPL-2.0
/* sk_skb/stream_verdict：对 SOCKMAP 里的 socket，首个数据段以 HELLO 开头就丢弃。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define SK_PASS 1
#define SK_DROP 0

struct {
	__uint(type, BPF_MAP_TYPE_SOCKMAP);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u32);
} sock_map SEC(".maps");

SEC("sk_skb/stream_verdict")
int sk_skb_verdict(struct __sk_buff *skb)
{
	char *data, *data_end;

	/* sk_skb 的 skb->data 默认可能指向空线性段，先把前 5 字节拉进线性区 */
	if (bpf_skb_pull_data(skb, 5) < 0)
		return SK_PASS;
	data = (void *)(long)skb->data;
	data_end = (void *)(long)skb->data_end;
	bpf_printk("verdict len=%u seg=%u", skb->len,
		   (unsigned int)(data_end - data));
	if (data + 5 > data_end)
		return SK_PASS;
	if (data[0] == 'H' && data[1] == 'E' && data[2] == 'L' &&
	    data[3] == 'L' && data[4] == 'O') {
		bpf_printk("verdict DROP");
		return SK_DROP;
	}
	return SK_PASS;
}

char _license[] SEC("license") = "GPL";
