/* 决定性：futex_waitv 的 timeout 语义 / 是否真的排队。
 *   A: timeout=NULL（无限），FUTEX_WAKE 应唤醒；
 *   B: timeout=3s，测实际等待时长；
 *   C: timeout=3s 但 flags=FUTEX_CLOCK_REALTIME。 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct wv { unsigned long long val, uaddr; unsigned flags, reserved; };
static unsigned word;
static volatile long g_rc, g_err;
static volatile long long g_us;
static int g_mode;

static long long now_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void* th(void* p)
{
    (void)p;
    struct timespec ts = {3, 0};
    struct wv it;
    it.val = word; it.uaddr = (unsigned long long)&word; it.flags = 2; it.reserved = 0;
    void* tsp = (g_mode == 0) ? NULL : (void*)&ts;
    unsigned flags = (g_mode == 2) ? 1u : 0u;   /* FUTEX_CLOCK_REALTIME */
    long long t0 = now_us();
    errno = 0;
    g_rc = syscall(449, &it, 1, flags, tsp, 1);
    g_err = errno;
    g_us = now_us() - t0;
    return NULL;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    g_mode = (argc > 1) ? atoi(argv[1]) : 0;
    word = 5;
    pthread_t t; pthread_create(&t, NULL, th, NULL);
    usleep(300000);
    errno = 0;
    long w = syscall(SYS_futex, &word, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    int alive = (g_us == 0);
    pthread_join(t, NULL);
    printf("[mini4] mode=%d (%s) wake_rc=%ld rc=%ld errno=%d waited_us=%lld queued=%s\n", g_mode,
           g_mode == 0 ? "timeout=NULL" : (g_mode == 1 ? "timeout=3s mono" : "timeout=3s realtime"), w, g_rc,
           (int)g_err, g_us, alive ? "still-waiting-at-wake" : "already-returned");
    return 0;
}
