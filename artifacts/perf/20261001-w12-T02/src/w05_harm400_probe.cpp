/* t58: harm chain of the has_peers()-gated stale scan.
 * Craft "peer_count==0 but PeerSlot.in_use==1 holding an old cc_id" (reachable
 * when a subscriber dies between remove_peer and release_peer_slot), hold it for
 * >2x peer_dead_timeout, then attach a real subscriber and publish.  The old slot
 * keeps a stale cc_id bit; when the gate finally reaps it (has_peers() true again)
 * the publisher issues disconnect_receivers(stale_cc_id) - if that bit has been
 * reused by the live subscriber, the live subscriber loses messages.
 * usage: t58_gate_harm_probe <domain> <publish_n>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "libipc/shm.h"
#include "ipc_msg/std_msgs/std_string.hpp"
using Clock = std::chrono::steady_clock;
using dzIPC::control_plane_shm::TopicControl;
static constexpr std::uint32_t kMsgId = 93;
#define SAY(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)
struct Raw
{
    ipc::shm::handle h; TopicControl* c{nullptr};
    bool open(const std::string& n)
    {
        if (!h.acquire(n.c_str(), sizeof(TopicControl), ipc::shm::open)) return false;
        c = static_cast<TopicControl*>(h.get());
        return c && c->magic.load() == dzIPC::control_plane_shm::kTopicControlMagic;
    }
    unsigned in_use(int i) const { return c->peers[i].in_use.load(std::memory_order_acquire); }
    unsigned count() const { return c->peer_count.load(std::memory_order_acquire); }
    unsigned cc(int i) const { return c->peers[i].cc_id.load(std::memory_order_acquire); }
};
int main(int argc, char** argv)
{
    const long dom = argc > 1 ? strtol(argv[1], nullptr, 10) : 8899;
    const int pub_n = argc > 2 ? atoi(argv[2]) : 200;
    const int hold_ms = argc > 3 ? atoi(argv[3]) : 4500;
    const bool craft = !(argc > 4 && std::string(argv[4]) == "nocraft");
    const std::string topic = "t58harm_" + std::to_string(dom) + "_0";
    const std::string seg = shm_topic_control_name(topic, (size_t)dom);
    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const bool fb = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    SAY("arm=%s dom=%ld craft=%d\n", fb ? "compat" : "process-scheduler", dom, (int)craft);

    auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub(td, topic, (size_t)dom, true);
    pub.InitChannel("t58harm");
    if (craft)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (!cp.open(seg)) { SAY("open_fail\n"); return 2; }
        const uint32_t gen = cp.generation();
        if (!cp.add_peer(gen)) { SAY("add_peer_fail\n"); return 3; }
        const int slot = cp.acquire_peer_slot(gen, 1u);
        cp.remove_peer(gen);
        Raw r; if (!r.open(seg)) { SAY("raw_open_fail\n"); return 4; }
        SAY("crafted peer_count=%u slot=%d in_use=%u cc_id=%u\n", r.count(), slot, r.in_use(0), r.cc(0));
        if (slot < 0 || r.count() != 0 || r.in_use(0) == 0) { SAY("craft_failed\n"); return 5; }
    }
    if (craft)
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    { Raw r; r.open(seg); SAY("after_hold(%dms) peer_count=%u slot_in_use=%u cc=%u\n", hold_ms, r.count(), r.in_use(0), r.cc(0)); }

    dzIPC::shm::shm_sub_ipc sub(td, topic, (size_t)dom, false);
    sub.InitChannel("t58harm");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {   /* 找出真实订阅者占用的槽位与 cc_id（用于判定 reap 是否打到活订阅者） */
        Raw r;
        if (r.open(seg))
        {
            SAY("live_slots:");
            for (int i = 0; i < 8; ++i)
                if (r.in_use(i)) SAY(" [%d]=cc%u", i, r.cc(i));
            SAY("\n");
        }
    }
    long rx = 0, mism = 0;
    for (int i = 0; i < pub_n; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "harm-" + std::to_string(i);
        pub.publish(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
        if (sub.try_get_clone(sink))
        {
            auto* p = dynamic_cast<dzIPC::Msg::StdString*>(sink->topic().get());
            const std::string want = "harm-" + std::to_string(i);
            if (p != nullptr && p->str == want) ++rx; else ++mism;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    { Raw r; if (r.open(seg)) SAY("final peer_count=%u slot_in_use=%u cc=%u\n", r.count(), r.in_use(0), r.cc(0)); }
    SAY("published=%d received=%ld mismatch=%ld\n", pub_n, rx, mism);
    SAY("T58_HARM_DONE\n");
    _exit(0);
}
