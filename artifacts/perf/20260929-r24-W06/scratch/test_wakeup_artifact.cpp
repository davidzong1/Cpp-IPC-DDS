/* 叫醒伪影守门：disconnect 叫醒的 recv 不得向用户队列投递伪消息
 *
 * 落点：docs/消息接收架构改造/阶段2_RouteSession实现说明.md §10；
 *       dzIPC/common/wire_accept.h 的 IsWakeupArtifact。
 *
 * ── 缺陷是什么 ───────────────────────────────────────────────────────────
 * 阶段 2 的 stop_and_wake / begin_rebuild 用 route->disconnect() 叫醒卡在 recv(50)
 * 里的收包线程（说明 §5 第 4 步：不依赖 50ms 超时）。但 libipc 的叫醒路径**不返回
 * 空 buffer**：quit_waiting() 置的 quit_ 是粘性的（waiter.h:66 只在 open() 复位），
 * wait_if 对「quit 短路」与「pred 满足」返回同一个 true（waiter.h:104-121），于是
 * wait_for 返回 true 而 pop() **从未执行** ⇒ 局部 msg{} 保持值初始化 ⇒
 * r_size = data_length(64) + remain_(0) > 0 ⇒ 返回 64 字节**全零**缓冲，且
 * empty() == false。收包循环原来只判 empty()，于是：
 *   · 话题 msg_id == 0（TopicDataPtrMake<T>() 的默认值）时，AcceptWire 的 check_id
 *     （读缓冲**尾部 4 字节**，全零 ⇒ 0）恰好通过、deserialize_ok 也为真 ⇒ 一条全零
 *     假消息被 push 进 msg_queue_，用户侧 try_get_clone **在没有任何发送方的情况下**
 *     也能取到（实测 2 条）；
 *   · 话题 msg_id != 0 时虽被挡下，但计数器被记成 kTlvIdSkipped —— 一条「不是我的
 *     话题」的正常过滤，把缺陷伪装成了正常过滤。
 * 每次重建 / stop 都会触发。
 *
 * ── 本文件的两层判据（为什么这样分层）────────────────────────────────────
 * 层 1（判别本体）**确定性**：真实 route 阻塞在 recv 时 disconnect，取回那个 buffer
 *   直接判。不依赖调度时序，也不依赖 shm_sub_ipc 的线程节奏。
 *   同时钉住边界：伪影只在那**一次**被叫醒的 recv 上产生，disconnect 之后的 recv 因
 *   handle 已失效直接返回空 buffer —— 所以守门不能写成「凡 disconnect 过就当伪影」。
 * 层 2（端到端）守**用户可见结果**：真实 pub/sub + generation 重建 ⇒ 用户队列不得
 *   多出任何消息、wire 计数不得被污染、真消息不得被丢掉。
 *
 * ── 判据为什么承重（不是"跑绿即过"）──────────────────────────────────────
 *   · NoPhantomMessageOnGenerationRebuild：修复前必红（实测 msg_id==0 时用户侧取到
 *     全零假消息）；把守门删掉同样必红。
 *   · ArtifactWouldBeAcceptedAsRealMessageWithoutTheGate：证明这道门是**唯一**拦截点
 *     —— 伪影能通过 AcceptWire 的全部校验，所以不能指望分流层挡住它。
 *   · RealDzFlatOfArtifactLengthIsNotFlagged：钉住判据**不得**退化成"只扫尾 12 字节"。
 *     TestMsg 的 dzflat_size 恰好 64 == ipc::data_length，且 data4 == false 时尾 12
 *     字节恰好全零（实测）—— 只扫尾会把这条**真段**判成伪影 ⇒ 丢真消息。
 *   · RealMessageOnMsgIdZeroTopicIsStillDelivered：阴性对照。真消息的载荷长度与伪影
 *     **完全相同**（都是 64），只有内容能区分 —— 最窄的不误伤判据。
 *   · RealMessagesSurviveGenerationRebuild：守 RouteSession 的 I5（重建期间已弹出的
 *     真消息必须仍投递），防判据退化成"generation 落后就丢"。
 *
 * ── 段名纪律 ─────────────────────────────────────────────────────────────
 * topic 名不含 '/'（POSIX shm 名只允许一个前导斜杠；实测 '/a/b' 会让 shm_open 返回
 * EINVAL，route 半初始化后 recv() 段错误）。每个用例用 RAII 在**所有** pub/sub 与
 * route 对象析构之后清段（数据段 + 控制面段），避免段残留污染其它用例。
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "libipc/ipc.h"
#include "libipc/shm.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* 缺陷的触发条件：TopicDataPtrMake<T>() 的默认 msg_id 是 0，于是 TLV 的 check_id
 * （比对缓冲尾部 4 字节）对全零伪影恰好通过。 */
