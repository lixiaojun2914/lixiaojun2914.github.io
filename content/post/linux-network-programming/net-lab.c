/*
 * net-lab.c —— socket 接口与四种多路复用写法的对照程序
 *
 * 编译：gcc -O2 -Wall -Wextra net-lab.c -o net-lab -luring
 * 运行：
 *   ./net-lab listen                 阻塞 echo 服务端，接一条连接后退出
 *   ./net-lab client 1234            连本机 1234 端口，发一条，读回显
 *   ./net-lab mux select 8           单线程服务端，8 个客户端，select
 *   ./net-lab mux poll 8             同上，poll
 *   ./net-lab mux epoll 8            同上，epoll 水平触发
 *   ./net-lab mux epoll-et 8         同上，epoll 边缘触发
 *   ./net-lab mux uring 8            同上，io_uring
 *   ./net-lab shutdown               半关闭：客户端 SHUT_WR 后仍能收到回话
 *   ./net-lab rst                    客户端设 SO_LINGER 后 close，服务端读到 RST
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXCONN		64
#define BUFSZ		256

static int g_listen_fd = -1;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void addr_text(const struct sockaddr_in *a, char *out, size_t n)
{
	char ip[INET_ADDRSTRLEN];

	if (inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip)) == NULL)
		snprintf(ip, sizeof(ip), "?");
	snprintf(out, n, "%s:%u", ip, ntohs(a->sin_port));
}

/* 建监听 socket，打印内核分配的端口 */
static unsigned short tcp_listen(unsigned short port)
{
	struct sockaddr_in a;
	socklen_t alen = sizeof(a);
	char text[64];
	int on = 1;
	int fd;

	memset(&a, 0, sizeof(a));
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		die("socket");
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		die("setsockopt SO_REUSEADDR");

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		die("bind");
	if (listen(fd, 8) < 0)
		die("listen");
	if (getsockname(fd, (struct sockaddr *)&a, &alen) < 0)
		die("getsockname");

	addr_text(&a, text, sizeof(text));
	printf("listen fd=%d addr=%s\n", fd, text);

	g_listen_fd = fd;
	return ntohs(a.sin_port);
}

static int tcp_connect(unsigned short port)
{
	struct sockaddr_in a;
	int fd;

	memset(&a, 0, sizeof(a));
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		die("socket");

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		die("connect");
	return fd;
}

/* 一次 send 可能只送出一部分，循环写完 */
static int send_all(int fd, const char *p, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, p + off, len - off, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/* ---------- listen / client ---------- */

static void mode_listen(void)
{
	struct sockaddr_in peer;
	socklen_t plen = sizeof(peer);
	char text[64];
	char buf[BUFSZ];
	int lfd, cfd;
	ssize_t n;

	(void)tcp_listen(0);
	lfd = g_listen_fd;
	cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
	if (cfd < 0)
		die("accept");
	addr_text(&peer, text, sizeof(text));
	printf("accept4 fd=%d peer=%s\n", cfd, text);

	n = recv(cfd, buf, sizeof(buf), 0);
	if (n < 0)
		die("recv");
	printf("recv %zd bytes: %.*s\n", n, (int)n, buf);
	if (send_all(cfd, buf, (size_t)n) < 0)
		die("send");
	close(cfd);
	close(lfd);
}

static void mode_client(unsigned short port)
{
	char buf[BUFSZ];
	int fd = tcp_connect(port);
	ssize_t n;

	if (send_all(fd, "hello", 5) < 0)
		die("send");
	n = recv(fd, buf, sizeof(buf), 0);
	if (n < 0)
		die("recv");
	printf("got %zd bytes: %.*s\n", n, (int)n, buf);
	close(fd);
}

/* ---------- 关闭语义 ---------- */

/* 客户端关掉写方向，仍能读服务端的回话 */
static void mode_shutdown(void)
{
	unsigned short port = tcp_listen(0);
	pid_t child = fork();

	if (child < 0)
		die("fork");
	if (child == 0) {
		char buf[BUFSZ];
		ssize_t n;
		int fd;

		close(g_listen_fd);
		fd = tcp_connect(port);
		if (send_all(fd, "ping", 4) < 0)
			_exit(2);
		if (shutdown(fd, SHUT_WR) < 0)	/* 只关写方向 */
			_exit(3);
		n = recv(fd, buf, sizeof(buf), 0);
		printf("client after SHUT_WR: recv %zd bytes: %.*s\n",
		       n, n > 0 ? (int)n : 0, buf);
		close(fd);
		_exit(0);
	}

	{
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		char buf[BUFSZ];
		int cfd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
		ssize_t n;

		if (cfd < 0)
			die("accept");
		n = recv(cfd, buf, sizeof(buf), 0);
		printf("server first recv %zd bytes: %.*s\n", n, (int)n, buf);
		n = recv(cfd, buf, sizeof(buf), 0);
		printf("server second recv %zd (0 = 对端关了写方向)\n", n);
		if (send_all(cfd, "bye", 3) < 0)
			die("send");
		close(cfd);
	}
	waitpid(child, NULL, 0);
	close(g_listen_fd);
}

/* 客户端设 SO_LINGER 后直接 close，服务端收到 RST */
static void mode_rst(void)
{
	unsigned short port = tcp_listen(0);
	pid_t child = fork();

	if (child < 0)
		die("fork");
	if (child == 0) {
		struct linger lg = { 1, 0 };	/* onoff=1, linger=0：立即 RST */
		int fd;

		close(g_listen_fd);
		fd = tcp_connect(port);
		if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) < 0)
			_exit(2);
		close(fd);
		_exit(0);
	}

	{
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		char buf[BUFSZ];
		int cfd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
		ssize_t n;

		if (cfd < 0)
			die("accept");
		n = recv(cfd, buf, sizeof(buf), 0);
		if (n < 0)
			printf("server recv: -1 %s\n", strerror(errno));
		else
			printf("server recv: %zd\n", n);
		close(cfd);
	}
	waitpid(child, NULL, 0);
	close(g_listen_fd);
}

