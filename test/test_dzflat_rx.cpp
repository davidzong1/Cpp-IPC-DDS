/* DZFlat 接收侧视图路径回归 (docs/dzflat_shm.md §4.3 的 Sample)
 *
 * 订阅线程现在把 DZFlat 段分流进**视图队列**(get/try_get 借样零拷贝), TLV 才进
 * 物化队列(get_clone/try_get_clone)。本文件验证视图路径本身:
 *
 *   ① 零拷贝: try_get 拿到的 Sample 就是共享 chunk(span 指向 chunk 内部), 不是拷贝;
 *   ② 生命周期(第 3/4/5 条 known_issues 的回归): 持有 Sample 的同时让发布端继续猛发,
 *      其 chunk 必须被 conns 引用计数钉住, 不被后续消息回收 —— 这是
 *      src/libipc/prod_cons.h pop 修复(clear-CAS 前重检 epoch)要守的不变量;
 *   ③ 严格分流: TLV 消息永远不被 try_get(视图)物化, DZFlat 段永远不被 try_get_clone
 *      拿去物化 —— 各走各的队列, 灰度期需调用方双 drain。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {

using namespace std::chrono_literals;

class DzFlatSwitch
{
public:
    explicit DzFlatSwitch(bool on) : prev_(dzIPC::IsDzFlatEnabled())
    {
        dzIPC::EnableDzFlat(on);
        dzIPC::ResetDzFlatCounters();
        dzIPC::ResetDzFlatRxCounters();
    }
    ~DzFlatSwitch() { dzIPC::EnableDzFlat(prev_); }

private:
    bool prev_;
};

std::string unique_topic(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/dzflat_rx/") + tag + "_" + std::to_string(n.fetch_add(1));
}

dzIPC::Msg::StdImage make_image(std::size_t w, std::size_t h, std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "camera";
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.step = static_cast<std::uint32_t>(w * 3);
    img.encoding = "rgb8";
    img.data.resize(w * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
        img.data[i] = static_cast<std::uint8_t>((i + seed) & 0xFF);
    return img;
}

constexpr std::uint32_t kMsgId = 31;

}   // namespace

/* ① 零拷贝: Sample 的段内存就在 chunk 里, 大数组 span 指向 chunk 内部。 */
TEST(DzFlatRx, ViewPointsIntoTheBorrowedChunk)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("zcp");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(160, 120, 0x2A);   /* data ~57KB, 占独立 chunk */
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    dzIPC::Sample sample;
    while (!sample.valid() && std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        sub.try_get(sample);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(sample.valid()) << "DZFlat 段未通过视图路径送达";
    EXPECT_GT(dzIPC::DzFlatRxCounters().dzflat_accepted, 0u);

    auto v = sample.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(v.valid()) << "bind 应通过";
    EXPECT_EQ(v.height(), src.height);
    EXPECT_EQ(v.width(), src.width);
    EXPECT_EQ(v.encoding(), "rgb8");

    const auto* base = static_cast<const std::uint8_t*>(sample.data());
    const std::size_t span_size = sample.size();
    auto px = v.data();   /* span<const uint8_t> 指向段内 */
    EXPECT_GT(px.size(), 0u);
    const auto* pxp = reinterpret_cast<const std::uint8_t*>(px.data());
    EXPECT_GE(pxp, base);
    EXPECT_LT(pxp, base + span_size) << "data span 必须指向借来的 chunk 内部(零拷贝)";
    EXPECT_EQ(pxp + px.size() <= base + span_size, true);
}

/* ② 生命周期: 持有 Sample 的同时猛发后续消息, 其 chunk 必须被钉住。 */
TEST(DzFlatRx, HeldSampleSurvivesSubsequentPublishes)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("hold");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(160, 120, 0x4E);
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    dzIPC::Sample held;
    while (!held.valid() && std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        sub.try_get(held);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(held.valid());

    /* 预期内容: 先拷一份 owning 出来, burst 之后再比对 held 仍一致。 */
    dzIPC::Msg::StdImage before{};
    {
        auto v = held.view<dzIPC::Msg::StdImageFlat>();
        ASSERT_TRUE(v.valid());
        v.copy_to(before);
    }
    EXPECT_EQ(before.data, src.data);

    /* 猛发: 同样尺寸档位的 chunk 池只有 32 块, 2000 条足以让 chunk 充分复用; 若 held
     * 的 chunk 没被 conns 钉住, 早就被回收写花。视图队列 drop-oldest, pump 不阻塞。 */
    for (int i = 0; i < 2000; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }

    /* held 仍然有效且内容未被写花 —— 这是 pop 修复 + conns 引用计数共同保证的。 */
    auto v2 = held.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(v2.valid()) << "held 的 view 失效 —— chunk 被后续消息回收了";
    dzIPC::Msg::StdImage after{};
    v2.copy_to(after);
    EXPECT_EQ(after.data, before.data) << "held 的 chunk 内容被后续消息覆写";
    EXPECT_EQ(after.width, src.width);
}

