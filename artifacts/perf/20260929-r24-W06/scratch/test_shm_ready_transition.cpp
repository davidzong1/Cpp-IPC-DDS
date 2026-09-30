/* 阶段 2 全量测试方案 §4.3：控制面 Ready 状态转换的集成覆盖。
 *
 * 落点：docs/消息接收架构改造/阶段2_全量测试方案.md §4.3；
 *       src/dzIPC/shm_pub_sub_ipc.cc 的 sub_handshake()（Ready 分支 :657-755，
 *       非 Ready 分支 :767-790）。
 *
 * ── 覆盖了什么、没覆盖什么（照方案原文，不冒充）──────────────────────────
 * 覆盖（测试**自己驱动控制面**，用的是既有抽象，不新增只供测试的注入点）：
 *   · Ready → 非 Ready（同 generation）：订阅端走
 *     `stop_and_wake + wait_quiescent + release(current_route())` 并
 *     `remove_peer + release_peer_slot` ⇒ route 释放、peer/slot 回收、投递停止。
 *   · 非 Ready → 新 generation Ready：新 route 建立、重新 add_peer、消息接收恢复，
 *     且**没有**伪影进用户队列。
 *   · 状态转换期间不得产生用户可见的假消息（叫醒伪影必须停在守门处）。
 * **未**覆盖（报告中必须标注为间接覆盖，不得冒充集成覆盖）：
 *   · `add_peer` 第一次失败、第二次成功 —— `TopicControlPlane` 是具体类，无虚函数、
 *     无 friend、`shm_sub_ipc` 也不接受注入（include/dzIPC/shm_pub_sub_ipc.h:198）；
 *     要在 sub_handshake 的 `generation()` 读取（:655）与 `add_peer`（:684）之间强制
 *     失配只能靠竞态 ⇒ 做不到确定性。该分支的**调用序列**由 RouteSession 单测
 *     `StopThenSuccessfulRebuildReopensLeases` 建模（test_shm_route_session.cpp:465）。
 *   · `cc_id == 0`（连接位耗尽）—— 由 `test_shm_receiver_cap` 独立覆盖（33 订阅者），
 *     本文件不重复造。
 *
 * ── 判据为什么承重 ──────────────────────────────────────────────────────
 *   · `peer_count() == 0`：非 Ready 分支里 `remove_peer` 是**唯一**能让它归零的路径；
 *     删掉它，第 33 个订阅者那类"冒充在线"的假成功态就会重演（docs/shm_defect_fixes.md
 *     第 2 条）。
 *   · 「离开 Ready 后不再投递」：`release(current_route())` + `handshake_completed=false`
 *     之后收包循环的 acquire_receive 必须返回空 —— 只断言"没崩"抓不到。
 *   · 「重新 Ready 后恰好收到一条」：既挡住"没恢复"（收不到）也挡住"重复投递"
 *     （控制面槽位/连接没清干净）。
 *
 * ── 段名纪律 ────────────────────────────────────────────────────────────
 * topic 名不含 '/'；RAII 在**所有** pub/sub 析构之后清数据段 + 控制面段。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 77;

struct TopicName
{
    std::string name;

    explicit TopicName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("ready_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~TopicName()
    {
        ipc::route::clear_storage(shm_topic_segment_name(name, 0).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(name, 0).c_str());
    }

    const char* c_str() const { return name.c_str(); }
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
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

std::shared_ptr<dzIPC::Msg::StdImage> make_image(std::uint32_t w, std::uint32_t h, std::uint8_t seed)
{
    auto img = std::make_shared<dzIPC::Msg::StdImage>();
    img->set_msg_id(kMsgId);
    img->header.frame_id = "ready_frame";
    img->width = w;
    img->height = h;
    img->step = w * 3;
    img->encoding = "rgb8";
    img->data.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < img->data.size(); ++i)
    {
        img->data[i] = static_cast<std::uint8_t>((i + seed) & 0xFF);
    }
    return img;
}

/* 收包循环的"心跳"观测器：用测试缝数收包线程完成了多少次 recv 迭代。
 *
 * 为什么需要它：`WakeupArtifactCount()` 只在「收包线程**正卡在 recv 里**时被
 * disconnect」才增长。刚 attach 上就立刻 set_stopping 时，收包线程可能还在
 * `sleep(50ms)` 分支上 —— 那不是缺陷，只是**窗口没打开**。有了这个观测器，
 * "现在它确实在 recv 循环里"就成了一个可等待的因果条件，而不是靠 sleep 猜。 */