constexpr std::uint32_t kMsgIdZero = 0;

/* 唯一 topic 名 + 析构清段。声明在 pub/sub 与 route 对象**之前**，于是析构逆序保证：
 * 传输对象先释放、段名 guard 最后清 —— 不会 unlink 仍在使用的段。 */
struct TopicName
{
    std::string name;
    std::size_t domain{0};

    explicit TopicName(const char* tag, std::size_t d = 0)
        : domain(d)
    {
        static std::atomic<int> n{0};
        name = std::string(tag) + "_" + std::to_string(n.fetch_add(1));
    }

    ~TopicName()
    {
        /* 数据段由 ipc::route 建；控制面段由 TopicControlPlane 用 ipc::shm::handle 建
         * （段名 = 数据段名 + "_control2"），两者的清理入口不同。 */
        ipc::route::clear_storage(shm_topic_segment_name(name, domain).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(name, domain).c_str());
    }

    const char* c_str() const { return name.c_str(); }
};

/* 纯 route 的段名 guard（层 1 用）。 */
struct RouteName
{
    std::string name;

    explicit RouteName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("wakeup_rs_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~RouteName() { ipc::route::clear_storage(name.c_str()); }

    const char* c_str() const { return name.c_str(); }
};

/* 借用外部存储、析构不释放的 buffer —— 只为把已有字节喂给判别函数。 */
ipc::buffer to_buffer(std::vector<std::uint8_t>& v)
{
    return ipc::buffer(v.data(), v.size(), [](void*, std::size_t) {});
}

/* 造一条真实 route，让 recv 阻塞，再 disconnect 叫醒，取回那次 recv 的返回值。
 * 这是**唯一**能确定性拿到伪影的手段：不依赖 shm_sub_ipc 的线程节奏。 */
struct WokenRecv
{
    ipc::buff_t buf;
    long elapsed_ms{-1};
    bool blocked_before_wake{false};
};

WokenRecv recv_woken_by_disconnect(const RouteName& rn)
{
    auto r = std::make_shared<ipc::route>(rn.c_str(), ipc::receiver, /*verbose=*/false);
    WokenRecv out;

    std::atomic<bool> recv_done{false};
    std::thread recv_thread([&] {
        const auto t0 = Clock::now();
        out.buf = r->recv(2000);   /* 超时远大于叫醒延迟 */
        out.elapsed_ms =
            static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
        recv_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(200ms);
    out.blocked_before_wake = !recv_done.load(std::memory_order_acquire);
    r->disconnect();
    recv_thread.join();
    r->release();
    return out;
}

}   // namespace

/* ══════════════════════════════ 层 1：判别本体（确定性） ══════════════════════════════ */

/* 叫醒路径的返回形状：size == ipc::data_length、全零、empty() == false。
 * 这条同时是"为什么原判据挡不住"的证据。 */
TEST(WakeupArtifactDetection, DisconnectWokenRecvProducesDetectableArtifact)
{
    RouteName rn{"shape"};
    const WokenRecv w = recv_woken_by_disconnect(rn);

    /* 前置：recv 确实进入了阻塞，且确实是被叫醒的（远早于 2000ms 超时）。
     * 没有这两条，后面"伪影"可能只是别的出口的返回值。 */
    ASSERT_TRUE(w.blocked_before_wake) << "recv 未进入阻塞 —— 本用例前提不成立";
    ASSERT_LT(w.elapsed_ms, 1000) << "recv 耗时 " << w.elapsed_ms << "ms，不是被 disconnect 叫醒的";

    EXPECT_EQ(w.buf.size(), static_cast<std::size_t>(ipc::data_length))
        << "叫醒路径的返回长度应为 ipc::data_length（伪影的形状）";
    EXPECT_FALSE(w.buf.empty()) << "叫醒路径返回的**不是**空 buffer —— 这正是原判据挡不住的原因";
    EXPECT_TRUE(dzIPC::IsWakeupArtifact(w.buf))
        << "被 disconnect 叫醒的 recv 返回的全零缓冲必须被判为伪影；"
           "判不出 ⇒ 它会穿过 empty() 判空进入分流（缺陷本体）";
}

/* 为什么必须有这道门：伪影能通过分流入口的**全部**校验。
 * 这一条把"分流层自己会挡住"这个错误假设钉死。 */
TEST(WakeupArtifactDetection, ArtifactWouldBeAcceptedAsRealMessageWithoutTheGate)
{
    RouteName rn{"accept"};
    const WokenRecv w = recv_woken_by_disconnect(rn);
    ASSERT_TRUE(dzIPC::IsWakeupArtifact(w.buf)) << "用例前提不成立：未取到伪影";

    /* buff_t 是 move-only（buffer.h: 拷贝构造已删除），而 AcceptWire 收 const 引用 ⇒
     * 直接绑定 const 引用，不拷贝、也不搬走 w.buf（后面还要用它的形状做断言）。 */
    const ipc::buff_t& artifact = w.buf;
    dzIPC::ResetDzFlatRxCounters();

    dzIPC::Msg::StdString sink;
    sink.set_msg_id(kMsgIdZero);
    EXPECT_TRUE(dzIPC::AcceptWire(artifact, kMsgIdZero, sink))
        << "msg_id == 0 的话题上，伪影通过了 check_id（尾部 4 字节全零 == 0）与 deserialize_ok"
           " —— 所以分流层挡不住它，收包循环里那道门是唯一拦截点";
    EXPECT_EQ(dzIPC::DzFlatRxCounters().tlv_accepted, 1u)
        << "它还会被记成一条「正常收下」—— 计数器看不见这个缺陷";
}

/* 阴性对照 ①：真 TLV 的长度**恰好**等于伪影长度。
 * StdString 载荷 40 字节时 serialize() == 64 == ipc::data_length（实测）。 */
TEST(WakeupArtifactDetection, RealTlvOfArtifactLengthIsNotFlagged)
{
    dzIPC::Msg::StdString m;
    m.set_msg_id(kMsgIdZero);
    m.str = std::string(40, 'a');
    auto ser = m.serialize();

    ASSERT_EQ(ser.size(), static_cast<std::size_t>(ipc::data_length))
        << "用例前提：这条真 TLV 的长度必须等于 ipc::data_length";
    EXPECT_FALSE(dzIPC::IsWakeupArtifact(ser))
        << "判据不得误伤真 TLV —— 否则每一条这个长度的真消息都会被守门吃掉";
}

/* 阴性对照 ②（**判据宽窄的承重条**）：真 DZFlat 段的尾 12 字节可以全零。
 * TestMsg 的 dzflat_size == 64 == ipc::data_length，data4 == false 时尾部恰好全零。
 * 只扫尾部的实现会把这条真段判成伪影 ⇒ 丢真消息。 */
TEST(WakeupArtifactDetection, RealDzFlatOfArtifactLengthIsNotFlagged)
{
    dzIPC::Msg::TestMsg m;
    m.set_msg_id(kMsgIdZero);
    m.data4 = false;
    std::vector<std::uint8_t> seg(m.dzflat_size());
    ASSERT_EQ(seg.size(), static_cast<std::size_t>(ipc::data_length))
        << "用例前提：TestMsg 的 dzflat_size 必须等于 ipc::data_length";
    ASSERT_TRUE(m.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));

    /* 前置：段首非零（确实是 DZFlat 段），而尾 12 字节全零。 */
    bool any_nonzero = false;
    for (std::uint8_t b : seg)
    {
        if (b != 0)
        {
            any_nonzero = true;
            break;
        }
    }
    ASSERT_TRUE(any_nonzero) << "用例前提：真段不得整段全零";
    bool tail12_zero = true;
    for (std::size_t i = seg.size() - 12; i < seg.size(); ++i)
    {
        if (seg[i] != 0)
        {
            tail12_zero = false;
        }
    }
    ASSERT_TRUE(tail12_zero) << "用例前提：这条真段的尾 12 字节应为全零";

    auto buf = to_buffer(seg);
    EXPECT_FALSE(dzIPC::IsWakeupArtifact(buf))
        << "「整段全零」才是伪影的充要特征；只扫尾 12 字节会把这条**真** DZFlat 段判成伪影"
           " ⇒ 静默丢真消息（比原缺陷更难查）";
}

/* 长度守卫：判据不得扩张到「全零就当伪影」。
 * 伪影恒为 ipc::data_length；其它长度的全零缓冲（含空 buffer）一律放行 ——
 * 空 buffer 由收包循环原有的 empty() 判掉。 */
TEST(WakeupArtifactDetection, NonArtifactLengthsAreNeverFlagged)
{
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{63}, std::size_t{65}, std::size_t{128}})
    {
        std::vector<std::uint8_t> zeros(n, 0);
        auto buf = to_buffer(zeros);
        EXPECT_FALSE(dzIPC::IsWakeupArtifact(buf)) << "长度 " << n << " 的全零缓冲不该被判为伪影";
    }

    /* 超时路径的返回值：真·空 buffer。 */
    ipc::buff_t empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(dzIPC::IsWakeupArtifact(empty)) << "空 buffer 不是伪影（它由 empty() 判掉）";
}

