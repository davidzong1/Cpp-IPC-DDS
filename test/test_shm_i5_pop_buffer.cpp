/* 阶段 2 全量测试方案 §4.1：I5 的**固定事件顺序**守门 —— 已弹出的 buffer 不得因
 * generation 落后被丢。
 *
 * 落点：docs/消息接收架构改造/阶段2_全量测试方案.md §4.1；
 *       RouteSession 的 I5 定义在 include/dzIPC/shm_route_session.h:30-32。
 *
 * ── 为什么必须加钩子（不加就是没有判据）────────────────────────────────────
 * I5 的事件序列是：
 *
 *   旧 route 的 recv 已返回一条带唯一序号的非空 buffer
 *     → 在收包线程处理该 buffer 前推进 generation / 启动 rebuild
 *     → 允许收包线程继续处理
 *     → 断言该序号恰好进入 view_queue_ 或 msg_queue_
 *
 * 中间那个窗口在真实代码里只有几条指令（recv 返回 → release_receive → 分流）。
 * v4 只读验收把「在收包循环里加 generation 判断」这条变异（X2）判为
 * 「靠**无人写该判断**成立，不是测试守住的」—— 因为**没有任何**用例能在这个窗口里
 * 插入事件。方案原文也明确禁止"以随机压力碰撞窗口"。
 *
 * 本文件用 include/dzIPC/detail/shm_sub_seam.h 的 kAfterRecvRelease 点把收包线程
 * **停住**（该点已 release_receive ⇒ inflight == 0 ⇒ 重建方能推进到 §4 第 5 步
 * release 旧 route），然后确定性地推进 generation，再放行。
 *
 * ── 判据为什么承重 ──────────────────────────────────────────────────────
 *   · 若实现改成「lease.generation != 当前 generation ⇒ 丢弃」：被暂停的这条 buffer
 *     携带旧 generation，放行后它会被丢掉 ⇒ 序号不在队列里 ⇒ 红。
 *   · 若实现改成「rebuild 期间不投递」：同上红。
 *   · 承重前提（正对照）：钩子**确实**在该窗口被命中过（hook_hits > 0），且 generation
 *     在放行前**确实**已经变了（new_gen > old_gen）。没有这两条，全绿可能只是
 *     "窗口根本没被打开"的伪绿。
 *   · 内容完整性：不只是"收到一条"，而是断言宽/高/载荷逐字节等于发送端构造的那条 ——
 *     防止"投递了一条别的/损坏的 buffer"被算成通过。
 *
 * ── 段名纪律 ────────────────────────────────────────────────────────────
 * topic 名**不得含 '/'**（POSIX shm 名只允许一个前导斜杠；实测 '/a/b' 会让 shm_open
 * 返回 EINVAL，route 半初始化后 recv() 段错误）。用 `i5_<tag>_<n>`。
 * 每个用例用 RAII 在**所有** pub/sub 对象析构之后清段（数据段 + 控制面段）。
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* StdImage 的 msg_id 必须非 0：0 会让全零伪影恰好通过 TLV 的 check_id（见
 * test_wakeup_artifact.cpp 的说明），本用例要的是**真消息**的路径，避开那个已知面。 */
constexpr std::uint32_t kMsgId = 61;

/* 唯一 topic 名 + 析构清段。声明在 pub/sub 对象**之前** ⇒ 析构逆序保证
 * "传输对象先释放、段名 guard 最后清"。 */
struct TopicName
{
    std::string name;

