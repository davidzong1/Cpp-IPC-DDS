/* D-22 要求 1 的"可判定错误面"取证：连接显式失败后，外部**能判定的**观测面有哪些。
 *
 * 期望（都来自"失败即提前返回"这一条）：
 *   ① IpcInfoPool 里**没有**该 topic 的 SocketSub 条目 ⇒ 订阅者从未登记（外部可查台账）；
 *   ② 发布端 has_subscribed() 恒 false（发现线程以池为准）；
 *   ③ 订阅端 try_get_clone() 恒 false（没有收包路径，不可能"假装收到"）；
 *   ④ 同进程其它正常话题不受影响（失败是**局部**的，不是整片挂起）。
 * 用法: d22_error_surface <domain> <好话题名> <坏话题名> */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

namespace {
constexpr int kMsgId = 78;
std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}
std::size_t count_socket_sub(const std::string& topic)
{
    std::size_t n = 0;
    for (const auto& e : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
    {
        if (e.kind == dzIPC::info_pool::EntryKind::SocketSub && e.topic_name == topic
            && e.pid == static_cast<int32_t>(::getpid()) && e.in_use)
            ++n;
    }
    return n;
}
}  // namespace

int main(int argc, char** argv)
{
    const long dom = (argc > 1) ? std::atol(argv[1]) : 3103;
    const std::string good = (argc > 2) ? argv[2] : "d22_good_topic";
    const std::string bad = (argc > 3) ? argv[3] : "w10_socket_independent_3103_38";

    /* 好话题：正常建链。 */
    auto sub_ok = std::make_unique<dzIPC::socket::socket_sub_ipc>(td(), good, static_cast<std::size_t>(dom), 64, false);
    sub_ok->InitChannel("d22");
    auto pub_ok = std::make_unique<dzIPC::socket::socket_pub_ipc>(td(), good, static_cast<std::size_t>(dom), false);
    pub_ok->InitChannel("d22");

    /* 坏话题：端口被占 ⇒ 有界重试耗尽 ⇒ 显式失败并提前返回。 */
    const auto t0 = std::chrono::steady_clock::now();
    auto sub_bad = std::make_unique<dzIPC::socket::socket_sub_ipc>(td(), bad, static_cast<std::size_t>(dom), 64, false);
    sub_bad->InitChannel("d22");
    const auto bad_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    const std::size_t pool_good = count_socket_sub(good);
    const std::size_t pool_bad = count_socket_sub(bad);

    /* 好话题端到端必须仍然通：失败**不是**整片挂起。 */
    bool good_rx = false;
    for (int i = 0; i < 3 && !good_rx; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "d22-good";
        (void)pub_ok->publish_best_effort(m);
        const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < dl)
        {
            auto sink = td();
            if (sub_ok->try_get_clone(sink)) { good_rx = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    bool bad_rx = false;
    {
        auto sink = td();
        bad_rx = sub_bad->try_get_clone(sink);
    }

    std::printf("bad_init_ms=%lld\n", (long long)bad_ms);
    std::printf("pool_socketsub_good=%zu (期望 >0)\n", pool_good);
    std::printf("pool_socketsub_bad=%zu (期望 0 ⇒ 失败可见于台账)\n", pool_bad);
    std::printf("good_route_rx=%d (期望 1 ⇒ 失败是局部的)\n", (int)good_rx);
    std::printf("bad_route_rx=%d (期望 0 ⇒ 不假装收到)\n", (int)bad_rx);
    const bool pass = (pool_good > 0) && (pool_bad == 0) && good_rx && !bad_rx && bad_ms >= 29000 && bad_ms < 40000;
    std::printf("D22_ERROR_SURFACE verdict=%s\n", pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    _exit(pass ? 0 : 1);
}
