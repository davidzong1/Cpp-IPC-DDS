/* t59 复核项 2 的**独立构造**：不复用 w06_regress --case=budget（其 kN 硬编码 5），
 * 自己写可调 N 的同族用例 + **逐条载荷顺序校验**。
 * DZIPC_SHM_RECV_BUDGET_MSGS=1 ⇒ 每轮 recv_once 后必然"预算耗尽"；若预算耗尽被实现成
 * 丢弃，就只能收到 1 条。用 StdString（与原探针同型，可比）并校验 body 顺序。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

static constexpr std::uint32_t kMsgId = 61;

static std::shared_ptr<dzIPC::TopicData> mk_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

int main(int argc, char** argv)
{
    const int n = (argc > 1) ? std::atoi(argv[1]) : 20;
    const std::size_t domain = (argc > 2) ? static_cast<std::size_t>(std::atoll(argv[2])) : 2460;
    const std::string topic = "r59budget" + std::to_string(domain);
    int sent = 0;
    std::vector<std::string> seen;
    {
        dzIPC::shm::shm_pub_ipc pub{mk_td(), topic, domain, false};
        dzIPC::shm::shm_sub_ipc sub{mk_td(), topic, domain, 64, false};
        pub.InitChannel("r59");
        sub.InitChannel("r59");
        for (int i = 0; i < 100 && !pub.has_subscribed(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (int i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "B" + std::to_string(i);
            if (pub.publish(m)) ++sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        auto sink = mk_td();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (std::chrono::steady_clock::now() < deadline && static_cast<int>(seen.size()) < sent)
        {
            if (sub.try_get_clone(sink))
            {
                auto* s = dynamic_cast<dzIPC::Msg::StdString*>(sink->topic().get());
                if (s != nullptr) seen.push_back(s->str);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        dzIPC::Sample sample;
        (void)sample;
    }
    const auto& b = dzIPC::threepools::RecvWorkerPool::instance().budget();
    bool content_ok = (static_cast<int>(seen.size()) == sent);
    for (std::size_t i = 0; i < seen.size(); ++i)
        if (seen[i] != ("B" + std::to_string(i))) { content_ok = false; break; }
    const char* req = std::getenv("DZIPC_SHM_RECV_BUDGET_MSGS");
    std::printf("case=budget_independent n=%d sent=%d got=%zu content_in_order=%d effective_max_msgs=%zu requested=%s\n",
                n, sent, seen.size(), content_ok ? 1 : 0, b.max_messages_per_route, req ? req : "(unset)");
    std::printf("verdict=%s budget_exhaustion_is_not_loss(n=%d got=%zu)\n",
                (sent == n && static_cast<int>(seen.size()) == n && content_ok) ? "PASS" : "FAIL", n, seen.size());
    return 0;
}
