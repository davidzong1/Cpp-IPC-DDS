/* 判定性收尾 C（无 EAGAIN 混淆）：同一 /dev/shm 文件的两次 MAP_SHARED 映射 m1/m2。
 *   线程 A 在 m1 上 futex_waitv（armed val = 当前值，**全程不改这个字**）。
 *   主线程分别用 m1 / m2 的地址调 futex_wake，看 A 的 syscall 返回码：
 *     0      ⇒ 真被 FUTEX_WAKE 唤醒（跨映射 key 相同）
 *     EAGAIN ⇒ 值不匹配返回（未睡着，探针无效）
 *     ETIMEDOUT ⇒ 只能超时（跨映射 key 不同 —— 关键结论）
 *   另外对照 fork 子进程：子进程 arm m1，父进程 wake m2。 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/futex.h>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <climits>
#include <unistd.h>

constexpr long kWaitv = 449;
constexpr std::uint32_t kFutex32 = 2;
struct futex_waitv_abi { std::uint64_t val, uaddr; std::uint32_t flags, reserved; };

static long raw_waitv(const std::atomic<std::uint32_t>* p, int sec)
{
    timespec ts{sec, 0};
    futex_waitv_abi item{p->load(), reinterpret_cast<std::uint64_t>(p), kFutex32, 0};
    return ::syscall(kWaitv, &item, 1, 0, &ts, CLOCK_MONOTONIC);
}
static int fwake(const std::atomic<std::uint32_t>* p)
{
    return static_cast<int>(::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}
static const char* rc_text(long rc, int err)
{
    if (rc == 0) return "WOKEN(0)";
    if (err == EAGAIN) return "EAGAIN(value mismatch)";
    if (err == ETIMEDOUT) return "ETIMEDOUT(only timeout)";
    return "other";
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = "/dev/shm/w06_futex_key_probe";
    ::unlink(path);
    const int fd = ::open(path, O_CREAT | O_RDWR, 0600);
    ::ftruncate(fd, 4096);
    auto* m1 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto* m2 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    m1->store(7);
    std::printf("[FK] m1=%p m2=%p value=%u\n", (void*)m1, (void*)m2, m1->load());

    for (int round = 1; round <= 2; ++round)
    {
        std::atomic<bool> armed{false};
        std::atomic<long> rc{99};
        std::atomic<int> err{0};
        std::thread t([&] {
            armed.store(true);
            errno = 0;
            const long r = raw_waitv(m1, 2);
            rc.store(r);
            err.store(errno);
        });
        while (!armed.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        std::this_thread::sleep_for(std::chrono::milliseconds{300});   /* 必须已睡进内核 */
        const auto* target = (round == 1) ? m2 : m1;
        std::printf("[FK] round %d: wake(%s) -> %d\n", round, target == m2 ? "m2 other map" : "m1 same map",
                    fwake(target));
        t.join();
        std::printf("[FK] round %d: waitv rc=%ld errno=%d => %s\n", round, rc.load(), err.load(),
                    rc_text(rc.load(), err.load()));
    }

    /* 跨进程对照：子进程 arm m1，父进程 wake m2。 */
    {
        int pfd[2];
        ::pipe(pfd);
        m1->store(9);
        const pid_t child = ::fork();
        if (child == 0)
        {
            ::close(pfd[0]);
            ::write(pfd[1], "R", 1);
            errno = 0;
            const long r = raw_waitv(m1, 3);
            char buf[64];
            const int n = std::snprintf(buf, sizeof(buf), "%ld %d", r, errno);
            ::write(pfd[1], buf, static_cast<std::size_t>(n));
            ::_exit(0);
        }
        ::close(pfd[1]);
        char c = 0;
        if (::read(pfd[0], &c, 1) == 1 && c == 'R')
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
            std::printf("[FK] cross-proc: wake(m2 of parent mm) -> %d\n", fwake(m2));
            char buf[64] = {0};
            const ssize_t n = ::read(pfd[0], buf, sizeof(buf) - 1);
            long r = -1;
            int e = 0;
            if (n > 0) std::sscanf(buf, "%ld %d", &r, &e);
            std::printf("[FK] cross-proc: child waitv rc=%ld errno=%d => %s\n", r, e, rc_text(r, e));
        }
        ::close(pfd[0]);
        int status = 0;
        ::waitpid(child, &status, 0);
    }
    ::unlink(path);
    return 0;
}