/* ---------- 多路复用 ---------- */

struct stat_s {
	int accepted;
	int done;
	long bytes;
	long loops;
};

/* 子进程里建 n 条连接，各发一条，读到回显后关闭 */
static pid_t spawn_clients(unsigned short port, int n)
{
	pid_t pid = fork();

	if (pid < 0)
		die("fork");
	if (pid > 0)
		return pid;

	if (g_listen_fd >= 0)
		close(g_listen_fd);		/* 子进程不监听 */

	for (int i = 0; i < n; i++) {
		char msg[32], buf[BUFSZ];
		int len, fd = tcp_connect(port);
		ssize_t r;

		len = snprintf(msg, sizeof(msg), "c%d:ping", i);
		if (send_all(fd, msg, (size_t)len) < 0)
			_exit(2);
		r = recv(fd, buf, sizeof(buf), 0);
		if (r != len)		/* 回显必须与发送一致 */
			_exit(3);
		close(fd);
	}
	_exit(0);
}

/* 一条连接收到数据：回写并关闭。返回 0 表示已关闭 */
static int serve_once(int fd, struct stat_s *st)
{
	char buf[BUFSZ];
	ssize_t n;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		close(fd);
		st->done++;
		return -1;
	}
	if (n == 0) {			/* 对端关闭 */
		close(fd);
		st->done++;
		return -1;
	}
	st->bytes += n;
	if (send_all(fd, buf, (size_t)n) < 0) {
		close(fd);
		st->done++;
		return -1;
	}
	close(fd);
	st->done++;
	return -1;
}

/* ET 版本：一次就绪必须把缓冲区读空，直到 EAGAIN */
static int drain_once(int fd, struct stat_s *st)
{
	char buf[BUFSZ];

	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);

		if (n > 0) {
			st->bytes += n;
			if (send_all(fd, buf, (size_t)n) < 0)
				break;
			continue;
		}
		if (n == 0)
			break;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;		/* 读空了，等下一次就绪 */
		if (errno == EINTR)
			continue;
		break;
	}
	close(fd);
	st->done++;
	return -1;
}

