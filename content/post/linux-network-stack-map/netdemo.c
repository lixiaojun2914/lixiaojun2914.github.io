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

static void die(const char *s) { perror(s); exit(1); }
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime");
    return t.tv_sec + t.tv_nsec / 1e9;
}
static struct sockaddr_in address(const char *ip, int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { fputs("bad IPv4\n",stderr); exit(1); }
    return a;
}
static void pause_ms(int ms) {
    struct timespec t = {.tv_sec=ms/1000, .tv_nsec=(ms%1000)*1000000L};
    while (nanosleep(&t,&t) && errno==EINTR) {}
}
int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,"usage: %s tcp-server|tcp-client|tcp-fill|udp-server|udp-client IP PORT [delay_ms|bytes]\n",argv[0]);
        return 2;
    }
    setvbuf(stdout,NULL,_IONBF,0);
    int udp = !strncmp(argv[1],"udp-",4);
    int server = strstr(argv[1],"server") != NULL;
    int s = socket(AF_INET,udp?SOCK_DGRAM:SOCK_STREAM,0);
    if (s<0) die("socket");
    struct sockaddr_in a=address(argv[2],atoi(argv[3]));
    if (server) {
        int yes=1; if (setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes)) die("reuseaddr");
        if (bind(s,(struct sockaddr *)&a,sizeof a)) die("bind");
        if (!udp && listen(s,1)) die("listen");
        printf("ready %s %s:%s\n",udp?"UDP":"TCP",argv[2],argv[3]);
        if (!udp) { int c=accept(s,NULL,NULL); if(c<0) die("accept"); close(s); s=c; }
        if (argc>4) pause_ms(atoi(argv[4]));
        char buf[4096]; size_t total=0;
        if (udp) {
            for (int i=0;i<2;i++) {
                ssize_t n=recv(s,buf,3,0); if(n<0) die("recv");
                printf("recv[%d]=%zd content=%.*s\n",i,n,(int)n,buf);
            }
        } else {
            ssize_t n;
            while ((n=read(s,buf,sizeof buf))>0) total+=(size_t)n;
            if(n<0) die("read");
            printf("received=%zu bytes\n",total);
        }
    } else {
        if(connect(s,(struct sockaddr *)&a,sizeof a)) die("connect");
        if (udp) {
            if(send(s,"ABCDE",5,0)!=5 || send(s,"FG",2,0)!=2) die("send");
            puts("sent datagrams: 5 bytes, 2 bytes");
        } else if (!strcmp(argv[1],"tcp-fill")) {
            int size=16384; if(setsockopt(s,SOL_SOCKET,SO_SNDBUF,&size,sizeof size)) die("sndbuf");
            int flags=fcntl(s,F_GETFL); if(flags<0 || fcntl(s,F_SETFL,flags|O_NONBLOCK)<0) die("fcntl");
            char buf[4096]; memset(buf,'X',sizeof buf); size_t total=0; ssize_t n;
            while((n=send(s,buf,sizeof buf,MSG_NOSIGNAL))>0) total+=(size_t)n;
            if(errno!=EAGAIN && errno!=EWOULDBLOCK) die("send");
            printf("accepted=%zu bytes; send=EAGAIN; holding fd 4000 ms\n",total);
            pause_ms(4000);
        } else {
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