/* 判别是**纯函数**：断言它不该有副作用（计数只由收包循环的 NoteWakeupArtifact 记）。
 * 否则单测的断言本身就会污染诊断计数。 */
TEST(WakeupArtifactDetection, DetectionItselfDoesNotTouchCounters)
{
    RouteName rn{"pure"};
    const WokenRecv w = recv_woken_by_disconnect(rn);
    ASSERT_TRUE(dzIPC::IsWakeupArtifact(w.buf));

    dzIPC::ResetWakeupArtifactCount();
    dzIPC::ResetDzFlatRxCounters();
    (void)dzIPC::IsWakeupArtifact(w.buf);
    (void)dzIPC::IsWakeupArtifact(w.buf);
    EXPECT_EQ(dzIPC::WakeupArtifactCount(), 0u) << "判别函数不得自行计数（它被单测大量调用）";
    EXPECT_EQ(dzIPC::DzFlatRxCounters().tlv_accepted, 0u);

    /* 显式计数接口必须真的记（收包循环用的就是它）。 */
    dzIPC::NoteWakeupArtifact();
    EXPECT_EQ(dzIPC::WakeupArtifactCount(), 1u);
    dzIPC::ResetWakeupArtifactCount();
    EXPECT_EQ(dzIPC::WakeupArtifactCount(), 0u);
}