    explicit TopicName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("i5_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~TopicName()
    {
        ipc::route::clear_storage(shm_topic_segment_name(name, 0).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(name, 0).c_str());
    }

    const char* c_str() const { return name.c_str(); }
};

/* 有界等待：所有等待都必须有界。 */
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

/* 让订阅端完成握手（handshake_completed 是私有成员，没有 getter）。
 * 判据是**行为**：控制面登记表里出现了 peer，说明订阅端已 attach 并 add_peer 过。 */
bool wait_for_peer(const std::string& topic, int timeout_ms)
{
    dzIPC::control_plane_shm::TopicControlPlane cp;
    if (!cp.open(shm_topic_control_name(topic, 0)))
    {
        return false;
    }
    return wait_for([&] { return cp.peer_count() > 0; }, timeout_ms);
}

/* DZFlat 的进程级开关（默认关，见 src/dzIPC/common/nodelet_config.cc:13）。
 * 借样路径用例必须先打开它，否则 publish_prebuilt_segment 会按"开关关"直接拒绝 ——
 * 这是**用例前提**，不是产品缺陷（test_dzflat_rx.cpp 的 DzFlatSwitch 同一手法）。 */
struct DzFlatSwitch
{
    bool prev_;
    explicit DzFlatSwitch(bool on) : prev_(dzIPC::IsDzFlatEnabled()) { dzIPC::EnableDzFlat(on); }
    ~DzFlatSwitch() { dzIPC::EnableDzFlat(prev_); }
};

/* 一条可辨识的载荷：内容随 seed 变化，用于断言"投递的就是那一条"。 */
dzIPC::Msg::StdImage make_image(std::uint32_t w, std::uint32_t h, std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "i5_frame";
    img.width = w;
    img.height = h;
    img.step = w * 3;
    img.encoding = "rgb8";
    img.data.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
    {
        img.data[i] = static_cast<std::uint8_t>((i * 7 + seed) & 0xFF);
    }
    return img;
}

/* ══════════════════ I5 的暂停闸门（确定性事件序的载体） ══════════════════
 *
 * 语义：装钩子后，**第一次**在 kAfterRecvRelease 点被命中时把收包线程停在闸门里；
 * 主线程看到"已停住"后再去推进 generation，然后放行。
 *
 * 为什么停在 kAfterRecvRelease 而不是 kAfterRecv：该点已 release_receive ⇒
 * receive_inflight_ == 0 ⇒ 并发的 begin_rebuild 第 4 步不会阻塞（钩子内阻塞是安全的，
 * 见 shm_sub_seam.h 的设计约束）。
 *
 * 为什么只停一次：第二次起直接放行，避免把收包循环整体锁死（后续还有伪影/空返回等
 * 迭代要走完）。 */
class PauseGate
{
public:
    ~PauseGate() { dzIPC::detail::SetSeamHook(nullptr); }

    void arm() { dzIPC::detail::SetSeamHook(&PauseGate::on_event); }

    /* 等收包线程真的停在闸门里。 */
    bool wait_paused(int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return paused_; });
    }

    /* 闸门里那条 buffer 的元信息（供断言"停的确实是那条真消息"）。 */
    std::size_t paused_size()
    {
        std::lock_guard<std::mutex> lock(m_);
        return paused_size_;
    }
    std::uint32_t paused_generation()
    {
        std::lock_guard<std::mutex> lock(m_);
        return paused_gen_;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        cv_.notify_all();
    }

    /* 钩子内允许阻塞 ⇒ 这里用条件变量等放行；带**兜底超时**，避免用例自身失败时
     * 把收包线程永久挂住（那会让后续 join 挂死、把有方向的失败退化成无方向的超时）。 */
    int hits()
    {
        std::lock_guard<std::mutex> lock(m_);
        return hits_;
    }

private:
    static void on_event(const dzIPC::detail::SeamEvent& ev) noexcept
    {
        if (ev.point != dzIPC::detail::SeamPoint::kAfterRecvRelease)
        {
            return;
        }
        auto* self = instance();
        if (self == nullptr)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(self->m_);
        ++self->hits_;
        if (self->paused_)
        {
            return;   /* 只停第一次 */
        }
        if (ev.size == 0)
        {
            return;   /* 空返回没有可暂停的字节；等真消息那条 */
        }
        self->paused_ = true;
        self->paused_size_ = ev.size;
        self->paused_gen_ = ev.generation;
        self->cv_.notify_all();
        self->cv_.wait_for(lock, std::chrono::seconds(10), [self] { return self->released_; });
    }

    /* 钩子是裸函数指针，没有上下文参数 ⇒ 用进程内单例（同进程内这些用例串行执行）。 */
    static PauseGate*& instance()
    {
        static PauseGate* p = nullptr;
        return p;
    }

public:
    PauseGate() { instance() = this; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool paused_{false};
    bool released_{false};
    int hits_{0};
    std::size_t paused_size_{0};
    std::uint32_t paused_gen_{0};
};

}   // namespace

