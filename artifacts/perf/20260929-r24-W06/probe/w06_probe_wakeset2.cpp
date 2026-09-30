/* W06 诊断探针 D：raw libipc 层再现"长寿命 sub + 短寿命 pub"的丢消息条件。
 * 目的：把"worker 收不到"与"底层 waiter 就没被唤醒"分开。 */
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

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = (argc > 1) ? std::string(argv[1]) : std::string("drop");
    const std::string name = "w06probeD_" + std::to_string(::getpid());

    std::unique_ptr<ipc::route> sub = std::make_unique<ipc::route>(name.c_str(), ipc::receiver, false);
    std::printf("[D] sub connected=%u\n", sub->connected_id());

    /* 先让 sub 建好并等待 200ms（模拟订阅先 Ready）。 */
    std::this_thread::sleep_for(200ms);

    auto tok_pre = sub->read_wait_token();
    std::printf("[D] token(no pub yet) valid=%d\n", static_cast<int>(tok_pre.valid()));

    std::unique_ptr<ipc::route> pub = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::printf("[D] pub created connected=%u\n", pub->connected_id());
    std::this_thread::sleep_for(300ms);

    /* 关键：**在 add 之前**取 token，还是 add 之后取 token？两种都试。
     * 这里模拟 worker：add_route 时取 token。 */
    auto tok = sub->read_wait_token();
    std::printf("[D] token valid=%d seq=%u\n", static_cast<int>(tok.valid()),
                tok.sequence() ? tok.sequence()->load() : 0u);
    ipc::recv_wait_set set;
    const bool added = set.add(tok);
    std::printf("[D] add=%d seq=%u\n", static_cast<int>(added), tok.sequence() ? tok.sequence()->load() : 0u);

    /* 等待线程模拟 worker 的 wait 循环。 */
    std::atomic<bool> stop{false};
    std::atomic<int> wake_count{0};
    std::thread waiter([&] {
        while (!stop.load())
        {
            const bool r = set.wait(100ms);
            if (r)
            {
                wake_count.fetch_add(1);
                const auto ready = set.consume_ready();
                for (const auto& t : ready)
                {
                    std::printf("[D]   wait woke; ready seq=%u\n", t.sequence() ? t.sequence()->load() : 0u);
                }
            }
        }
    });

    std::this_thread::sleep_for(200ms);
    std::printf("[D] try_send=%d mode=%s\n", static_cast<int>(pub->try_send("HELLO", 100)), mode.c_str());
    if (mode == "drop")
    {
        pub.reset();
        std::printf("[D] pub destroyed immediately\n");
    }
    std::this_thread::sleep_for(500ms);
    std::printf("[D] wake_count=%d seq=%u\n", wake_count.load(), tok.sequence() ? tok.sequence()->load() : 0u);
    const auto data = sub->recv(100);
    std::printf("[D] recv size=%zu\n", data.size());
    stop.store(true);
    waiter.join();
    if (pub) pub.reset();
    sub.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}
