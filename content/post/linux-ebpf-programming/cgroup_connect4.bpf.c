// SPDX-License-Identifier: GPL-2.0
/* cgroup/connect4：拒绝该 cgroup 内进程连向 8080 端口。 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

SEC("cgroup/connect4")
int connect4_block(struct bpf_sock_addr *ctx)
{
	if (ctx->user_port == bpf_htons(8080))
		return 0;		/* 0 = 拒绝，connect 返回 EPERM */
	return 1;			/* 1 = 放行 */
}

char _license[] SEC("license") = "GPL";