struct RecvTick
{
    std::atomic<unsigned> n{0};

    static RecvTick*& instance()
    {
        static RecvTick* p = nullptr;
        return p;
    }

    RecvTick()
    {
        instance() = this;
        dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
            if (ev.point == dzIPC::detail::SeamPoint::kAfterRecvRelease && instance() != nullptr)
            {
                instance()->n.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    ~RecvTick()
    {
        dzIPC::detail::SetSeamHook(nullptr);
        instance() = nullptr;
    }

    unsigned get() const { return n.load(std::memory_order_relaxed); }
};

/* 有界排空：返回取到的条数（上限 cap 防止无界循环）。 */
int drain(dzIPC::shm::shm_sub_ipc& sub, int cap)
{
    auto sink = make_td();
    int got = 0;
    while (wait_for([&] { return sub.try_get_clone(sink); }, 1500) && got < cap)
    {
        ++got;
    }
    return got;
}

}   // namespace

/* ═══════════ Ready → 非 Ready（同 generation）：route 释放 + peer/slot 回收 ═══════════ */
TEST(ShmReadyTransition, LeaveReadyReleasesRouteAndReclaimsPeer)
{
    TopicName tn{"leave"};
    auto pub_td = make_td();
    auto sub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(tn.name, 0))) << "打不开控制面段";
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() >= 1; }, 3000))
        << "订阅端未完成握手 —— 用例前提不成立";

    /* 链路确实通：先发一条并收到，否则下面的"不再投递"可能是"从来就不通"的伪绿。 */
    ASSERT_TRUE(pub.publish(make_image(4, 2, 0x21)));
    ASSERT_EQ(drain(sub, 8), 1) << "链路不通 —— 用例前提不成立";

    const std::uint32_t gen_before = cp.generation();
    /* ★ 被观测的一步：控制面离开 Ready（同 generation）。产品里这一步由
     *   "发布端退出/崩溃 ⇒ set_stopping" 触发（pub_handshake 收尾 :183），
     *   这里由测试直接驱动，用的是**同一个既有抽象**。 */
    cp.set_stopping();

    EXPECT_TRUE(wait_for([&] { return cp.peer_count() == 0; }, 3000))
        << "离开 Ready 后订阅端未回收 peer 登记（remove_peer 没走到）—— 这正是"
           "「连不上的订阅者冒充在线」那个缺陷的形态";
    EXPECT_EQ(cp.generation(), gen_before) << "离开 Ready 不得改 generation";

    /* 再发一条：订阅端已 release route 且 handshake_completed == false ⇒ 不得投递。 */
    ASSERT_TRUE(pub.publish(make_image(4, 2, 0x22)));
    auto sink = make_td();
    EXPECT_FALSE(wait_for([&] { return sub.try_get_clone(sink); }, 500))
        << "离开 Ready 之后仍然收到消息 —— route 没被释放或收包循环仍在用旧 route";
    EXPECT_FALSE(sub.try_get_clone(sink));

    /* 队列里不得有伪消息（离开 Ready 会 disconnect，可能叫醒在途 recv）。 */
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.tlv_accepted, 1u) << "离开 Ready 后 wire 计数被污染（多出/少掉接受）";
    EXPECT_EQ(st.defects(), 0u);
    std::printf("[ready-transition] leave_ready artifacts=%llu\n",
                (unsigned long long)dzIPC::WakeupArtifactCount());
}