static void mux_select(int lfd, int n)
{
	struct stat_s st = {0};
	int conns[MAXCONN];
	struct timeval tv;
	char text[64];
	int i, nconn = 0;

	for (i = 0; i < MAXCONN; i++)
		conns[i] = -1;

	while (st.done < n) {
		fd_set rfds;
		int maxfd = lfd;
		int rc;

		if (lfd >= FD_SETSIZE) {
			fprintf(stderr, "fd %d 超过 FD_SETSIZE=%d\n", lfd, FD_SETSIZE);
			exit(1);
		}
		FD_ZERO(&rfds);			/* 每次都要重设 */
		FD_SET(lfd, &rfds);
		for (i = 0; i < nconn; i++) {
			if (conns[i] < 0)
				continue;
			if (conns[i] >= FD_SETSIZE) {
				fprintf(stderr, "fd %d 超过 FD_SETSIZE=%d\n",
					conns[i], FD_SETSIZE);
				exit(1);
			}
			FD_SET(conns[i], &rfds);
			if (conns[i] > maxfd)
				maxfd = conns[i];
		}
		tv.tv_sec = 5;			/* select 可能改写 tv，也要重设 */
		tv.tv_usec = 0;

		rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			die("select");
		}
		if (rc == 0)
			break;
		st.loops++;

		if (FD_ISSET(lfd, &rfds)) {
			struct sockaddr_in peer;
			socklen_t plen = sizeof(peer);
			int cfd = accept4(lfd, (struct sockaddr *)&peer, &plen,
					  SOCK_CLOEXEC);

			if (cfd >= 0 && nconn < MAXCONN) {
				conns[nconn++] = cfd;
				st.accepted++;
				if (st.accepted == 1) {
					addr_text(&peer, text, sizeof(text));
					printf("first peer=%s\n", text);
				}
			}
		}
		for (i = 0; i < nconn; i++) {
			if (conns[i] < 0 || !FD_ISSET(conns[i], &rfds))
				continue;
			serve_once(conns[i], &st);
			conns[i] = -1;
		}
	}
	printf("mode=select conns=%d accepted=%d bytes=%ld loops=%ld\n",
	       n, st.accepted, st.bytes, st.loops);
}

static void mux_poll(int lfd, int n)
{
	struct stat_s st = {0};
	struct pollfd pfd[MAXCONN + 1];
	int i, cnt;

	pfd[0].fd = lfd;
	pfd[0].events = POLLIN;
	pfd[0].revents = 0;
	cnt = 1;

	while (st.done < n) {
		int rc;

		rc = poll(pfd, (nfds_t)cnt, 5000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}
		if (rc == 0)
			break;
		st.loops++;

		if (pfd[0].revents & POLLIN) {
			int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

			if (cfd >= 0 && cnt < MAXCONN + 1) {
				pfd[cnt].fd = cfd;
				pfd[cnt].events = POLLIN;
				pfd[cnt].revents = 0;
				cnt++;
				st.accepted++;
			}
		}
		for (i = 1; i < cnt; i++) {
			if (pfd[i].fd < 0)
				continue;
			if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			serve_once(pfd[i].fd, &st);
			pfd[i].fd = -1;		/* 关闭后置 -1，poll 会跳过 */
		}
	}
	printf("mode=poll conns=%d accepted=%d bytes=%ld loops=%ld\n",
	       n, st.accepted, st.bytes, st.loops);
}

static void mux_epoll(int lfd, int n, int et)
{
	struct stat_s st = {0};
	struct epoll_event ev, evs[MAXCONN];
	int ep, i;

	ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = lfd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev) < 0)
		die("epoll_ctl ADD listen");

	while (st.done < n) {
		int rc = epoll_wait(ep, evs, MAXCONN, 5000);

		if (rc < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait");
		}
		if (rc == 0)
			break;
		st.loops++;

		for (i = 0; i < rc; i++) {
			int fd = evs[i].data.fd;

			if (fd == lfd) {
				int cfd = accept4(lfd, NULL, NULL,
						  SOCK_CLOEXEC | SOCK_NONBLOCK);

				if (cfd >= 0) {
					memset(&ev, 0, sizeof(ev));
					ev.events = EPOLLIN | (et ? EPOLLET : 0);
					ev.data.fd = cfd;
					if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev) < 0)
						die("epoll_ctl ADD conn");
					st.accepted++;
				}
				continue;
			}
			if (et)
				drain_once(fd, &st);
			else
				serve_once(fd, &st);
		}
	}
	close(ep);
	printf("mode=epoll%s conns=%d accepted=%d bytes=%ld loops=%ld\n",
	       et ? "-et" : "", n, st.accepted, st.bytes, st.loops);
}

/*
 * io_uring 版：一个 accept 在飞，连接建立后提交 recv，收到后提交 send。
 * user_data 区分三个阶段的完成事件。
 */
#define UD_ACCEPT		0ULL
#define UD_RECV(fd)		((uint64_t)(fd))
#define UD_SEND(fd)		((uint64_t)(fd) | (1ULL << 32))
#define UD_FD(ud)		((int)((ud) & 0xffffffffULL))
#define UD_IS_SEND(ud)		(((ud) >> 32) != 0)

