/* 探针 L（判定性）：复刻 dz 的真实顺序，找出"seq 不涨"的那一步。
 *   A: 先 sub 后 pub，无 clear_storage          （对照，预期 OK）
 *   B: 先 sub，pub 前 clear_storage（dz 的做法）  （预期坏）
 *   C: clear_storage 后**重建** sub，再加 wait-set（dz 重建路径，关键）
 */
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

static std::uint32_t seqv(const ipc::route& r)
{
    const auto t = r.read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0u;
}

static void scenario(const char* tag, bool clear_before_pub, bool rebuild_sub)
{
    const std::string name = std::string("w06probeL_") + tag + "_" + std::to_string(::getpid());
    std::printf("=== %s (clear=%d rebuild_sub=%d) ===\n", tag, static_cast<int>(clear_before_pub),
                static_cast<int>(rebuild_sub));

    auto sub = std::make_unique<ipc::route>(name.c_str(), ipc::receiver, false);
    std::this_thread::sleep_for(50ms);

    if (clear_before_pub) ipc::route::clear_storage(name.c_str());
    auto pub = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(150ms);

    if (rebuild_sub)
    {
        sub.reset();                       /* 模拟 begin_rebuild 的 release */
        sub = std::make_unique<ipc::route>(name.c_str(), ipc::receiver, false);
        std::this_thread::sleep_for(150ms);
    }

    ipc::recv_wait_set set;
    const bool added = set.add(sub->read_wait_token());
    std::printf("  add=%d sub.connected=%u sub.seq=%u pub.connected=%u\n", static_cast<int>(added),
                sub->connected_id(), seqv(*sub), pub->connected_id());

    std::this_thread::sleep_for(100ms);
    const bool sent = pub->try_send("HELLO", 100);
    const auto t_send = std::chrono::steady_clock::now();
    const bool woke = set.wait(300ms);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t_send).count();
    std::printf("  try_send=%d  wait_woke=%d after %lldus  sub.seq=%u ready=%zu\n", static_cast<int>(sent),
                static_cast<int>(woke), (long long)us, seqv(*sub), set.consume_ready().size());
    const auto d = sub->recv(50);
    std::printf("  sub.recv size=%zu\n", d.size());

    pub->clear();
    sub->clear();
    ipc::route::clear_storage(name.c_str());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    scenario("A_noclear", false, false);
    scenario("B_clear", true, false);
    scenario("C_clear_rebuild", true, true);
    return 0;
}