/* 机理边界（守门不得过度泛化）：伪影只产生于「recv 正阻塞时被叫醒」的那**一次**。
 *
 * disconnect 会把 handle 一并清掉，此后的 recv 走 valid() 短路、立即返回**空** buffer。
 * 所以：
 *   · 诊断计数正常增长 ≈ 重建次数（每次重建最多叫醒一次），不会无限增长；
 *   · 不能把守门写成「route 被 disconnect 过 ⇒ 后续返回一律丢弃」—— 那会把
 *     disconnect 之后仍然合法的空返回当成伪影，也会让判据与实际机理脱钩
 *     （伪影的充要特征是**内容**：size == data_length 且整段全零）。
 * 这条不依赖任何线程节奏：disconnect 在 recv 之前，无并发。 */
TEST(WakeupArtifactDetection, PostDisconnectRecvReturnsEmptyNotArtifact)
{
    RouteName rn{"post_disconnect"};
    auto r = std::make_shared<ipc::route>(rn.c_str(), ipc::receiver, /*verbose=*/false);
    ASSERT_TRUE(r->valid()) << "用例前提：route 未建起来";

    r->disconnect();

    for (int i = 0; i < 3; ++i)
    {
        const auto t0 = Clock::now();
        auto b = r->recv(200);
        const auto ms =
            static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
        EXPECT_TRUE(b.empty()) << "disconnect 之后的 recv 应返回空 buffer（handle 已失效），"
                                  "而不是每叫醒一次都吐一条伪影";
        EXPECT_LT(ms, 150) << "应走 valid() 短路立即返回，而不是等满 200ms 超时";
        EXPECT_FALSE(dzIPC::IsWakeupArtifact(b)) << "空 buffer 由 empty() 判掉，不是伪影";
    }

    r->release();
}