/* ═══════════ 非 Ready → 新 generation Ready：新 route 建立、接收恢复 ═══════════ */
TEST(ShmReadyTransition, NewGenerationReadyRebuildsRouteAndResumes)
{
    TopicName tn{"regain"};
    auto pub_td = make_td();
    auto sub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(tn.name, 0)));
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() >= 1; }, 3000)) << "订阅端未完成握手";
    ASSERT_TRUE(pub.publish(make_image(4, 2, 0x31)));
    ASSERT_EQ(drain(sub, 8), 1) << "链路不通 —— 用例前提不成立";

    /* 先离开 Ready（走一遍完整的"停"），再以新 generation 回到 Ready。 */
    cp.set_stopping();
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() == 0; }, 3000)) << "离开 Ready 未回收 peer";

    dzIPC::ResetDzFlatRxCounters();
    const std::uint32_t gen_old = cp.generation();
    const std::uint32_t gen_new = cp.begin_rebuild();
    cp.set_ready();
    ASSERT_GT(gen_new, gen_old) << "generation 未推进 —— 用例前提不成立";

    EXPECT_TRUE(wait_for([&] { return cp.peer_count() == 1; }, 3000))
        << "重新 Ready 后订阅端未重新 attach（begin_rebuild 失败或 add_peer 未走到）";

    /* 主判据：接收恢复，且**恰好一条**（既挡"没恢复"也挡"重复投递"）。 */
    ASSERT_TRUE(pub.publish(make_image(6, 3, 0x32)));
    auto sink = make_td();
    ASSERT_TRUE(wait_for([&] { return sub.try_get_clone(sink); }, 3000))
        << "新 generation Ready 之后收不到消息 —— 重建路径把收包弄坏了";
    auto img = sink->topic()->msgcast<dzIPC::Msg::StdImage>();
    ASSERT_TRUE(img != nullptr);
    EXPECT_EQ(img->width, 6u);
    EXPECT_EQ(img->height, 3u);
    EXPECT_EQ(img->data.size(), 6u * 3u * 3u);

    int extra = 0;
    while (wait_for([&] { return sub.try_get_clone(sink); }, 300) && extra < 8)
    {
        ++extra;
    }
    EXPECT_EQ(extra, 0) << "同一条消息被重复投递 —— 控制面槽位/连接没清干净";

    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.tlv_accepted, 1u) << "重建后的接收计数不符";
    EXPECT_EQ(st.defects(), 0u);
}

/* ═══════ 状态转换全程：叫醒伪影不得成为用户可见的假消息 ═══════
 *
 * 与 test_wakeup_artifact.cpp 的 `NoPhantomMessageOnGenerationRebuild` 的区别是
 * **触发源**：那条靠 pub 析构（控制面离开 Ready），这条靠测试直接驱动
 * set_stopping + begin_rebuild，于是"离开 Ready"与"新 generation Ready"两条路径
 * 各自都经过一次 disconnect，全程**没有任何发送方发过真消息**。 */
TEST(ShmReadyTransition, NoPhantomMessageAcrossStateTransitions)
{
    TopicName tn{"phantom"};
    auto pub_td = make_td();
    auto sub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(tn.name, 0)));
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() >= 1; }, 3000)) << "订阅端未完成握手";

    dzIPC::ResetWakeupArtifactCount();
    dzIPC::ResetDzFlatRxCounters();

    /* 两轮状态转换：离开 Ready → 新 generation Ready，再来一次。
     * 每轮推进前先等收包线程**确实在 recv 循环里**（kAfterRecvRelease 计数前进），
     * 否则 disconnect 可能落在两次 recv 之间 ⇒ 不产生伪影 ⇒ 正对照失败。 */
    RecvTick ticks;
    for (int i = 0; i < 2; ++i)
    {
        const unsigned before = ticks.get();
        ASSERT_TRUE(wait_for([&] { return ticks.get() > before; }, 3000))
            << "第 " << i << " 轮：收包线程未进入 recv 循环 —— 用例前提不成立";
        cp.set_stopping();
        ASSERT_TRUE(wait_for([&] { return cp.peer_count() == 0; }, 3000)) << "第 " << i << " 轮离开 Ready 未回收";
        cp.begin_rebuild();
        cp.set_ready();
        ASSERT_TRUE(wait_for([&] { return cp.peer_count() == 1; }, 3000)) << "第 " << i << " 轮重新 attach 失败";
    }

    /* 主判据 A：无发送方 ⇒ 用户队列必须为空。 */
    auto sink = make_td();
    int phantoms = 0;
    while (wait_for([&] { return sub.try_get_clone(sink); }, 500) && phantoms < 8)
    {
        ++phantoms;
    }
    EXPECT_EQ(phantoms, 0) << "状态转换期间叫醒伪影被投递进了 msg_queue_（用户可见的假消息）";

    /* 主判据 B：伪影必须停在守门处，不得污染 wire 计数。 */
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.tlv_accepted, 0u) << "伪影被记成「正常收下」";
    EXPECT_EQ(st.tlv_id_skipped, 0u) << "伪影被记成「msg_id 不符」—— 缺陷被伪装成正常过滤";
    EXPECT_EQ(st.defects(), 0u);

    /* 主判据 C（正对照）：门确实被走到过，否则 A/B 是"没产生伪影"的伪绿。 */
    EXPECT_GT(dzIPC::WakeupArtifactCount(), 0u)
        << "两轮状态转换都没观测到叫醒伪影 ⇒ 本用例前提不成立（收包线程没卡在 recv 里），"
           "主判据 A/B 因此没有牙";
}