/* ③ 严格分流(a): DZFlat 未开 ⇒ 消息走 TLV ⇒ try_get(视图)必须永远空。 */
TEST(DzFlatRx, TlvOnlyNeverReachesTheViewPath)
{
    DzFlatSwitch off{false};
    const std::string topic = unique_topic("tlvonly");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(48, 32, 0x77);
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    bool cloned = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);

        dzIPC::Sample sample;
        EXPECT_FALSE(sub.try_get(sample)) << "TLV 消息不该出现在视图路径";
        if (sub.try_get_clone(sub_td))
        {
            cloned = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(cloned) << "TLV 消息应通过物化路径送达";
    EXPECT_GT(dzIPC::DzFlatRxCounters().tlv_accepted, 0u);
}

/* ③ 严格分流(b): DZFlat 开 + typed 话题 ⇒ 段进视图队列; try_get_clone 不该拿到。 */
TEST(DzFlatRx, DzflatOnlyGoesToTheViewPath)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("viewonly");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(48, 32, 0x99);
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    dzIPC::Sample sample;
    while (!sample.valid() && std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        sub.try_get(sample);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(sample.valid()) << "DZFlat 段应经视图路径送达";

    /* 物化队列不该收到 typed DZFlat 段(它被分流到视图队列了)。 */
    std::shared_ptr<dzIPC::TopicData> spill;
    EXPECT_FALSE(sub.try_get_clone(spill)) << "typed DZFlat 段不应出现在物化队列";
    EXPECT_GT(dzIPC::DzFlatRxCounters().dzflat_accepted, 0u);
}

/* ④ TLV-only 话题上, 带超时的 get 必须按时返回 false, 而不是永久挂死。
 * (docs/shm_defect_fixes.md 第 4 条: 视图队列只承载 DZFlat 段, 而是否发 DZFlat 由发布端
 * 决定, 所以 TLV-only 话题的视图队列**永远是空的** —— 无超时的 get 不是"等数据"而是注定
 * 挂死。判据必须带时限: 用一个 watchdog 线程断言它在预算内返回。) */
TEST(DzFlatRx, TimedGetReturnsOnTlvOnlyTopic)
{
    DzFlatSwitch off{false};   /* 关掉 DZFlat ⇒ 全走 TLV ⇒ 视图队列恒空 */
    const std::string topic = unique_topic("timedget");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    /* 发几条 TLV, 让通道确实活着 —— 证明"返回 false"不是因为通道不通, 而是因为视图
     * 队列本来就收不到东西。 */
    const auto src = make_image(48, 32, 0x11);
    for (int i = 0; i < 5; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }
    std::this_thread::sleep_for(200ms);

    const auto t0 = std::chrono::steady_clock::now();
    dzIPC::Sample sample;
    const bool got = sub.get(sample, 200);   /* 200ms 预算 */
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_FALSE(got) << "TLV-only 话题上视图 get 不该拿到东西";
    EXPECT_GE(elapsed, 150ms) << "提前返回了, 没真正等满超时";
    EXPECT_LT(elapsed, 2000ms) << "超过预算仍未返回 —— 超时没生效, 调用方会挂死";
}