/* ═══════════════════════════ 层 2：端到端（用户可见结果） ═══════════════════════════ */

/* 承重条：generation 重建触发的叫醒，不得让用户拿到任何消息。
 *
 * 时序：起 pub ⇒ 订阅端握手并卡进 recv(50) ⇒ 拆 pub ⇒ 控制面离开 Ready ⇒
 * sub_handshake 走 stop_and_wake（disconnect 叫醒 recv）⇒ 伪影产生。
 * 全程没有任何发送方发过真消息。 */
TEST(WakeupArtifactGate, NoPhantomMessageOnGenerationRebuild)
{
    TopicName tn{"wakeup_e2e"};

    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, tn.domain, /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();

    dzIPC::ResetWakeupArtifactCount();
    dzIPC::ResetDzFlatRxCounters();

    /* 多轮起停：单轮实测 10/10 命中（收包线程绝大部分时间在 recv(50) 里），
     * 有界轮次让"门确实被走到"这件事稳定成立，同时不引入无界等待。 */
    constexpr int kRounds = 3;
    for (int i = 0; i < kRounds; ++i)
    {
        {
            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
            dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, tn.domain, /*verbose=*/false};
            pub.InitChannel();
            std::this_thread::sleep_for(300ms);   /* 让订阅端完成握手并进入 recv 循环 */
        }
        std::this_thread::sleep_for(300ms);       /* 拆 pub ⇒ 触发 stop_and_wake */
    }

    /* 主判据 A（承重）：无发送方 ⇒ 用户队列必须为空。
     * 修复前：msg_id == 0 的话题上伪影穿过分流，这里会取到全零假消息。 */
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
    int phantoms = 0;
    while (sub.try_get_clone(sink) && phantoms < 8)
    {
        ++phantoms;
    }
    EXPECT_EQ(phantoms, 0) << "叫醒伪影被投递进了 msg_queue_（用户可见的假消息）";

    /* 主判据 B：伪影必须停在守门处，不得污染 wire 拒收计数。
     * 修复前 msg_id != 0 的话题上它被记成 kTlvIdSkipped（"不是我的话题"的正常过滤），
     * 缺陷因此不可见。 */
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.tlv_accepted, 0u) << "伪影被记成「正常收下」";
    EXPECT_EQ(st.tlv_id_skipped, 0u) << "伪影被记成「msg_id 不符」—— 缺陷被伪装成正常过滤";
    EXPECT_EQ(st.defects(), 0u);

    /* 主判据 C：门确实被走到过。否则上面两条可能是"根本没产生伪影"的伪绿。
     * （伪影只可能在订阅端已握手、收包线程卡在 recv 里时产生 —— 这解释了为什么
     * 计数在订阅端完成握手之前恒为 0。） */
    EXPECT_GT(dzIPC::WakeupArtifactCount(), 0u)
        << kRounds << " 轮重建都没观测到伪影 ⇒ 本用例前提不成立（收包线程没卡在 recv 里），"
                        "主判据 A/B 因此没有牙";
}

/* 阴性对照：守门不许把真消息一起丢掉。
 * msg_id == 0 且载荷长度**恰好等于伪影长度**（64）的真 TLV —— 只有内容能区分。 */
