/* 精确刻画 futex_waitv 与 FUTEX_WAKE 的配对条件（同进程、同一字）。 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static unsigned word;
static volatile long g_rc, g_err;
static volatile long long g_us;

struct args { int mode; };

static long long now_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void* th(void* pv)
{
    struct args* a = (struct args*)pv;
    struct timespec ts = {3, 0};
    long long t0 = now_us();
    errno = 0;
    if (a->mode == 0)
    {
        g_rc = syscall(SYS_futex, &word, FUTEX_WAIT, word, &ts, NULL, 0);
    }
    else
    {
        struct { unsigned long long val, uaddr; unsigned flags, reserved; } it;
        it.val = word;
        it.uaddr = (unsigned long long)&word;
        it.flags = (a->mode == 1) ? 2u : (a->mode == 2 ? (2u | 128u) : 2u);
        it.reserved = 0;
        g_rc = syscall(449, &it, 1, 0, &ts, 1);
    }
    g_err = errno;
    g_us = now_us() - t0;
    return NULL;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int mode = (argc > 1) ? atoi(argv[1]) : 1;
    word = 5;
    static struct args a;
    a.mode = mode;
    pthread_t t;
    pthread_create(&t, NULL, th, &a);
    usleep(300000);
    errno = 0;
    long w;
    if (mode == 2) w = syscall(SYS_futex, &word, FUTEX_WAKE | 128, 0x7fffffff, NULL, NULL, 0);
    else           w = syscall(SYS_futex, &word, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    long long t_wake = now_us();
    pthread_join(t, NULL);
    const char* name = (mode == 0) ? "FUTEX_WAIT vs WAKE"
                     : (mode == 1) ? "futex_waitv(FUTEX_32) vs WAKE"
                                   : "futex_waitv(FUTEX_32|PRIVATE) vs WAKE|PRIVATE";
    printf("[mini3] %-42s wake_rc=%ld waiter rc=%ld errno=%d waited_us=%lld => %s\n", name, w, g_rc, (int)g_err,
           g_us, (g_rc == 0) ? "WOKEN" : (g_err == 110 && g_us > 2500000 ? "TIMED_OUT(未排队)" :
                                          (g_err == 110 ? "EARLY_ETIMEDOUT(未排队)" : "other")));
    (void)t_wake;
    return 0;
}
