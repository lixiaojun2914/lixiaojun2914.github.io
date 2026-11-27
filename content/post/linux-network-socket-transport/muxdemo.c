/* muxdemo.c —— 同一件事的五种写法：线程、epoll LT、epoll ET、epoll 多线程、只读一次
 *
 * muxdemo thread    <port>            每连接一个线程，阻塞收发
 * muxdemo epoll-lt  <port>            单线程 epoll，水平触发，每次事件读一次
 * muxdemo epoll-et  <port>            单线程 epoll，边缘触发，读到 EAGAIN
 * muxdemo epoll-mt  <port> <threads>  epoll + 多线程，EPOLLONESHOT 重新武装
 * muxdemo sink-lt   <port>            只收不回，水平触发
 * muxdemo sink-et   <port>            只收不回，边缘触发读到 EAGAIN
 * muxdemo sink-et-once <port>         只收不回，边缘触发但只读一次
 * muxdemo client     <ip> <port> <conns> <rounds>   回环压测，每轮收发 64 字节
 * muxdemo blast      <ip> <port> <bytes>            只写不收，带 3 秒截止
 *
 * 服务端在收到 SIGUSR1 时打印计数器，便于对照不同写法发出多少次通知。
 * 编译：gcc -O2 -pthread -o muxdemo muxdemo.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MSG 64
#define MAXEV 256

static volatile unsigned long n_wait, n_read, n_write, n_accept;

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void on_usr1(int sig)
{
    (void)sig;
    char b[160];
    int n = snprintf(b, sizeof b, "stats epoll_wait=%lu accept=%lu read=%lu write=%lu\n",
                     n_wait, n_accept, n_read, n_write);
    if (write(1, b, n) < 0) { /* 忽略 */ }
}

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 512) < 0) { perror("listen"); exit(1); }
    return fd;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 读一次就回写一次，返回读到的字节数；EAGAIN 返回 -1 */
static int echo_once(int fd)
{
    char b[65536];
    ssize_t r = recv(fd, b, sizeof b, 0);
    if (r <= 0) return (int)r;
    n_read++;
    ssize_t off = 0;
    while (off < r) {
        ssize_t w = send(fd, b + off, r - off, MSG_NOSIGNAL);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
        off += w;
    }
    n_write++;
    return (int)r;
}

/* ---------- 模式一：每连接一个线程 ---------- */

static void *thread_worker(void *arg)
{
    int fd = (int)(long)arg;
    char b[MSG];
    for (;;) {
        ssize_t r = recv(fd, b, sizeof b, MSG_WAITALL);
        if (r <= 0) break;
        n_read++;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = send(fd, b + off, r - off, MSG_NOSIGNAL);
            if (w <= 0) goto out;
            off += w;
        }
        n_write++;
    }
out:
    close(fd);
    return NULL;
}

static void run_thread(int port)
{
    int lfd = listen_on(port);
    fprintf(stderr, "ready thread\n");
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        n_accept++;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_t th;
        if (pthread_create(&th, NULL, thread_worker, (void *)(long)fd) == 0)
            pthread_detach(th);
        else
            close(fd);
    }
}

/* ---------- 模式二、三：单线程 epoll ---------- */

static int add_fd(int ep, int fd, unsigned flags)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = flags;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static void run_epoll(int port, int edge)
{
    int lfd = listen_on(port);
    set_nonblock(lfd);
    int ep = epoll_create1(0);
    add_fd(ep, lfd, EPOLLIN);
    struct epoll_event evs[MAXEV];
    fprintf(stderr, "ready epoll-%s\n", edge ? "et" : "lt");

    for (;;) {
        int n = epoll_wait(ep, evs, MAXEV, -1);
        n_wait++;
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                for (;;) {
                    int c = accept(lfd, NULL, NULL);
                    if (c < 0) break;
                    n_accept++;
                    set_nonblock(c);
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    add_fd(ep, c, EPOLLIN);
                }
                continue;
            }
            if (edge) {
                int r;
                while ((r = echo_once(fd)) > 0)
                    ;
                if (r == 0) { epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL); close(fd); }
            } else {
                int r = echo_once(fd);
                if (r == 0) { epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL); close(fd); }
            }
        }
    }
}

/* ---------- 模式四：epoll 多线程，EPOLLONESHOT ---------- */

struct mt {
    int ep;
    int lfd;
    pthread_mutex_t mu;
};

static struct mt g_mt;

static void *mt_worker(void *arg)
{
    (void)arg;
    struct epoll_event evs[MAXEV];
    for (;;) {
        int n = epoll_wait(g_mt.ep, evs, MAXEV, -1);
        n_wait++;
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == g_mt.lfd) {
                pthread_mutex_lock(&g_mt.mu);
                for (;;) {
                    int c = accept(g_mt.lfd, NULL, NULL);
                    if (c < 0) break;
                    n_accept++;
                    set_nonblock(c);
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    add_fd(g_mt.ep, c, EPOLLIN | EPOLLONESHOT);
                }
                pthread_mutex_unlock(&g_mt.mu);
                struct epoll_event again;
                memset(&again, 0, sizeof again);
                again.events = EPOLLIN;
                again.data.fd = g_mt.lfd;
                epoll_ctl(g_mt.ep, EPOLL_CTL_MOD, g_mt.lfd, &again);
                continue;
            }
            int r;
            while ((r = echo_once(fd)) > 0)
                ;
            if (r == 0) { epoll_ctl(g_mt.ep, EPOLL_CTL_DEL, fd, NULL); close(fd); }
            else {
                struct epoll_event again;
                memset(&again, 0, sizeof again);
                again.events = EPOLLIN | EPOLLONESHOT;
                again.data.fd = fd;
                epoll_ctl(g_mt.ep, EPOLL_CTL_MOD, fd, &again);
            }
        }
    }
    return NULL;
}