TEST(WakeupArtifactGate, RealMessageOnMsgIdZeroTopicIsStillDelivered)
{
    TopicName tn{"wakeup_real"};

    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, tn.domain, /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();

    {
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
        dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, tn.domain, /*verbose=*/false};
        pub.InitChannel();
        std::this_thread::sleep_for(600ms);   /* 等握手完成 */

        dzIPC::ResetWakeupArtifactCount();
        dzIPC::ResetDzFlatRxCounters();

        auto msg = std::make_shared<dzIPC::Msg::StdString>();
        msg->str = std::string(40, 'Z');      /* serialize() == 64 == ipc::data_length */
        ASSERT_TRUE(pub.publish(msg)) << "发布失败";

        /* 有界等待送达。 */
        bool got = false;
        auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
        for (int i = 0; i < 60 && !got; ++i)
        {
            std::this_thread::sleep_for(20ms);
            if (sub.try_get_clone(sink))
            {
                got = true;
            }
        }
        ASSERT_TRUE(got) << "真消息被守门吃掉了 —— 判据过宽（把长度相同、内容非零的真 TLV 也判成了伪影）";

        auto received = sink->topic()->msgcast<dzIPC::Msg::StdString>();
        ASSERT_TRUE(received) << "取回的对象不是本话题类型";
        EXPECT_EQ(received->str, std::string(40, 'Z')) << "内容被改动";

        /* ⛔ 计数断言必须钉在**这一刻**: pub 还活着、真消息刚送达, 全程没有任何
         * disconnect ⇒ 计数器必须仍是 0。
         * 放到块外（pub 已析构）就被污染: pub 析构触发的 stop_and_wake 会 disconnect
         * 叫醒卡在 recv 里的收包线程, 产生一条**正常路径**的伪影（实测该计数 = 1）。
         * 那时这条断言已无法区分「守门误伤真消息」与「重建叫醒」—— 前者的判据是
         * 块外那条 tlv_accepted == 1（误伤 ⇒ 伪影停在守门处 ⇒ 它会是 0）。 */
        EXPECT_EQ(dzIPC::WakeupArtifactCount(), 0u)
            << "真消息不得被误判为伪影（长度与伪影相同 ⇒ 只有内容能区分）";
    }

    /* 块外只留 wire 口径：真消息被**正常收下**。守门若误伤它，伪影会停在守门处、
     * 根本不进分流, 这里就是 0 —— 所以这一条同时是「守门不过宽」的最强证据。
     * ⛔ 不要在这里再断言 WakeupArtifactCount() == 0：pub 析构触发的 stop_and_wake
     * 叫醒收包线程产生的伪影是**正常路径**, 该计数正常增长 ≈ 重建次数（口径见
     * dzIPC/common/wire_accept.h）。 */
    EXPECT_EQ(dzIPC::DzFlatRxCounters().tlv_accepted, 1u) << "真消息应被正常收下并计数";
}

/* I5：重建期间已弹出的真消息必须仍投递 —— 守门不得退化成「generation 落后就丢」。
 * 两次重建各发一条真消息，两条都必须到。 */
TEST(WakeupArtifactGate, RealMessagesSurviveGenerationRebuild)
{
    TopicName tn{"wakeup_i5"};

    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, tn.domain, /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();

    /* 发一条真消息；pub 对象析构即触发一次 generation 重建（含 disconnect 叫醒）。 */
    auto publish_marker = [&](const std::string& marker) {
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
        dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, tn.domain, /*verbose=*/false};
        pub.InitChannel();
        std::this_thread::sleep_for(300ms);
        auto msg = std::make_shared<dzIPC::Msg::StdString>();
        msg->str = marker;
        return pub.publish(msg);
    };

    ASSERT_TRUE(publish_marker("BEFORE_REBUILD"));
    std::this_thread::sleep_for(400ms);          /* pub 析构 ⇒ 重建 + 叫醒 */
    ASSERT_TRUE(publish_marker("AFTER_REBUILD"));
    std::this_thread::sleep_for(600ms);

    /* 收齐两条。顺序不保证（重建会清空 route 侧的游标），只断言两条都在。 */
    std::string seen;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgIdZero);
    for (int i = 0; i < 40; ++i)
    {
        if (sub.try_get_clone(sink))
        {
            auto received = sink->topic()->msgcast<dzIPC::Msg::StdString>();
            if (received && !received->str.empty())
            {
                seen += received->str + ",";
            }
        }
        if (seen.find("BEFORE_REBUILD") != std::string::npos && seen.find("AFTER_REBUILD") != std::string::npos)
        {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    EXPECT_NE(seen.find("BEFORE_REBUILD"), std::string::npos)
        << "重建前发出的真消息丢了（seen=[" << seen << "]）—— 守门不得按 generation 丢弃已弹出的字节";
    EXPECT_NE(seen.find("AFTER_REBUILD"), std::string::npos)
        << "重建后发出的真消息丢了（seen=[" << seen << "]）";
    std::printf("[wakeup-artifact] seen=[%s] artifacts=%llu\n", seen.c_str(),
                static_cast<unsigned long long>(dzIPC::WakeupArtifactCount()));
}
