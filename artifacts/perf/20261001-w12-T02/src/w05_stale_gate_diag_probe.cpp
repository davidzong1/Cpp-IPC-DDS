/* T02 诊断探针：复刻门控用例 `test_w05_stale_slot_gate.cpp` 的**双臂**流程，但打印
 * 「第几条消息没收到」，用于刻画 T02 复跑中观察到的那次 99/100。
 *
 * ⛔ 本探针是**诊断**用途，不替代常驻用例判据；判据仍是 test/test_w05_stale_slot_gate[_arm].cpp。
 * 用法: ./w05_stale_gate_diag <domain> <n>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "libipc/ipc.h"
#include "libipc/shm.h"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kMsgId = 93;

struct RawControl
{
    ipc::shm::handle h;
    dzIPC::control_plane_shm::TopicControl* c{nullptr};
    bool open(const std::string& name)
    {
        if (!h.acquire(name.c_str(), sizeof(dzIPC::control_plane_shm::TopicControl), ipc::shm::open))
        {
            return false;
        }
        c = static_cast<dzIPC::control_plane_shm::TopicControl*>(h.get());
        return c != nullptr && c->magic.load() == dzIPC::control_plane_shm::kTopicControlMagic;
    }
    std::uint32_t count() const { return c->peer_count.load(std::memory_order_acquire); }
    std::uint32_t in_use(int i) const { return c->peers[i].in_use.load(std::memory_order_acquire); }
    std::uint32_t cc(int i) const { return c->peers[i].cc_id.load(std::memory_order_acquire); }
};

bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (pred()) { return true; }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

}   // namespace

int main(int argc, char** argv)
{
    const long dom = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 9700;
    const int n = (argc > 2) ? std::atoi(argv[2]) : 100;

    static int seq = 0;
    const std::string topic = "w05diag_" + std::to_string(dom) + "_" + std::to_string(seq++);
    const std::string seg = shm_topic_control_name(topic, 0);
    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const bool fb = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    std::printf("arm=%s topic=%s n=%d\n", fb ? "compat" : "process-scheduler", topic.c_str(), n);

    int rc = 0;
    {
        auto td = make_td();
        dzIPC::shm::shm_pub_ipc pub{td, topic, 0, false};
        pub.InitChannel();

        int slot = -1;
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (!cp.open(seg))
            {
                std::printf("diag_fail=cp.open(seg) fs_visible=false\n");
                return 3;
            }
            const std::uint32_t gen = cp.generation();
            if (gen == 0)
            {
                std::printf("diag_fail=generation==0\n");
                return 3;
            }
            if (!cp.add_peer(gen))
            {
                std::printf("diag_fail=add_peer\n");
                return 3;
            }
            slot = cp.acquire_peer_slot(gen, 1u);
            if (slot < 0)
            {
                std::printf("diag_fail=acquire_peer_slot\n");
                return 3;
            }
            cp.remove_peer(gen);
        }
        {
            RawControl r;
            if (!r.open(seg) || r.count() != 0u || r.in_use(slot) != 1u) { return 3; }
            std::printf("crafted slot=%d cc=%u in_use=%u count=%u\n", slot, r.cc(slot), r.in_use(slot), r.count());
        }
        const auto t0 = Clock::now();
        const bool reaped = wait_for([&]
                                     {
                                         RawControl r;
                                         return r.open(seg) && r.in_use(slot) == 0u;
                                     },
                                     6000);
        const auto reap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        std::printf("reaped=%d after_ms=%lld\n", (int)reaped, (long long)reap_ms);
        if (!reaped) { rc = 1; }

        auto sub_td = make_td();
        dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
        sub.InitChannel();
        if (!wait_for([&]
                      {
                          RawControl r;
                          return r.open(seg) && r.count() >= 1u;
                      },
                      3000))
        {
            return 3;
        }
        {   /* 观察活订阅者占的槽位/cc_id，以及旧槽位是否被复用 */
            RawControl r;
            if (r.open(seg))
            {
                std::printf("live_slots:");
                for (int i = 0; i < 8; ++i)
                {
                    if (r.in_use(i)) { std::printf(" [%d]=cc%u", i, r.cc(i)); }
                }
                std::printf("\n");
            }
        }

        int rx = 0;
        int first_miss = -1;
        for (int i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "diag-" + std::to_string(i);
            if (!pub.publish(m)) { std::printf("publish_failed_at=%d\n", i); rc = 2; break; }
            const auto deadline = Clock::now() + 200ms;
            bool got = false;
            while (Clock::now() < deadline)
            {
                auto sink = make_td();
                if (sub.try_get_clone(sink)) { ++rx; got = true; break; }
                std::this_thread::sleep_for(1ms);
            }
            if (!got)
            {
                if (first_miss < 0) { first_miss = i; }
                std::printf("miss_at=%d\n", i);
            }
        }
        std::printf("n=%d received=%d first_miss=%d\n", n, rx, first_miss);
        if (rx != n) { rc = 1; }
        {
            RawControl r;
            if (r.open(seg)) { std::printf("final count=%u\n", r.count()); }
        }
    }
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(seg.c_str());
    std::printf("T02_STALE_GATE_DIAG_DONE rc=%d\n", rc);
    return rc;
}
