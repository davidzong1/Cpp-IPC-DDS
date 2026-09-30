/* 探针 Y（判定性收尾）：futex 唤醒到底能不能跨"同一文件的两次映射"？
 *   用例 1（同进程）: sub 与 pub 各映射一次同名 RD_CONN 段；
 *        在 sub 的映射上 arm futex_waitv（经 recv_wait_set），再从 **pub 的映射** 调 futex_wake，
 *        打印 woken；再从 **sub 的映射** 调 futex_wake，打印 woken。
 *        两者不同 ⇒ futex key 与虚拟地址/映射对象有关（同一文件也不共享）。
 *   用例 2（跨进程）: fork 子进程持 receiver 并 arm，父进程 200ms 后 send，测延迟。 */
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <linux/futex.h>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static int fwake(const std::atomic<std::uint32_t>* p)
{
    return static_cast<int>(::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    /* ---- 用例 1：同进程跨映射 ---- */
    {
        const std::string name = "w06probeY1_" + std::to_string(::getpid());
        ipc::route sub{name.c_str(), ipc::receiver, false};
        ipc::route pub{name.c_str(), ipc::sender, false};
        std::this_thread::sleep_for(200ms);
        const auto st = sub.read_wait_token();
        const auto pt = pub.read_wait_token();
        std::printf("[Y1] sub seq @%p  pub seq @%p (same inode=%s)\n", (const void*)st.sequence(),
                    (const void*)pt.sequence(), st.sequence() == pt.sequence() ? "same addr" : "different addr");

        ipc::recv_wait_set set;
        set.add(st);
        std::atomic<long long> lat{-1};
        std::atomic<bool> armed{false};
        std::thread w([&] {
            armed.store(true);
            const auto t0 = std::chrono::steady_clock::now();
            const bool woke = set.wait(3000ms);
            lat.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count());
            std::printf("[Y1] wait woke=%d after %lldus\n", static_cast<int>(woke), (long long)lat.load());
        });
        while (!armed.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(200ms);

        std::printf("[Y1] futex_wake(pub-mapping)  -> woken=%d\n", fwake(pt.sequence()));
        std::this_thread::sleep_for(50ms);
        std::printf("[Y1] futex_wake(sub-mapping)  -> woken=%d\n", fwake(st.sequence()));
        w.join();
        std::printf("[Y1] verdict: %s\n",
                    lat.load() < 1000 ? "跨映射唤醒生效" : "跨映射唤醒失败（只能等超时）");
        pub.clear();
        sub.clear();
        ipc::route::clear_storage(name.c_str());
    }

    /* ---- 用例 2：跨进程 ---- */
    {
        const std::string name = "w06probeY2_" + std::to_string(::getpid());
        int pfd[2];
        if (::pipe(pfd) != 0) return 1;
        const pid_t child = ::fork();
        if (child == 0)
        {
            ::close(pfd[0]);
            ipc::route rx{name.c_str(), ipc::receiver, false};
            ipc::recv_wait_set set;
            if (!set.add(rx.read_wait_token())) ::write(pfd[1], "X", 1);
            else
            {
                ::write(pfd[1], "R", 1);
                const auto t0 = std::chrono::steady_clock::now();
                const bool woke = set.wait(3000ms);
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0).count();
                char buf[128];
                const int n = std::snprintf(buf, sizeof(buf), "%d %lld %zu", static_cast<int>(woke), (long long)us,
                                            rx.recv(100).size());
                ::write(pfd[1], buf, static_cast<std::size_t>(n));
            }
            ::close(pfd[1]);
            ::_exit(0);
        }
        ::close(pfd[1]);
        char c = 0;
        if (::read(pfd[0], &c, 1) != 1 || c != 'R')
        {
            std::printf("[Y2] child not ready (%c)\n", c);
        }
        else
        {
            ipc::route tx{name.c_str(), ipc::sender, false};
            std::this_thread::sleep_for(300ms);   /* 确保子进程已睡进 futex_waitv */
            std::printf("[Y2] parent try_send=%d\n", static_cast<int>(tx.try_send("HELLO", 100)));
            char buf[128] = {0};
            const ssize_t n = ::read(pfd[0], buf, sizeof(buf) - 1);
            std::printf("[Y2] child reported: woke,latency_us,recv_size = %.*s\n", (int)(n > 0 ? n : 0), buf);
            tx.clear();
        }
        ::close(pfd[0]);
        int status = 0;
        ::waitpid(child, &status, 0);
        std::printf("[Y2] verdict: 若 latency_us < 1000 ⇒ 跨进程唤醒生效\n");
        ipc::route::clear_storage(name.c_str());
    }
    return 0;
}
