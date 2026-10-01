/* t64 独立最小反事实（与被审代码无关的 20 行件）：证明 FD_SET 在 fd>=FD_SETSIZE 时崩溃、poll 不崩。
 * 编译: gcc -O2 -D_FORTIFY_SOURCE=2 -o fdset_probe fdset_probe.c
 * 用法: ./fdset_probe fdset   (期望 rc=134 SIGABRT + "*** buffer overflow detected ***")
 *       ./fdset_probe poll    (期望 rc=0 且打印 poll rc) */
#define _GNU_SOURCE
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char** argv)
{
    int fd = -1;
    while ((fd = open("/dev/null", O_RDONLY)) < 1024) { }
    printf("fd=%d FD_SETSIZE=%d\n", fd, FD_SETSIZE);
    fflush(stdout);
    if (argc > 1 && strcmp(argv[1], "fdset") == 0)
    {
        fd_set s; FD_ZERO(&s); FD_SET(fd, &s);
        printf("FD_SET(fd>=FD_SETSIZE) 未崩溃 —— 与 W07 的越界声明不符\n");
        return 0;
    }
    struct pollfd p; p.fd = fd; p.events = POLLIN; p.revents = 0;
    printf("poll(fd>=1024) rc=%d ⇒ 无越界\n", poll(&p, 1, 0));
    return 0;
}
