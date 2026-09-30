#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static unsigned word = 5;
static void* w_wait(void* a)
{
    struct timespec ts = {1, 0};
    errno = 0;
    long r = syscall(SYS_futex, &word, FUTEX_WAIT, 5, &ts, NULL, 0);
    printf("FUTEX_WAIT: rc=%ld errno=%d\n", r, errno);
    return NULL;
}
static void* w_waitv(void* a)
{
    struct timespec ts = {1, 0};
    struct { unsigned long long val, uaddr; unsigned flags, reserved; } item;
    item.val = word; item.uaddr = (unsigned long long)&word; item.flags = 2; item.reserved = 0;
    errno = 0;
    long r = syscall(449, &item, 1, 0, &ts, 1);
    printf("futex_waitv: rc=%ld errno=%d\n", r, errno);
    return NULL;
}
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t t;
    if (argc > 1 && argv[1][0] == 'v') pthread_create(&t, NULL, w_waitv, NULL);
    else pthread_create(&t, NULL, w_wait, NULL);
    usleep(300000);
    errno = 0;
    long w = syscall(SYS_futex, &word, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    printf("FUTEX_WAKE: rc=%ld errno=%d\n", w, errno);
    pthread_join(t, NULL);
    return 0;
}
