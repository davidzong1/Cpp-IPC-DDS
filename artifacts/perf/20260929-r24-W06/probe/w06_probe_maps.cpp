/* 探针 X（判定性，最终）：找出 sub 与 pub 各自的 rd_waiter seq 字**底层是哪个段文件**。
 * 用 /proc/self/map_files/<start>-<end> 反查映射对象；同一 inode ⇒ 同一 futex key。
 * 同时按 worker 的姿势 arm 一次 futex_waitv，并在 publish 时打印 futex_wake 的返回。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static std::string backing(const void* p)
{
    const auto* a = static_cast<const char*>(p);
    const char* page = a - (reinterpret_cast<std::uintptr_t>(a) % 4096);
    char path[128];
    char link[512] = {0};
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(page);
    std::snprintf(path, sizeof(path), "/proc/self/map_files/%lx-%lx", (unsigned long)start,
                  (unsigned long)(start + 4096));
    if (::readlink(path, link, sizeof(link) - 1) < 0) return std::string("<no map_files: ") + path + ">";
    /* 再通过 /proc/self/maps 找该地址所在行的 inode/dev，更直观。 */
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    std::string maps_line = "<not found in maps>";
    if (f != nullptr)
    {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f) != nullptr)
        {
            unsigned long lo = 0, hi = 0;
            if (std::sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && start >= lo && start < hi)
            {
                maps_line = buf;
                if (!maps_line.empty() && maps_line.back() == '\n') maps_line.pop_back();
                break;
            }
        }
        std::fclose(f);
    }
    return std::string("link=") + link + " | maps=" + maps_line;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeX_" + std::to_string(::getpid());

    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);

    const auto sub_tok = sub.read_wait_token();
    const auto pub_tok = pub.read_wait_token();
    std::printf("[X] sub rd_waiter seq @%p val=%u\n    %s\n", (const void*)sub_tok.sequence(),
                sub_tok.sequence()->load(), backing(sub_tok.sequence()).c_str());
    std::printf("[X] pub rd_waiter seq @%p val=%u\n    %s\n", (const void*)pub_tok.sequence(),
                pub_tok.sequence()->load(), backing(pub_tok.sequence()).c_str());

    while (!sub.recv(0).empty()) {}

    ipc::recv_wait_set set;
    set.add(sub_tok);
    std::atomic<bool> armed{false};
    std::atomic<long long> lat{-1};
    std::thread waiter([&] {
        armed.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(1000ms);
        lat.store(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count());
        std::printf("[X] wait woke=%d after %lldus\n", static_cast<int>(woke), (long long)lat.load());
    });
    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(200ms);
    const auto t_send = std::chrono::steady_clock::now();
    std::printf("[X] try_send=%d\n", static_cast<int>(pub.try_send("HELLO", 100)));
    waiter.join();
    const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t_send).count();
    std::printf("[X] send->join=%lldus latency=%lldus\n", (long long)dt, lat.load());
    std::printf("[X] verdict: %s\n",
                lat.load() < 5000 ? "同一 futex key（唤醒立即生效）" : "不同 key 或唤醒丢失（只能靠超时）");

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}
