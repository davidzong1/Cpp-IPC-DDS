/* 最小 futex 判定（含 heap 对照）：
 *   case H  : 词在堆上（匿名内存）
 *   case T1 : 词在 /dev/shm 文件映射 m1 上，wake 打到 **同一个地址**
 *   case T2 : 词在 m1 上，wake 打到同文件的 **另一映射 m2**
 * 每个 case：线程 arm futex_waitv(val=当前值) → 主线程 300ms 后 futex_wake → 打印 woken 与 waiter 返回码。 */
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

constexpr long kWaitv = 449;
constexpr std::uint32_t kFutex32 = 2;
struct futex_waitv_abi { std::uint64_t val, uaddr; std::uint32_t flags, reserved; };

static void arm_and_wake(const char* tag, std::atomic<std::uint32_t>* armed_on,
                         std::atomic<std::uint32_t>* wake_on)
{
    std::atomic<bool> armed{false};
    std::atomic<long> rc{99};
    std::atomic<int> err{0};
    std::thread t([&] {
        timespec ts{2, 0};
        futex_waitv_abi item{armed_on->load(), reinterpret_cast<std::uint64_t>(armed_on), kFutex32, 0};
        armed.store(true);
        errno = 0;
        const long r = ::syscall(kWaitv, &item, 1, 0, &ts, CLOCK_MONOTONIC);
        rc.store(r);
        err.store(errno);
    });
    while (!armed.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const int woken = static_cast<int>(::syscall(SYS_futex, wake_on, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
    t.join();
    const char* verdict = (rc.load() == 0) ? "WOKEN"
                        : (err.load() == EAGAIN) ? "EAGAIN(未睡着)"
                        : (err.load() == ETIMEDOUT) ? "ETIMEDOUT(唤醒没到)"
                        : "other";
    std::printf("[FM] %-28s arm@%p wake@%p woken=%d rc=%ld errno=%d => %s\n", tag, (void*)armed_on,
                (void*)wake_on, woken, rc.load(), err.load(), verdict);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    static std::atomic<std::uint32_t> heap_word{5};
    arm_and_wake("H heap same-addr", &heap_word, &heap_word);

    const char* path = "/dev/shm/w06_futex_min";
    ::unlink(path);
    const int fd = ::open(path, O_CREAT | O_RDWR, 0600);
    ::ftruncate(fd, 4096);
    auto* m1 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto* m2 = static_cast<std::atomic<std::uint32_t>*>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    m1->store(5);
    arm_and_wake("T1 tmpfs same-addr", m1, m1);
    m1->store(5);
    arm_and_wake("T2 tmpfs other-map", m1, m2);
    /* 对照：wake 之前先写新值（模拟 notify 的 fetch_add），看 waiter 是否靠 EAGAIN 返回。 */
    {
        std::atomic<bool> armed{false};
        std::atomic<long> rc{99};
        std::atomic<int> err{0};
        std::atomic<long long> lat{-1};
        std::thread t([&] {
            timespec ts{2, 0};
            futex_waitv_abi item{m1->load(), reinterpret_cast<std::uint64_t>(m1), kFutex32, 0};
            armed.store(true);
            const auto t0 = std::chrono::steady_clock::now();
            errno = 0;
            const long r = ::syscall(kWaitv, &item, 1, 0, &ts, CLOCK_MONOTONIC);
            rc.store(r);
            err.store(errno);
            lat.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count());
        });
        while (!armed.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        m1->fetch_add(1);   /* fetch_add 后 val 变了：再 waitv 会 EAGAIN，但已睡着的必须靠 wake */
        const int woken = static_cast<int>(::syscall(SYS_futex, m2, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
        std::printf("[FM] T3 fetch_add(m1)+wake(m2)   woken=%d\n", woken);
        t.join();
        std::printf("[FM] T3 waiter rc=%ld errno=%d latency=%lldus\n", rc.load(), err.load(), lat.load());
    }
    ::unlink(path);
    return 0;
}