/* ═════════════ TLV 路径：已弹出的 TLV 必须进 msg_queue_（物化队列） ═════════════ */
TEST(ShmI5PopBuffer, PoppedTlvSurvivesGenerationRebuild)
{
    TopicName tn{"tlv"};
    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    ASSERT_TRUE(wait_for_peer(tn.name, 3000)) << "订阅端未完成握手 —— 用例前提不成立";

    /* 1) 发一条真 TLV（StdImage 默认走 TLV 序列化；DZFlat 开关在下面那条用例里单独管）。 */
    auto src = make_image(8, 4, 0x33);
    src.set_msg_id(kMsgId);
    ASSERT_TRUE(pub.publish(std::make_shared<dzIPC::Msg::StdImage>(src)))
        << "发布失败 —— 用例前提不成立";

    /* 2) 收包线程会在 kAfterRecvRelease 处停住（recv 已返回、尚未分流）。 */
    PauseGate gate;
    gate.arm();
    ASSERT_TRUE(gate.wait_paused(3000))
        << "收包线程未在 kAfterRecvRelease 停住 ⇒ 窗口没被打开, 本用例无判据";
    const std::size_t paused_size = gate.paused_size();
    const std::uint32_t old_gen = gate.paused_generation();
    ASSERT_GT(paused_size, 0u) << "停在闸门里的应是真消息, 不是空返回";

    /* 3) 在**分流入队之前**推进 generation：测试自己驱动控制面（既有抽象，
     *    见 test_shm_receiver_cap.cpp:119 / test_sercli_auto_path.cpp:566 的用法），
     *    订阅端会在下一轮 10ms 心跳里看到新 generation 并 begin_rebuild。 */
    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(tn.name, 0))) << "打不开控制面段";
    const std::uint32_t new_gen = cp.begin_rebuild();
    cp.set_ready();
    ASSERT_GT(new_gen, old_gen) << "generation 未推进 —— 本用例的前提（跨代投递）不成立";

    /* 4) 等订阅端真的完成了重建（peers 被 begin_rebuild 清空 ⇒ 订阅端重新 attach
     *    并 add_peer ⇒ 回到 1）。用轮询而非 sleep：这是**因果**条件。 */
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() == 1; }, 3000))
        << "订阅端未在推进 generation 后重新 attach —— 重建未完成";
    /* 再给一拍，让 begin_rebuild 的第 5 步（release 旧 route）确实走完：
     * peer 登记发生在 begin_rebuild 返回**之后**，所以上面那条成立时旧 route 已 release。 */

    /* 5) 放行收包线程：它手上那条 buffer 来自**旧** generation。 */
    gate.release();

    /* 6) 主判据：这条已弹出的消息必须进 msg_queue_，且恰好一次、内容完整。 */
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    int got = 0;
    while (wait_for([&] { return sub.try_get_clone(sink); }, 2000) && got < 8)
    {
        ++got;
    }
    EXPECT_EQ(got, 1) << "已弹出的 TLV 在 generation 推进后被丢弃/重复投递 —— I5 被破坏"
                         "（generation 只可阻止**新的** recv，不得丢已弹出的字节）";

    if (got >= 1)
    {
        auto img = sink->topic()->msgcast<dzIPC::Msg::StdImage>();
        ASSERT_TRUE(img != nullptr) << "取出的对象不是 StdImage";
        EXPECT_EQ(img->width, src.width);
        EXPECT_EQ(img->height, src.height);
        EXPECT_EQ(img->data, src.data) << "载荷被改坏 —— 投递的不是发送端构造的那条";
    }

    /* 正对照：闸门确实被命中过（否则上面全绿可能只是"窗口没打开"）。 */
    EXPECT_GT(gate.hits(), 0) << "钩子从未命中 ⇒ 本用例没有打开 I5 窗口, 判据不承重";
    EXPECT_EQ(gate.paused_size(), paused_size) << "暂停点元信息被后续事件覆盖";

    /* 计数面：这条真 TLV 只应记一次 kTlvAccepted，不得混入缺陷计数。 */
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.tlv_accepted, 1u) << "真 TLV 的接收计数不符";
    EXPECT_EQ(st.defects(), 0u) << "重建过程中出现了 wire 缺陷计数";
}

/* ═══════ DZFlat 路径：已弹出的借样段必须进 view_queue_（零拷贝队列） ═══════
 *
 * 与上一条的区别是队列与生命周期：DZFlat 段被**借**进 Sample，chunk 的引用计数在
 * 用户读完字段前保持非零。这条同时覆盖方案 §3.4 的
 * 「route rebuild 不破坏仍由 Sample/用户队列持有的 buffer」—— 被暂停的这条 buff_t
 * 完整跨过了旧 route 的 release()。 */
