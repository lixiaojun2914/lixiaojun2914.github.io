// SPDX-License-Identifier: GPL-2.0
/* 最小加载器：把 sk_skb/stream_verdict 程序 attach 到 sockhash，
 * 再把 sock_ops 程序 attach 到指定 cgroup。 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

int main(int argc, char **argv)
{
	struct bpf_object *obj;
	struct bpf_program *prog;
	int map_fd, cg_fd, err;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <bpf.o> <cgroup-path>\n", argv[0]);
		return 1;
	}

	obj = bpf_object__open_file(argv[1], NULL);
	if (!obj) {
		fprintf(stderr, "open failed\n");
		return 1;
	}
	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "load failed: %d\n", err);
		return 1;
	}

	map_fd = bpf_object__find_map_fd_by_name(obj, "sock_hash");
	if (map_fd < 0) {
		fprintf(stderr, "map not found\n");
		return 1;
	}

	prog = bpf_object__find_program_by_name(obj, "sk_skb_verdict");
	if (!prog) {
		fprintf(stderr, "verdict prog not found\n");
		return 1;
	}
	err = bpf_prog_attach(bpf_program__fd(prog), map_fd,
			      BPF_SK_SKB_STREAM_VERDICT, 0);
	if (err) {
		fprintf(stderr, "attach verdict failed: %d\n", err);
		return 1;
	}
	printf("verdict attached to sockhash\n");

	cg_fd = open(argv[2], O_RDONLY | O_DIRECTORY);
	if (cg_fd < 0) {
		fprintf(stderr, "open cgroup failed\n");
		return 1;
	}
	prog = bpf_object__find_program_by_name(obj, "sockops_track");
	if (!prog) {
		fprintf(stderr, "sockops prog not found\n");
		return 1;
	}
	err = bpf_prog_attach(bpf_program__fd(prog), cg_fd,
			      BPF_CGROUP_SOCK_OPS, 0);
	if (err) {
		fprintf(stderr, "attach sockops failed: %d\n", err);
		return 1;
	}
	printf("sock_ops attached to cgroup\n");

	fflush(stdout);
	sleep(10);
	return 0;
}
