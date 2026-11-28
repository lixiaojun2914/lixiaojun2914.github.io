// SPDX-License-Identifier: GPL-2.0
/* SOCKMAP + stream_verdict 最小验证：accept 后把连接 fd 插入 map，
 * 子进程 connect 并发送 HELLO，父进程 recv 观察是否被丢。 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define PORT 9092

static int do_server(int map_fd)
{
	struct sockaddr_in addr = {0};
	struct timeval tv = {2, 0};
	char buf[64];
	int ls, cs, key = 0, n, one = 1;

	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(ls, (void *)&addr, sizeof(addr)) || listen(ls, 4)) {
		perror("bind/listen");
		return 1;
	}

	if (fork() == 0) {			/* 子进程：客户端 */
		int s;

		usleep(200000);
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(s, (void *)&addr, sizeof(addr))) {
			perror("connect");
			_exit(1);
		}
		write(s, "HELLO", 5);
		usleep(300000);
		_exit(0);
	}

	cs = accept(ls, NULL, NULL);
	if (cs < 0) {
		perror("accept");
		return 1;
	}
	/* value 传 fd，内核取走 socket 引用 */
	if (bpf_map_update_elem(map_fd, &key, &cs, BPF_ANY)) {
		perror("map_update");
		return 1;
	}
	printf("accepted fd=%d, inserted into sockmap\n", cs);

	setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	n = recv(cs, buf, sizeof(buf), 0);
	if (n > 0)
		printf("server recv %d bytes: %.*s\n", n, n, buf);
	else if (n == 0)
		printf("server recv: EOF\n");
	else
		printf("server recv: %s (data dropped)\n", strerror(errno));
	return 0;
}

int main(int argc, char **argv)
{
	struct bpf_object *obj;
	struct bpf_program *prog;
	int map_fd, err;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <bpf.o>\n", argv[0]);
		return 1;
	}
	obj = bpf_object__open_file(argv[1], NULL);
	if (!obj || bpf_object__load(obj)) {
		fprintf(stderr, "load failed\n");
		return 1;
	}
	map_fd = bpf_object__find_map_fd_by_name(obj, "sock_map");
	prog = bpf_object__find_program_by_name(obj, "sk_skb_verdict");
	if (map_fd < 0 || !prog) {
		fprintf(stderr, "map/prog not found\n");
		return 1;
	}
	err = bpf_prog_attach(bpf_program__fd(prog), map_fd,
			      BPF_SK_SKB_STREAM_VERDICT, 0);
	if (err) {
		fprintf(stderr, "attach verdict failed: %d\n", err);
		return 1;
	}
	printf("verdict attached to sockmap\n");
	return do_server(map_fd);
}