TEST(ShmI5PopBuffer, PoppedDzFlatSurvivesGenerationRebuild)
{
    DzFlatSwitch dzflat_on{true};
    TopicName tn{"dzflat"};
    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    ASSERT_TRUE(wait_for_peer(tn.name, 3000)) << "订阅端未完成握手 —— 用例前提不成立";

    auto src = make_image(16, 8, 0x5A);
    src.set_msg_id(kMsgId);
    std::vector<std::uint8_t> seg(src.dzflat_size());
    ASSERT_TRUE(src.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));
    dzflat::SegHeader h{};
    std::memcpy(&h, seg.data(), sizeof(h));
    ASSERT_EQ(h.msg_id, kMsgId) << "段头 msg_id 必须与话题模板一致, 否则会被门拦下";

    PauseGate gate;
    gate.arm();
    ASSERT_TRUE(pub.publish_prebuilt_segment(seg.data(), seg.size()))
        << "预构造段发布失败 —— 用例前提不成立";
    ASSERT_TRUE(gate.wait_paused(3000))
        << "收包线程未在 kAfterRecvRelease 停住 ⇒ 窗口没被打开, 本用例无判据";
    const std::uint32_t old_gen = gate.paused_generation();

    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(tn.name, 0))) << "打不开控制面段";
    const std::uint32_t new_gen = cp.begin_rebuild();
    cp.set_ready();
    ASSERT_GT(new_gen, old_gen) << "generation 未推进 —— 用例前提不成立";
    ASSERT_TRUE(wait_for([&] { return cp.peer_count() == 1; }, 3000))
        << "订阅端未在推进 generation 后重新 attach —— 重建未完成";

    gate.release();

    /* 主判据：借样段必须进 view 队列，且段内容完整。 */
    dzIPC::Sample sample;
    ASSERT_TRUE(wait_for([&] { return sub.try_get(sample); }, 2000))
        << "已弹出的 DZFlat 段在 generation 推进后被丢弃 —— I5 被破坏（借样路径）";
    ASSERT_TRUE(sample.valid());
    EXPECT_EQ(sample.msg_id(), kMsgId);
    EXPECT_EQ(sample.schema_hash(), src.dzflat_schema_hash());

    /* 段字节必须与发送端写出的**逐字节**相同 —— 借样路径不许在重建中被改写/截断。
     * ⚠️ Sample::size() 是**借到的 chunk 容量**（sample_message.h 的契约），SHM 上通常
     * 大于段长；段内有效长度由段头的 total_size 决定。所以判"装得下"+"前 total_size
     * 字节逐字节相同"，不是判相等（test_dzflat_rx.cpp:381-385 同一口径）。 */
    const auto* seg_bytes = static_cast<const std::uint8_t*>(sample.data());
    ASSERT_NE(seg_bytes, nullptr);
    const std::uint32_t seg_len = h.total_size;
    ASSERT_LE(seg_len, seg.size()) << "用例构造异常：段头声明的长度超过写出的缓冲";
    EXPECT_GE(sample.size(), seg_len) << "借来的 chunk 装不下段头声明的段长";
    EXPECT_EQ(0, std::memcmp(seg_bytes, seg.data(), seg_len))
        << "借样段的内容在重建过程中被改动 —— 用户会读到坏数据";

    /* 阴性对照：不得同时冒出一条 clone 队列的消息（DZFlat 只走 view 队列）。 */
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    EXPECT_FALSE(sub.try_get_clone(sink)) << "DZFlat 段不得被物化进 clone 队列";

    EXPECT_GT(gate.hits(), 0) << "钩子从未命中 ⇒ 判据不承重";
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_EQ(st.dzflat_accepted, 1u) << "真段的接收计数不符";
    EXPECT_EQ(st.defects(), 0u);
}

/* ═══════ 阴性对照：没有 generation 推进时，同一条消息也必须恰好投递一次 ═══════
 *
 * 这条不含 rebuild，因此它不承重 I5；它的作用是钉住"上面两条断言的那个 `got == 1`
 * 不是因为 rebuild 才成立的"—— 若分流路径本身就重复投递，上面两条会红在错误的地方，
 * 让人误判成 I5 缺陷。 */
TEST(ShmI5PopBuffer, NoRebuildControlDeliversExactlyOnce)
{
    TopicName tn{"ctrl"};
    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, tn.name, 0, /*verbose=*/false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, tn.name, 0, /*queue_size=*/8, /*verbose=*/false};
    pub.InitChannel();
    sub.InitChannel();

    ASSERT_TRUE(wait_for_peer(tn.name, 3000)) << "订阅端未完成握手 —— 用例前提不成立";

    auto src = make_image(8, 4, 0x11);
    src.set_msg_id(kMsgId);
    ASSERT_TRUE(pub.publish(std::make_shared<dzIPC::Msg::StdImage>(src)));

    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    int got = 0;
    while (wait_for([&] { return sub.try_get_clone(sink); }, 2000) && got < 8)
    {
        ++got;
    }
    EXPECT_EQ(got, 1) << "无重建时也必须恰好投递一次 —— 否则上面两条用例的 got==1 判据"
                         "失去参照（重复投递会被误判成 I5 缺陷）";
}
