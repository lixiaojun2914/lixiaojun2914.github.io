/*
 * netdemo —— 网络栈实验用的最小收发程序
 *
 * 目的：配合 linux-network-stack-map 一文，在 veth 或真实网卡上造出
 *       "应用写了 5 个字节，抓包却是 71 字节" 这种能对照的现象。
 *
 *       刻意不写重试、不写超时、不打日志：每一步都可以从外面用
 *       tcpdump / strace / bpftrace 看清楚，行为越简单越好解释。
 *
 * 编译：gcc -O0 -g -Wall -Wextra netdemo.c -o netdemo
 *       -O0 是故意的。开了优化，tcp_transmit_skb 这类静态函数会被
 *       内联或常量传播掉，kprobe 就挂不上，时间线会断。
 *
 * 用法：
 *   netdemo tcp-server IP PORT [delay_ms]   accept 后先等 delay_ms 再读，读完打印总数
 *   netdemo tcp-client IP PORT [bytes]      connect 后写 bytes 字节（默认 5），打印耗时
 *   netdemo tcp-fill   IP PORT [hold_ms]   把发送队列写满，停在 EAGAIN，然后挂 hold_ms（默认 4000）
 *   netdemo udp-server IP PORT [recv_bytes] [delay_ms]
 *                                           收两个报文，每次最多取 recv_bytes 字节（默认 3），
 *                                           带 MSG_TRUNC 打印报文原始长度与实际拿到的字节
 *   netdemo udp-client IP PORT              发两个报文：6 字节、2 字节
 *
 * 退出码：0 正常；1 某个系统调用失败（perror 打印 errno 原文）；2 参数不对。
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* 系统调用失败就直接退出：实验程序不恢复，只把 errno 原文留给人看。 */
static void die(const char *s) { perror(s); exit(1); }

/* 用单调时钟算耗时。CLOCK_REALTIME 会被 NTP 调整，量微秒级差值不能用它。 */
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime");
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* 只接受点分十进制 IPv4（inet_pton 返回 1 才算成功），不做域名解析：
 * 实验里地址是固定的，解析会引入 DNS 这条与本篇无关的路径。 */
static struct sockaddr_in address(const char *ip, int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { fputs("bad IPv4\n",stderr); exit(1); }
    return a;
}

