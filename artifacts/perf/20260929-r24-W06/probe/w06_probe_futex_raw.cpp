/* 底层判定：同一 tmpfs 文件的两次 MAP_SHARED 映射，
 *   futex_waitv 在 map1 上 arm，futex_wake 打到 map2（不同虚拟地址、同 inode）
 *   是否算同一个 futex key？
 * 这决定 libipc 的"发布端 notify"能不能叫醒订阅端 wait-set。 */
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

constexpr long kWaitv = 449;
constexpr std::uint32_t kFutex32 = 2;
struct futex_waitv_abi { std::uint64_t val, uaddr; std::uint32_t flags, reserved; };

static int fwake(const std::atomic<std::uint32_t>* p)
{
    return static_cast<int>(::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = "/dev/shm/w06_futex_raw_probe";
    ::unlink(path);
    const int fd = ::open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { std::printf("open failed\n"); return 1; }
    ::ftruncate(fd, 4096);

    auto* m1 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto* m2 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (m1 == MAP_FAILED || m2 == MAP_FAILED) { std::printf("mmap failed\n"); return 1; }
    m1->store(0);
    std::printf("[RAW] fd=%d m1=%p m2=%p (same file, offset 0)\n", fd, (void*)m1, (void*)m2);

    /* A) 先自行 arm（子线程），再分别用 m2 / m1 唤醒。 */
    for (int round = 1; round <= 3; ++round)
    {
        std::atomic<bool> armed{false};
        std::atomic<long long> lat{-1};
        std::thread t([&] {
            armed.store(true);
            timespec ts{2, 0};
            futex_waitv_abi item{m1->load(), reinterpret_cast<std::uint64_t>(m1), kFutex32, 0};
            const auto t0 = std::chrono::steady_clock::now();
            ::syscall(kWaitv, &item, 1, 0, &ts, CLOCK_MONOTONIC);
            lat.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count());
        });
        while (!armed.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(200ms);   /* 保证已睡进内核 */
        const auto* target = (round == 3) ? m1 : m2;
        m1->fetch_add(1);
        std::printf("[RAW] round %d: futex_wake(%s) -> woken=%d\n", round, target == m1 ? "m1(same map)" : "m2(other map)",
                    fwake(target));
        t.join();
        std::printf("[RAW] round %d: waiter latency=%lldus (%s)\n", round, lat.load(),
                    lat.load() < 1000 ? "被唤醒" : "等到超时");
    }

    /* B) 跨进程：子进程 arm m1，父进程用 m2 唤醒。 */
    {
        std::atomic<bool> dummy{false};
        (void)dummy;
        ::munmap(m1, 4096);
        m1 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        m1->store(0);
        int pfd[2];
        ::pipe(pfd);
        const pid_t child = ::fork();
        if (child == 0)
        {
            ::close(pfd[0]);
            ::write(pfd[1], "R", 1);
            timespec ts{3, 0};
            futex_waitv_abi item{m1->load(), reinterpret_cast<std::uint64_t>(m1), kFutex32, 0};
            const auto t0 = std::chrono::steady_clock::now();
            ::syscall(kWaitv, &item, 1, 0, &ts, CLOCK_MONOTONIC);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            char buf[64];
            const int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)us);
            ::write(pfd[1], buf, static_cast<std::size_t>(n));
            ::_exit(0);
        }
        ::close(pfd[1]);
        char c = 0;
        if (::read(pfd[0], &c, 1) == 1 && c == 'R')
        {
            std::this_thread::sleep_for(500ms);
            m1->fetch_add(1);
            std::printf("[RAW] cross-proc: futex_wake(m2) -> woken=%d\n", fwake(m2));
            char buf[64] = {0};
            const ssize_t n = ::read(pfd[0], buf, sizeof(buf) - 1);
            std::printf("[RAW] cross-proc: child latency=%s us (%s)\n", buf,
                        (buf[0] != '\0' && std::atoll(buf) < 1000) ? "被唤醒" : "等到超时");
            (void)n;
        }
        ::close(pfd[0]);
        int status = 0;
        ::waitpid(child, &status, 0);
    }

    ::unlink(path);
    return 0;
}
