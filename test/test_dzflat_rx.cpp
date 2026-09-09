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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
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