/* 睡 ms 毫秒。nanosleep 被信号打断时剩余时间会写回 t，EINTR 就接着睡完。 */
static void pause_ms(int ms) {
    struct timespec t = {.tv_sec=ms/1000, .tv_nsec=(ms%1000)*1000000L};
    while (nanosleep(&t,&t) && errno==EINTR) {}
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,"usage: %s tcp-server|tcp-client|tcp-fill|udp-server|udp-client IP PORT [recv_bytes|delay_ms|bytes] [delay_ms]\n",argv[0]);
        return 2;
    }
    /* 关掉 stdio 缓冲：输出常被重定向到文件（配合 bpftrace -c 观察顺序），
     * 行缓冲会在进程退出时才刷出去，读日志的人就看不到"卡在哪一步"。 */
    setvbuf(stdout,NULL,_IONBF,0);
    /* 模式由 argv[1] 拼出来：前缀 udp- 决定报文语义，后缀 server 决定谁是监听方。 */
    int udp = !strncmp(argv[1],"udp-",4);
    int server = strstr(argv[1],"server") != NULL;
    int s = socket(AF_INET,udp?SOCK_DGRAM:SOCK_STREAM,0);
    if (s<0) die("socket");
    struct sockaddr_in a=address(argv[2],atoi(argv[3]));
    if (server) {
        /* SO_REUSEADDR：上一轮的连接还停在 TIME_WAIT 时，端口仍能绑上，
         * 否则连续跑几次实验会一直撞 EADDRINUSE。 */
        int yes=1; if (setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes)) die("reuseaddr");
        if (bind(s,(struct sockaddr *)&a,sizeof a)) die("bind");
        /* 只接一条连接：实验只需要一对进程，多路复用留给后面 socket 那篇。 */
        if (!udp && listen(s,1)) die("listen");
        printf("ready %s %s:%s\n",udp?"UDP":"TCP",argv[2],argv[3]);
        /* accept 之后立刻关掉监听 fd，后续只用同一个 fd 收发：
         * strace 里看到的 read/write 全部落在同一个 fd 上，好读。 */
        if (!udp) { int c=accept(s,NULL,NULL); if(c<0) die("accept"); close(s); s=c; }
        char buf[4096]; size_t total=0;
        if (udp) {
            /* 第四个参数是每次最多取多少字节，默认 3；第五个才是先睡多久。
             * MSG_TRUNC 让 recv 返回整条报文的长度，缓冲区里只放得下前 rcv 个字节，
             * 于是"本来多大、拿到多少"能一次打印出来。 */
            int rcv = argc>4 ? atoi(argv[4]) : 3;
            if (rcv < 1) rcv = 1;
            if (rcv > (int)sizeof buf) rcv = (int)sizeof buf;
            if (argc>5) pause_ms(atoi(argv[5]));
            for (int i=0;i<2;i++) {
                ssize_t n=recv(s,buf,(size_t)rcv,MSG_TRUNC); if(n<0) die("recv");
                int copied = n<rcv ? (int)n : rcv;
                printf("recv[%d] len=%zd copied=%d content=%.*s\n",i,n,copied,copied,buf);
            }
        } else {
            /* 先睡一会儿再读。作用是让"数据已经到了、应用还没读"这个状态
             * 存续一段时间：tcpdump、ss -mni、bpftrace 都在这个窗口里能抓到东西。 */
            if (argc>4) pause_ms(atoi(argv[4]));
            /* TCP 一直读到 EOF（对端 close 或 shutdown），只打印总字节数。
             * 不按"读几次"打印，是因为字节流没有边界：一次 read 可能拿到
             * 1 字节，也可能拿到 5 字节，取决于到达时机与缓冲。 */
            ssize_t n;
            while ((n=read(s,buf,sizeof buf))>0) total+=(size_t)n;
            if(n<0) die("read");
            printf("received=%zu bytes\n",total);
        }
    } else {
        if(connect(s,(struct sockaddr *)&a,sizeof a)) die("connect");
        if (udp) {
            /* 两条独立报文，中间没做任何合并：接收端若只给 3 字节缓冲，
             * 第二条 2 字节能收到，第一条会被截断。 */
            if(send(s,"ABCDEF",6,0)!=6 || send(s,"FG",2,0)!=2) die("send");
            puts("sent datagrams: 6 bytes, 2 bytes");
        } else if (!strcmp(argv[1],"tcp-fill")) {
            /* 造"发送队列满"：先把 socket 发送缓冲压小（内核会把设定值翻倍），
             * 再切成非阻塞，然后一直写。send 返回 EAGAIN 就说明内核已经收不下，
             * 这些字节开始堆在 socket 队列和 qdisc 里，此时：
             *   ss -mni        能看到 send-q 不再为 0
             *   tc -s qdisc    能看到 backlog 与 drops 增长
             * 随后持有 fd 不动 4 秒，给外部命令留观察窗口。 */
            int size=16384; if(setsockopt(s,SOL_SOCKET,SO_SNDBUF,&size,sizeof size)) die("sndbuf");
            int flags=fcntl(s,F_GETFL); if(flags<0 || fcntl(s,F_SETFL,flags|O_NONBLOCK)<0) die("fcntl");
            char buf[4096]; memset(buf,'X',sizeof buf); size_t total=0; ssize_t n;
            /* MSG_NOSIGNAL：对端先关时不要用 SIGPIPE 把进程打死，要的是 errno。 */
            while((n=send(s,buf,sizeof buf,MSG_NOSIGNAL))>0) total+=(size_t)n;
            if(errno!=EAGAIN && errno!=EWOULDBLOCK) die("send");
            int hold = argc>4 ? atoi(argv[4]) : 4000;
            printf("accepted=%zu bytes; send=EAGAIN; holding fd %d ms\n",total,hold);
            pause_ms(hold);
        } else {
            /* 普通客户端：写 bytes 个 'A'（默认 5）。
             * 注意 write 返回的是"内核收下了多少"，不是"对端收到了多少"：
             * 返回值正常而抓不到包，问题在 tcp_write_xmit 之后的某一站。 */
            size_t left=argc>4?strtoull(argv[4],NULL,10):5, total=left;
            char buf[4096]; memset(buf,'A',sizeof buf);
            double t=now();
            while(left) { size_t want=left<sizeof buf?left:sizeof buf; ssize_t n=write(s,buf,want);
                if(n<0) { if(errno==EINTR) continue; die("write"); } left-=(size_t)n; }
            printf("write accepted=%zu bytes in %.3f ms\n",total,(now()-t)*1000);
        }
    }
    if(close(s)) die("close");
    return 0;
}
