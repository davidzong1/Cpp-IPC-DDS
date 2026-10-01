/* t77（队长裁定 §4.4 / R1-W05-F1）常驻回归：`peer_count()==0` + 陈旧 `in_use` 槽位
 *
 * ── 这条用例守的是什么（为什么非有不可）────────────────────────────────────
 * `collect_stale_peers()` 判死的是 **PeerSlot**，不是 `peer_count`。而产品的清理
 * 顺序是 `remove_peer()` → `release_peer_slot()` **两步**（generation 重建分支与
 * `detach_on_exit()` 同序）。进程若恰好死在两步之间，共享段就留下
 *   `peer_count == 0` + `slot.in_use == 1` + 心跳陈旧
 * —— 这正是 stale 扫描存在的理由。
 *
 * W05 曾把扫描挪到 `has_peers()` 门控之后（`peer_count()==0` 时**整条跳过**），
 * 并在注释里断言"无 peer 时扫描结果必然是 0 ⇒ 行为等价"—— 该前提为假。
 * 后果（t76 独立复现）：陈旧槽位不回收；回收被推迟到"活订阅者已挂上"之后 ⇒
 * 旧 `cc_id` 位**已被复用** ⇒ `disconnect_receivers()` **永久误断活订阅者**。
 * 最小反例只差那一行门控：W05 生效 **400 发 0 收** vs 基线 400/400。
 *
 * ── 判据（两条，都必须成立）──────────────────────────────────────────────
 *   ① **回收**：`peer_count()==0` 期间，陈旧槽位必须在 `peer_dead_timeout` 量级内
 *      被清掉（`in_use == 0`）。修法 = `has_peers()==false` 时按低频仍扫一次。
 *   ② **活订阅者不被误断**：随后挂上真实订阅者并发布 N 条，必须全收到。
 *      ⛔ 这一条是"回收"的**反向闸门**：把回收**提前**（错的方向）会让陈旧 cc_id
 *      在活订阅者复用该位之后被 disconnect ⇒ 本条变红。
 *
 * ── 反向验证（"判据的判据"，t73 方法论）──────────────────────────────────
 * 把 `shm_control_scheduler.cc` 的门控改回 `if (has_peers) { … }`（去掉 else 分支）
 * ⇒ ① 必须变红。实测见 `artifacts/perf/20261001-t77-W05-F1/`（negative/ 子目录）。
 *
 * ── 段名纪律 ──────────────────────────────────────────────────────────────
 * topic 名不含 '/'；RAII 在**所有** pub/sub 析构之后清数据段 + 控制面段。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
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

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 93;

/* 私有段探针：直接按段名 open 控制面段，读 PeerSlot（⛔ 不新建、不 unlink）。 */
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

struct TopicName
{
    std::string name;

    explicit TopicName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("w05gate_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~TopicName()
    {
        ipc::route::clear_storage(shm_topic_segment_name(name, 0).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(name, 0).c_str());
    }
};

bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

}   // namespace

/* 陈旧槽位 + peer_count==0 ⇒ 必须在判死超时量级内被回收，且活订阅者不被误断。 */
TEST(ShmControlScheduler, StaleSlotIsReapedWhenPeerCountZeroAndLiveSubscriberSurvives)
{
    TopicName tn{"gate"};
    const std::string seg = shm_topic_control_name(tn.name, 0);

    auto pub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    pub.InitChannel();

    /* ⚠️ 必须在 InitChannel **之后**造状态：InitChannel 会 begin_rebuild()
     *    （control_plane.cc 的 begin_rebuild → clear_all_peer_slots），把先造的状态清掉。 */
    int slot = -1;
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        ASSERT_TRUE(cp.open(seg)) << "打不开控制面段";
        const std::uint32_t gen = cp.generation();
        ASSERT_NE(gen, 0u);
        ASSERT_TRUE(cp.add_peer(gen)) << "add_peer 失败 —— 用例前提不成立";
        slot = cp.acquire_peer_slot(gen, /*cc_id=*/1u);
        ASSERT_GE(slot, 0) << "借不到 peer slot —— 用例前提不成立";
        /* 只摘 peer_count，**故意不** release 槽位：这正是"进程死在两步之间"的终态。 */
        cp.remove_peer(gen);
    }

    {
        RawControl r;
        ASSERT_TRUE(r.open(seg));
        ASSERT_EQ(r.count(), 0u) << "构造失败：peer_count 未归零";
        ASSERT_EQ(r.in_use(slot), 1u) << "构造失败：陈旧槽位未保持 in_use";
    }

    /* ★ 判据①：peer_count()==0 期间仍须回收（周期 = peer_dead_timeout = 2s 量级）。 */
    EXPECT_TRUE(wait_for(
                    [&]
                    {
                        RawControl r;
                        return r.open(seg) && r.in_use(slot) == 0u;
                    },
                    6000))
        << "❌ peer_count()==0 期间陈旧槽位**未被回收** —— has_peers() 为假时也必须按低频"
           "（周期 ≥ peer_dead_timeout 量级）扫一次；跳过整条扫描会让槽位/cc_id 位泄漏。";

    /* ★ 判据②：活订阅者不得被误断（发布 N 条须全收到）。 */
    auto sub_td = make_td();
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();
    ASSERT_TRUE(wait_for(
                    [&]
                    {
                        RawControl r;
                        return r.open(seg) && r.count() >= 1u;
                    },
                    3000))
        << "订阅端未完成握手 —— 用例前提不成立";

    constexpr int kMsgs = 100;
    int rx = 0;
    for (int i = 0; i < kMsgs; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "gate-" + std::to_string(i);
        ASSERT_TRUE(pub.publish(m)) << "第 " << i << " 条发布失败";
        const auto deadline = Clock::now() + 200ms;
        while (Clock::now() < deadline)
        {
            auto sink = make_td();
            if (sub.try_get_clone(sink))
            {
                ++rx;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    EXPECT_EQ(rx, kMsgs)
        << "❌ 活订阅者被误断：" << rx << "/" << kMsgs << " —— 陈旧 cc_id 位在活订阅者复用后"
           "被 disconnect_receivers() 摘掉，是「回收时机被推迟」的直接后果。";
}
