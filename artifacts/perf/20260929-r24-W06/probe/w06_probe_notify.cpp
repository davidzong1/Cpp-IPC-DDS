/* 判定性收尾 G：libipc 的"发布端 notify"到底有没有推进订阅端那条 seq 字？
 *   · 用**阻塞 FUTEX_WAIT**（不是 futex_waitv）在 sub 的 rd_waiter seq 字上等 3s；
 *   · 主线程 try_send；
 *   · waiter 返回 rc=0 ⇒ notify 打到了同一个字（waitv 才是坏的那一环）；
 *     rc=-1/ETIMEDOUT ⇒ notify 打的是另一个字（sub 侧的段映射陈旧）。
 *   同时打印 try_send 前后两侧字的**值**（sub 侧 / pub 侧）。 */
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <errno.h>
#include <linux/futex.h>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"

using namespace std::chrono_literals;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeNT_" + std::to_string(::getpid());
    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);

    const auto st = sub.read_wait_token();
    const auto pt = pub.read_wait_token();
    auto* sw = const_cast<std::atomic<std::uint32_t>*>(st.sequence());
    std::printf("[NT] sub seq@%p val=%u | pub seq@%p val=%u (%s)\n", (void*)sw, sw->load(), (void*)pt.sequence(),
                pt.sequence()->load(), sw->load() == pt.sequence()->load() ? "同值" : "异值");

    std::atomic<long> rc{99};
    std::atomic<int> err{0};
    std::atomic<long long> lat{-1};
    std::thread waiter([&] {
        const auto t0 = std::chrono::steady_clock::now();
        timespec ts{3, 0};
        errno = 0;
        const long r = ::syscall(SYS_futex, sw, FUTEX_WAIT, sw->load(), &ts, nullptr, 0);
        rc.store(r);
        err.store(errno);
        lat.store(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                      .count());
    });
    std::this_thread::sleep_for(300ms);   /* 确保已睡进内核 */
    std::printf("[NT] try_send=%d | after: sub val=%u pub val=%u\n",
                static_cast<int>(pub.try_send("HELLO", 100)), sw->load(), pt.sequence()->load());
    waiter.join();
    std::printf("[NT] FUTEX_WAIT rc=%ld errno=%d latency=%lldus => %s\n", rc.load(), err.load(), lat.load(),
                rc.load() == 0 ? "notify 打到了同一个字（waitv 才是坏环）"
                               : "notify 没打到这个字（映射陈旧/另一字）");

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}