static char g_ubuf[MAXCONN][BUFSZ];

static void mux_uring(int lfd, int n)
{
	struct stat_s st = {0};
	struct io_uring ring;
	int pending_accept = 0;

	if (io_uring_queue_init(32, &ring, 0) < 0)
		die("io_uring_queue_init");

	while (st.done < n) {
		struct io_uring_sqe *sqe;
		struct io_uring_cqe *cqe;
		int ret;

		if (st.accepted < n && !pending_accept) {
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_accept(sqe, lfd, NULL, NULL, SOCK_CLOEXEC);
			sqe->user_data = UD_ACCEPT;
			if (io_uring_submit(&ring) < 0)
				die("io_uring_submit accept");
			pending_accept = 1;
		}

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			break;
		}

		st.loops++;
		if (cqe->user_data == UD_ACCEPT) {
			int cfd = cqe->res;

			io_uring_cqe_seen(&ring, cqe);
			pending_accept = 0;
			if (cfd < 0 || cfd >= MAXCONN) {
				if (cfd >= 0)
					close(cfd);
				continue;
			}
			st.accepted++;
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_recv(sqe, cfd, g_ubuf[cfd], BUFSZ, 0);
			sqe->user_data = UD_RECV(cfd);
			if (io_uring_submit(&ring) < 0)
				die("io_uring_submit recv");
			continue;
		}

		if (UD_IS_SEND(cqe->user_data)) {
			int fd = UD_FD(cqe->user_data);

			io_uring_cqe_seen(&ring, cqe);
			close(fd);
			st.done++;
			continue;
		}

		{
			int fd = UD_FD(cqe->user_data);
			int res = cqe->res;

			io_uring_cqe_seen(&ring, cqe);
			if (res <= 0) {		/* 0 = 对端关闭，负数 = 出错 */
				close(fd);
				st.done++;
				continue;
			}
			st.bytes += res;
			sqe = io_uring_get_sqe(&ring);
			io_uring_prep_send(sqe, fd, g_ubuf[fd], (size_t)res, 0);
			sqe->user_data = UD_SEND(fd);
			if (io_uring_submit(&ring) < 0)
				die("io_uring_submit send");
		}
	}

	io_uring_queue_exit(&ring);
	printf("mode=uring conns=%d accepted=%d bytes=%ld loops=%ld\n",
	       n, st.accepted, st.bytes, st.loops);
}

/* ---------- 入口 ---------- */

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);	/* fork 前后都不要留缓冲 */

	if (argc < 2) {
		fprintf(stderr,
			"用法: %s listen | client <port> | mux <select|poll|epoll|uring> [n]\n",
			argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "listen")) {
		mode_listen();
		return 0;
	}
	if (!strcmp(argv[1], "client")) {
		if (argc < 3) {
			fprintf(stderr, "用法: %s client <port>\n", argv[0]);
			return 1;
		}
		mode_client((unsigned short)atoi(argv[2]));
		return 0;
	}
	if (!strcmp(argv[1], "shutdown")) {
		mode_shutdown();
		return 0;
	}
	if (!strcmp(argv[1], "rst")) {
		mode_rst();
		return 0;
	}
	if (!strcmp(argv[1], "mux")) {
		const char *how = argc > 2 ? argv[2] : "epoll";
		int nconn = argc > 3 ? atoi(argv[3]) : 8;
		unsigned short port;
		pid_t child;
		int st;

		if (nconn > MAXCONN)
			nconn = MAXCONN;
		port = tcp_listen(0);
		child = spawn_clients(port, nconn);

		if (!strcmp(how, "select"))
			mux_select(g_listen_fd, nconn);
		else if (!strcmp(how, "poll"))
			mux_poll(g_listen_fd, nconn);
		else if (!strcmp(how, "epoll"))
			mux_epoll(g_listen_fd, nconn, 0);
		else if (!strcmp(how, "epoll-et"))
			mux_epoll(g_listen_fd, nconn, 1);
		else if (!strcmp(how, "uring"))
			mux_uring(g_listen_fd, nconn);
		else {
			fprintf(stderr, "未知模式 %s\n", how);
			kill(child, SIGKILL);
			return 1;
		}

		waitpid(child, &st, 0);
		close(g_listen_fd);
		return 0;
	}

	fprintf(stderr, "未知命令 %s\n", argv[1]);
	return 1;
}
