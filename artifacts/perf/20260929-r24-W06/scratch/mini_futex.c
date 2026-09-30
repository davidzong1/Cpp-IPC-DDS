#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
static unsigned word = 5;
static void* waiter(void* arg)
{
    struct timespec ts = {2, 0};
    struct { unsigned long long val, uaddr; unsigned flags, reserved; } item;
    item.val = word;
    item.uaddr = (unsigned long long)&word;
    item.flags = 2;
    item.reserved = 0;
    errno = 0;
    long r = syscall(449, &item, 1, 0, &ts, 1);
    printf("waiter: rc=%ld errno=%d\n", r, errno);
    return NULL;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
    usleep(300000);
    errno = 0;
    long w = syscall(SYS_futex, &word, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    printf("wake: rc=%ld errno=%d word=%u\n", w, errno, word);
    pthread_join(t, NULL);
    return 0;
}
