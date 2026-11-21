#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc == 2 && strcmp(argv[1], "--child") == 0) {
        printf("after exec pid=%ld ppid=%ld\n",
               (long)getpid(), (long)getppid());
        return 7;
    }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        printf("before exec pid=%ld ppid=%ld\n",
               (long)getpid(), (long)getppid());
        execl("/proc/self/exe", "lifecycle", "--child", (char *)NULL);
        perror("execl");
        _exit(127);
    }
    siginfo_t info;
    int ret;
    do {
        ret = waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT);
    } while (ret == -1 && errno == EINTR);
    if (ret == -1) {
        perror("waitid");
        return EXIT_FAILURE;
    }
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)child);
    FILE *status_file = fopen(path, "r");
    if (!status_file) {
        perror("fopen");
        return EXIT_FAILURE;
    }
    while (fgets(line, sizeof(line), status_file))
        if (strncmp(line, "State:", 6) == 0)
            printf("before reap %s", line);
    fclose(status_file);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "child did not exit normally\n");
        return EXIT_FAILURE;
    }
    printf("parent=%ld child=%ld exit=%d\n",
           (long)parent, (long)child, WEXITSTATUS(status));
    errno = 0;
    ret = access(path, F_OK);
    printf("after reap status_missing=%d\n", ret == -1 && errno == ENOENT);
    return EXIT_SUCCESS;
}
