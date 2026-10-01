/* T04（W12 §6 成员三 / 接收池负责人）独立复核探针：真实故障窗口端到端
 *
 * 与 test_w05_stale_slot_gate*.cpp **独立实现**（不链接 gtest、不复用其构建树）：
 *   ① 造状态：add_peer → acquire_peer_slot → remove_peer（**故意不** release_peer_slot）
 *      ⇒ 共享段留下 `peer_count == 0` + `slot.in_use == 1` + 心跳陈旧；
 *   ② 逐毫秒采样原始段，记录 `slot.in_use` 何时变 0（**判据①回收**，含实测耗时）；
 *   ③ 挂真订阅者 + 发布 N 条，逐条等收（**判据②活订阅者不被误断**），打印 rx/N；
 *   ④ `nocraft` 模式 = 干净对照（无陈旧槽位，同一收发协议）。
 *
 * 段名纪律：topic 前缀 `t04gate_`（⇒ 不会与其它成员在跑的
 * 门控用例互相摧毁段）；退出前清数据段 + 控制面段。
 *
 * usage: t04_probe <domain> <n_msgs> [nocraft] [cc_id] [hold] [expect_reap_ms]
 *   hold = 造完状态后**等待**多久才开始收（默认 0；仅在需要观察更长窗口时用）
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace {
constexpr std::uint32_t kMsgId = 93;
using Clock = std::chrono::steady_clock;

#define SAY(...)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        std::printf(__VA_ARGS__);                                                                  \
        std::fflush(stdout);                                                                       \
    } while (0)

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
    std::int64_t hb(int i) const { return c->peers[i].heartbeat_ns.load(std::memory_order_acquire); }
};

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void clear_segs(const std::string& topic, size_t domain)
{
    ipc::route::clear_storage(shm_topic_segment_name(topic, domain).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, domain).c_str());
}
}   // namespace

int main(int argc, char** argv)
{
    const long domain = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 7700;
    const int n_msgs = argc > 2 ? std::atoi(argv[2]) : 100;
    const bool craft = !(argc > 3 && std::strcmp(argv[3], "nocraft") == 0);
    const std::uint32_t cc_id = argc > 4 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 1u;
    const int hold_ms = argc > 5 ? std::atoi(argv[5]) : 0;

    static std::atomic<int> seq{0};
    const std::string topic = "t04gate_" + std::to_string(domain) + "_" + std::to_string(seq.fetch_add(1));
    const std::string seg = shm_topic_control_name(topic, static_cast<size_t>(domain));

    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const bool l1 = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    SAY("T04_PROBE topic=%s domain=%ld n=%d craft=%d arm=%s cc_id=%u\n", topic.c_str(), domain, n_msgs,
        static_cast<int>(craft), l1 ? "L1-compat" : "default-sched", cc_id);

    auto pub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, static_cast<size_t>(domain), /*verbose=*/false};
    pub.InitChannel();

    int slot = -1;
    if (craft)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (!cp.open(seg)) { SAY("CRAFT_FAIL open_control_plane\n"); return 2; }
        const std::uint32_t gen = cp.generation();
        if (gen == 0) { SAY("CRAFT_FAIL generation_zero\n"); return 2; }
        if (!cp.add_peer(gen)) { SAY("CRAFT_FAIL add_peer\n"); return 2; }
        slot = cp.acquire_peer_slot(gen, cc_id);
        if (slot < 0) { SAY("CRAFT_FAIL acquire_peer_slot\n"); return 2; }
        cp.remove_peer(gen);   /* 只摘 peer_count；⛔ 故意不 release_peer_slot() */
        RawControl r;
        if (!r.open(seg)) { SAY("CRAFT_FAIL raw_open\n"); return 2; }
        SAY("CRAFT_DONE slot=%d peer_count=%u slot_in_use=%u slot_cc_id=%u (release_peer_slot NOT called)\n", slot,
            r.count(), r.in_use(slot), r.cc(slot));
        if (r.count() != 0u || r.in_use(slot) != 1u) { SAY("CRAFT_FAIL state_not_as_required\n"); return 2; }
    }

    if (hold_ms > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms)); }

    /* ★ 判据①：peer_count==0 期间，陈旧槽位必须在 dead_timeout（2s）量级内变为 0。
     * 采样：每 1 ms 读一次原始段，记录最后若干次 `in_use==1` 的样本（含心跳年龄）。 */
    const auto t_craft = Clock::now();
    bool reaped = false;
    int64_t reap_ms = -1;
    int64_t last_seen_age_ms = -1;
    int samples_in_use = 0;
    int64_t sample_ms = -1;
    if (craft)
    {
        for (;;)
        {
            const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_craft).count();
            RawControl r;
            if (!r.open(seg)) { SAY("REAP_FAIL raw_open_lost\n"); break; }
            const std::uint32_t iu = r.in_use(slot);
            const std::uint32_t pc = r.count();
            if (iu == 1u)
            {
                ++samples_in_use;
                const int64_t hbn = r.hb(slot);
                last_seen_age_ms = (hbn > 0) ? (now_ns() - hbn) / 1000000 : -1;
                /* 每 200 ms 打一行轨迹，另在最后 250 ms 每 25 ms 打一行 */
                if (sample_ms < 0 || elapsed - sample_ms >= 200 || (last_seen_age_ms >= 1750 && elapsed - sample_ms >= 25))
                {
                    SAY("REAP_TRACE t=%lldms peer_count=%u slot_in_use=%u hb_age=%lldms\n", static_cast<long long>(elapsed),
                        pc, iu, static_cast<long long>(last_seen_age_ms));
                    sample_ms = elapsed;
                }
            }
            else
            {
                reaped = true;
                reap_ms = elapsed;
                SAY("REAP_OK t=%lldms peer_count=%u slot_in_use=0 last_seen_hb_age=%lldms in_use_samples=%d\n",
                    static_cast<long long>(reap_ms), pc, static_cast<long long>(last_seen_age_ms), samples_in_use);
                break;
            }
            if (elapsed > 6000) { SAY("REAP_FAIL timeout_6000ms last_seen_hb_age=%lldms\n", static_cast<long long>(last_seen_age_ms)); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /* ★ 判据②：挂活订阅者 + 发 N 条，必须全收到（逐条等收，与门控用例同协议）。 */
    auto sub_td = make_td();
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, static_cast<size_t>(domain), /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();
    bool attached = false;
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(3000);
        while (Clock::now() < deadline)
        {
            RawControl r;
            if (r.open(seg) && r.count() >= 1u) { attached = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    {
        RawControl r;
        if (r.open(seg))
        {
            SAY("SUB_ATTACHED=%d peer_count=%u live_slots=", static_cast<int>(attached), r.count());
            for (int i = 0; i < 8; ++i)
                if (r.in_use(i)) SAY("[%d]=cc%u(hb_age=%lldms)", i, r.cc(i), static_cast<long long>((r.hb(i) > 0 ? (now_ns() - r.hb(i)) / 1000000 : -1)));
            SAY("\n");
        }
    }

    int rx = 0;
    int misses = 0;
    int first_miss_at = -1;
    /* v2 增补：逐条等待时长直方图（判据②的时序余量证据）。桶边界 = 门控用例的 200ms 等待窗。 */
    const long long kB[] = {1, 5, 20, 50, 100, 200};
    long long hist[7] = {0, 0, 0, 0, 0, 0, 0};
    long long max_wait_us = 0;
    long long sum_wait_us = 0;
    for (int i = 0; i < n_msgs; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "t04-" + std::to_string(i);
        if (!pub.publish(m)) { SAY("PUBLISH_FALSE at %d\n", i); break; }
        bool got = false;
        const auto t0 = Clock::now();
        const auto deadline = t0 + std::chrono::milliseconds(200);
        while (Clock::now() < deadline)
        {
            auto sink = make_td();
            if (sub.try_get_clone(sink)) { got = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const long long wait_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
        sum_wait_us += wait_us;
        if (wait_us > max_wait_us) { max_wait_us = wait_us; }
        {
            int b = 6;
            for (int k = 0; k < 6; ++k)
            {
                if (wait_us < kB[k] * 1000) { b = k; break; }
            }
            ++hist[b];
        }
        if (got) { ++rx; }
        else
        {
            ++misses;
            if (first_miss_at < 0) { first_miss_at = i; }
            SAY("MISS at index=%d rx=%d\n", i, rx);
        }
    }
    {
        RawControl r;
        if (r.open(seg))
        {
            SAY("FINAL peer_count=%u slots=", r.count());
            for (int i = 0; i < 8; ++i)
                if (r.in_use(i)) SAY("[%d]=cc%u", i, r.cc(i));
            SAY("\n");
        }
    }
    SAY("RESULT arm=%s craft=%d n=%d received=%d misses=%d first_miss_at=%d criterion1_reaped=%d reap_ms=%lld "
        "criterion2_pass=%d\n",
        l1 ? "L1-compat" : "default-sched", static_cast<int>(craft), n_msgs, rx, misses, first_miss_at,
        static_cast<int>(reaped), static_cast<long long>(reap_ms), static_cast<int>(rx == n_msgs));
    SAY("WAIT_HIST us_buckets <1ms=%lld 1-5ms=%lld 5-20ms=%lld 20-50ms=%lld 50-100ms=%lld 100-200ms=%lld "
        ">=200ms(miss)=%lld max=%lldus mean=%lldus\n",
        hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], max_wait_us,
        (n_msgs > 0 ? sum_wait_us / n_msgs : 0));
    SAY("T04_PROBE_DONE\n");

    clear_segs(topic, static_cast<size_t>(domain));
    _exit(0);
}