static void run_epoll_mt(int port, int workers)
{
    g_mt.lfd = listen_on(port);
    set_nonblock(g_mt.lfd);
    g_mt.ep = epoll_create1(0);
    pthread_mutex_init(&g_mt.mu, NULL);
    add_fd(g_mt.ep, g_mt.lfd, EPOLLIN);
    fprintf(stderr, "ready epoll-mt %d\n", workers);
    for (int i = 0; i < workers; i++) {
        pthread_t th;
        pthread_create(&th, NULL, mt_worker, NULL);
        pthread_detach(th);
    }
    for (;;) pause();
}

/* ---------- 收包对照：只读不回 ---------- */

/* edge: 0 水平触发每次事件读一次；1 边缘触发读到 EAGAIN；2 边缘触发只读一次 */
static void run_sink(int port, int edge)
{
    int lfd = listen_on(port);
    fprintf(stderr, "ready sink\n");
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) { perror("accept"); exit(1); }
    set_nonblock(fd);
    int ep = epoll_create1(0);
    add_fd(ep, fd, edge ? EPOLLIN | EPOLLET : EPOLLIN);
    struct epoll_event ev;
    unsigned long total = 0, reads = 0, wakes = 0;
    long last = now_ms();
    for (;;) {
        int n = epoll_wait(ep, &ev, 1, 500);
        if (n <= 0) {
            if (now_ms() - last > 1000) break;
            continue;
        }
        wakes++;
        char b[4096];
        if (edge == 1) {
            for (;;) {
                ssize_t r = recv(fd, b, sizeof b, 0);
                if (r > 0) { total += r; reads++; last = now_ms(); }
                else if (r == 0) goto done;
                else break;
            }
        } else {
            ssize_t r = recv(fd, b, sizeof b, 0);
            if (r > 0) { total += r; reads++; last = now_ms(); }
            else if (r == 0) break;
        }
    }
done:
    printf("read=%lu reads=%lu wakes=%lu\n", total, reads, wakes);
    fflush(stdout);
}

/* ---------- 客户端 ---------- */

static int connect_to(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); exit(1); }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int run_client(const char *ip, int port, int conns, int rounds)
{
    int *fd = calloc(conns, sizeof(int));
    char out[MSG], in[MSG];
    memset(out, 'x', sizeof out);
    for (int i = 0; i < conns; i++) fd[i] = connect_to(ip, port);

    long t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < conns; i++) {
            if (send(fd[i], out, MSG, MSG_NOSIGNAL) != MSG) { perror("send"); exit(1); }
            size_t got = 0;
            while (got < MSG) {
                ssize_t n = recv(fd[i], in + got, MSG - got, 0);
                if (n <= 0) { perror("recv"); exit(1); }
                got += n;
            }
        }
    }
    long dt = now_ms() - t0;
    printf("conns=%d rounds=%d msgs=%d ms=%ld\n", conns, rounds, conns * rounds, dt);
    for (int i = 0; i < conns; i++) close(fd[i]);
    return 0;
}

static int run_blast(const char *ip, int port, long bytes, int hold_ms)
{
    int fd = connect_to(ip, port);
    set_nonblock(fd);
    char *buf = malloc(65536);
    memset(buf, 'y', 65536);
    long sent = 0, deadline = now_ms() + 3000;
    while (sent < bytes && now_ms() < deadline) {
        size_t want = (size_t)((bytes - sent) < 65536 ? (bytes - sent) : 65536);
        ssize_t n = send(fd, buf, want, MSG_NOSIGNAL);
        if (n > 0) sent += n;
        else if (errno != EAGAIN && errno != EINTR) break;
        else usleep(1000);
    }
    printf("sent=%ld\n", sent);
    fflush(stdout);
    if (hold_ms > 0) usleep((useconds_t)hold_ms * 1000);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage:\n"
                "  muxdemo thread <port>\n"
                "  muxdemo epoll-lt <port>\n"
                "  muxdemo epoll-et <port>\n"
                "  muxdemo epoll-mt <port> <threads>\n"
                "  muxdemo sink-lt|sink-et|sink-et-once <port>\n"
                "  muxdemo client <ip> <port> <conns> <rounds>\n"
                "  muxdemo blast <ip> <port> <bytes>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGUSR1, on_usr1);
    signal(SIGPIPE, SIG_IGN);

    if (!strcmp(argv[1], "thread")) {
        run_thread(atoi(argv[2]));
        return 0;
    } else if (!strcmp(argv[1], "epoll-lt")) {
        run_epoll(atoi(argv[2]), 0);
        return 0;
    } else if (!strcmp(argv[1], "epoll-et")) {
        run_epoll(atoi(argv[2]), 1);
        return 0;
    } else if (!strcmp(argv[1], "epoll-mt")) {
        run_epoll_mt(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 4);
        return 0;
    } else if (!strcmp(argv[1], "sink-lt")) {
        run_sink(atoi(argv[2]), 0);
        return 0;
    } else if (!strcmp(argv[1], "sink-et")) {
        run_sink(atoi(argv[2]), 1);
        return 0;
    } else if (!strcmp(argv[1], "sink-et-once")) {
        run_sink(atoi(argv[2]), 2);
        return 0;
    } else if (!strcmp(argv[1], "client")) {
        return run_client(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    } else if (!strcmp(argv[1], "blast")) {
        return run_blast(argv[2], atoi(argv[3]), atol(argv[4]), argc > 5 ? atoi(argv[5]) : 0);
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
