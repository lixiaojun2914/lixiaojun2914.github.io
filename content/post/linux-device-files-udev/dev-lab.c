#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

int main(void)
{
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd == -1) fail("open null");
    int zero_fd = open("/dev/zero", O_RDONLY);
    if (zero_fd == -1) fail("open zero");
    unsigned char buf[4];
    ssize_t written = write(null_fd, "abc", 3);
    if (written == -1) fail("write null");
    ssize_t from_null = read(null_fd, buf, sizeof(buf));
    if (from_null == -1) fail("read null");
    ssize_t from_zero = read(zero_fd, buf, sizeof(buf));
    if (from_zero == -1) fail("read zero");
    if (written != 3 || from_null != 0 || from_zero != 4) {
        fputs("unexpected device result\n", stderr);
        return EXIT_FAILURE;
    }
    printf("null: write=%zd read=%zd\n", written, from_null);
    printf("zero: read=%zd bytes=%02x %02x %02x %02x\n", from_zero,
           (unsigned int)buf[0], (unsigned int)buf[1],
           (unsigned int)buf[2], (unsigned int)buf[3]);
    int a = close(null_fd);
    int b = close(zero_fd);
    if (a == -1 || b == -1) fail("close");
    return EXIT_SUCCESS;
}
