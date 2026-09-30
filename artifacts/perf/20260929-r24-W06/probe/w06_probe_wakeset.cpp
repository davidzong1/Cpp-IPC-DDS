/* W06 诊断探针 C：只验 libipc 一层 —— 同名 route 的 sub 读等待 seq 是否被 pub 的 try_send 推进。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeC_" + std::to_string(::getpid());
    ipc::route pub{name.c_str(), ipc::sender, false};
    ipc::route sub{name.c_str(), ipc::receiver, false};
    auto tok = sub.read_wait_token();
    std::printf("[C] token valid=%d seq=%u connected=%u\n", static_cast<int>(tok.valid()),
                tok.sequence() ? tok.sequence()->load() : 0u, sub.connected_id());

    ipc::recv_wait_set set;
    const bool added = set.add(tok);
    std::printf("[C] set.add=%d seq=%u\n", static_cast<int>(added),
                tok.sequence() ? tok.sequence()->load() : 0u);
    if (!added)
    {
        std::printf("[C] backend unavailable; abort\n");
        return 0;
    }

    std::thread spin([&] {
        for (int i = 0; i < 30; ++i)
        {
            std::this_thread::sleep_for(50ms);
            std::printf("[C]   t+%dms seq=%u\n", (i + 1) * 50, tok.sequence()->load());
        }
    });

    std::this_thread::sleep_for(200ms);
    std::printf("[C] try_send=%d\n", static_cast<int>(pub.try_send("hello", 100)));
    std::printf("[C] after try_send seq=%u (immediate)\n", tok.sequence()->load());
    std::printf("[C] set.wait(500ms)=%d\n", static_cast<int>(set.wait(500ms)));
    std::printf("[C] ready=%zu seq=%u\n", set.consume_ready().size(), tok.sequence()->load());
    const auto data = sub.recv(100);
    std::printf("[C] recv size=%zu payload=%.*s\n", data.size(), static_cast<int>(data.size()),
                static_cast<const char*>(data.data()));
    spin.join();
    ipc::route::clear_storage(name.c_str());
    return 0;
}