/* ⑤ TLV 消息确实还在(证明上一条的 false 不是通道故障) —— 双 drain 的另一半。 */
TEST(DzFlatRx, TimedGetDoesNotDisturbTheClonePath)
{
    DzFlatSwitch off{false};
    const std::string topic = unique_topic("timedget2");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const auto src = make_image(48, 32, 0x22);
    dzIPC::Sample sample;
    sub.get(sample, 100);   /* 先空等一次, 不应影响后续 */

    bool cloned = false;
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        if (sub.try_get_clone(sub_td))
        {
            cloned = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(cloned) << "超时返回后物化路径应当照常工作";
}

/* ⑥ 预构造段发布: 段由调用方写好交来, 发布端只负责借 chunk 送出去。
 *
 * 这条路径是给"schema 在调用方"的进程用的(今天唯一使用者是 Python 的
 * dzipc.publish_dzflat), 所以 C++ 侧必须单独钉住两件事:
 *   ① 段真的借样送达, 且**逐字节**与发出去的段相同 —— 这条路径不做任何转换;
 *   ② 三道门各自都会返回 false, 好让调用方回退 TLV: 开关关 / 段头不合法 /
 *      段头 msg_id 与本话题模板不符(后者对端是**静默丢弃**, 不本地拦住就等于白丢一条)。 */
TEST(DzFlatRx, PrebuiltSegmentPublishReachesTheViewPath)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("prebuilt");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    auto src = make_image(96, 64, 0x5C);
    src.set_msg_id(kMsgId);   /* 段头 msg_id 必须与话题模板一致(否则会被门拦下) */
    /* 段由"调用方"写 —— 这里用 C++ 写端, 与 Python 编码器产出的是同一种东西。 */
    std::vector<std::uint8_t> seg(src.dzflat_size());
    ASSERT_TRUE(src.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));
    dzflat::SegHeader h{};
    std::memcpy(&h, seg.data(), sizeof(h));
    ASSERT_EQ(h.msg_id, kMsgId) << "段头 msg_id 来自 set_msg_id";
    const std::uint32_t seg_len = h.total_size;
    ASSERT_LE(seg_len, seg.size());

    /* ---- 负例先跑: 三道门都不许把段发出去 ---- */
    {
        auto bad = seg;   /* (a) 段头 magic 坏掉 */
        bad[0] = 0x00;
        EXPECT_FALSE(pub.publish_prebuilt_segment(bad.data(), bad.size()))
            << "magic 不合法的段不得发出";
    }
    {
        auto bad = seg;   /* (b) 段头 msg_id 与话题模板不符 */
        bad[24] = 0xEE;   /* SegHeader.msg_id 在第 24 字节 */
        EXPECT_FALSE(pub.publish_prebuilt_segment(bad.data(), bad.size()))
            << "msg_id 不符的段不得发出 —— 对端只会静默丢掉它";
    }
    {
        dzIPC::EnableDzFlat(false);   /* (c) 全局开关 */
        EXPECT_FALSE(pub.publish_prebuilt_segment(seg.data(), seg.size()));
        dzIPC::EnableDzFlat(true);
    }
    {
        dzIPC::Sample none;   /* 负例不得在两条队列里留下任何东西 */
        EXPECT_FALSE(sub.try_get(none));
        EXPECT_FALSE(sub.try_get_clone(sub_td));
    }

    /* ---- 正例: 借样送达, 内容的每个字节都要对上 ---- */
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    dzIPC::Sample sample;
    while (!sample.valid() && std::chrono::steady_clock::now() < deadline)
    {
        pub.publish_prebuilt_segment(seg.data(), seg.size());
        sub.try_get(sample);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(sample.valid()) << "预构造段没有通过借样路径送达";
    EXPECT_EQ(sample.msg_id(), kMsgId);
    EXPECT_EQ(sample.schema_hash(), src.dzflat_schema_hash());
    /* Sample::size() 是**借到的 chunk 容量**(sample_message.h 的契约), SHM 上通常大于
     * 段长; 段内有效长度由段头的 total_size 决定。所以这里判"够装"而不是"相等"。 */
    EXPECT_GE(sample.size(), seg_len) << "借来的 chunk 装不下声明的段长";
    EXPECT_EQ(std::memcmp(sample.data(), seg.data(), seg_len), 0)
        << "收到的段与发出去的段必须逐字节相同";

    auto v = sample.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(v.valid()) << "bind 应通过";
    EXPECT_EQ(v.width(), src.width);
    EXPECT_EQ(v.height(), src.height);
    EXPECT_EQ(v.encoding(), "rgb8");
    auto px = v.data();
    ASSERT_EQ(px.size(), src.data.size());
    EXPECT_EQ(std::memcmp(px.data(), src.data.data(), px.size()), 0)
        << "大数组内容与源不一致";
    EXPECT_GT(dzIPC::DzFlatPublishCount(), 0u) << "走平坦段发出的条数必须记上";
}

/* ⑦ 无接收方时必须返回 false, 而不是"发进空通道后返回 true" —— 前者才能让调用方回退 TLV。 */
TEST(DzFlatRx, PrebuiltSegmentWithoutReceiversReturnsFalse)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("prebuilt_nosub");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto src = make_image(32, 16, 0x09);
    src.set_msg_id(kMsgId);   /* 先过掉 msg_id 那道门, 保证本条测的是"无接收方"而不是别的 */
    std::vector<std::uint8_t> seg(src.dzflat_size());
    ASSERT_TRUE(src.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));

    EXPECT_FALSE(pub.publish_prebuilt_segment(seg.data(), seg.size()))
        << "没有接收方时 chunk 借不到 ⇒ 必须回退, 不能假装成功";
}
