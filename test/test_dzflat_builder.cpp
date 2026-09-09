/* DZFlat B 级(就地构造)回归 (docs/dzflat_shm.md Step 3)
 *
 * A 级已经把「serialize 整包 new + 页尾分段拷贝 + send 再 memcpy 进 chunk」压成"一次
 * 拷贝进 chunk"。B 级要证明的是**那最后一次拷贝也没了**: alloc_* 返回的 span 必须
 * 指向共享 chunk 本身, 调用方(相机 / 雷达驱动)直接往里写。
 *
 * 因此本文件的核心断言不是"值对不对"(那是 A 级已覆盖的), 而是:
 *   ① alloc_* 返回的地址落在 chunk 区间内, 且写进去的字节被订阅方原样读到 ——
 *      中间没有任何中转缓冲;
 *   ② 生命周期: 未投递的借样在析构时必须归还 chunk, 否则 32 块/档位很快耗干;
 *   ③ 超出变长预算时不得写出坏段: alloc 返回空、publish 失败、chunk 归还。
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
#include "ipc_msg/std_msgs/std_point_cloud.hpp"
#include "ipc_msg/test_nested/robot_state.hpp"

namespace {

using namespace std::chrono_literals;

class DzFlatSwitch
{
public:
    explicit DzFlatSwitch(bool on) : prev_(dzIPC::IsDzFlatEnabled())
    {
        dzIPC::EnableDzFlat(on);
        dzIPC::ResetDzFlatCounters();
    }
    ~DzFlatSwitch() { dzIPC::EnableDzFlat(prev_); }

private:
    bool prev_;
};

std::string unique_topic(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/dzflat_b/") + tag + "_" + std::to_string(n.fetch_add(1));
}

/* 生成类型 → Flat, 用于把借来的 Sample bind 成 XxxView。订阅线程把 DZFlat 段分流进
 * 视图队列(get/try_get 借样), TLV 才进物化队列 —— 所以拿 owning 对象需双 drain。 */
template<typename Msg>
struct FlatOf;
template<> struct FlatOf<dzIPC::Msg::StdImage>      { using type = dzIPC::Msg::StdImageFlat; };
template<> struct FlatOf<dzIPC::Msg::StdPointCloud> { using type = dzIPC::Msg::StdPointCloudFlat; };
template<> struct FlatOf<dzIPC::Msg::RobotState>    { using type = dzIPC::Msg::RobotStateFlat; };

/* 反复取直到拿到满足 pred 的一条(双 drain: 视图队列的借样 + 物化队列的 TLV)。 */
template<typename Msg, typename Sub, typename Pred>
bool drain_until(Sub& sub, std::shared_ptr<dzIPC::TopicData>& sink, Pred pred, Msg& out,
                 std::chrono::milliseconds budget = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        while (sub.try_get_clone(sink))
        {
            auto g = sink->topic()->template msgcast<Msg>();
            if (g && pred(*g))
            {
                out = *g;
                return true;
            }
        }
        dzIPC::Sample sample;
        while (sub.try_get(sample))
        {
            auto v = sample.template view<typename FlatOf<Msg>::type>();
            if (!v.valid())
            {
                continue;
            }
            Msg g{};
            v.copy_to(g);
            if (pred(g))
            {
                out = g;
                return true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

}   // namespace

/* ① 零拷贝的实质: alloc_data 返回的地址必须落在借到的 chunk 里。
 *
 * 这是 B 级唯一无法用"值一致"替代的断言 —— 若实现偷偷给了一个临时缓冲再拷进 chunk,
 * 值照样一致, 但零拷贝就是假的。所以直接比对指针区间。 */
TEST(DzFlatBuilder, AllocReturnsSpanInsideTheLoanedChunk)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("inside");
    constexpr std::uint32_t kMsgId = 41;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    constexpr std::uint32_t kBytes = 64 * 48 * 3;
    auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(kBytes + 256);
    ASSERT_TRUE(lo.valid()) << "借样失败(无接收方或 chunk 池耗尽)";

    const auto* chunk_lo = static_cast<const std::uint8_t*>(lo.loan().data);
    const auto* chunk_hi = chunk_lo + lo.loan().size;

    auto px = lo->alloc_data(kBytes);
    ASSERT_EQ(px.size(), kBytes);
    const auto* first = px.data();
    EXPECT_GE(first, chunk_lo) << "alloc_data 返回的地址在 chunk 之前";
    EXPECT_LE(first + kBytes, chunk_hi) << "alloc_data 返回的区间越出 chunk";
    /* Root 之后才是变长区: 负载不能压到段头/Root 上。 */
    EXPECT_GE(first, chunk_lo + sizeof(dzflat::SegHeader) + sizeof(dzIPC::Msg::StdImageRoot));
}

/* ① 端到端: 就地写进共享内存的字节, 订阅方必须原样读到。 */
TEST(DzFlatBuilder, InPlaceConstructionRoundTrips)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("rt");
    constexpr std::uint32_t kMsgId = 42;
    constexpr std::uint32_t kW = 64, kH = 48, kStep = kW * 3;
    constexpr std::uint32_t kBytes = kStep * kH;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    bool published = false;
    for (int attempt = 0; attempt < 40 && !published; ++attempt)
    {
        auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(kBytes + 256);
        if (!lo.valid())
        {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        lo->set_width(kW);
        lo->set_height(kH);
        lo->set_step(kStep);
        ASSERT_TRUE(lo->set_encoding("rgb8"));
        auto hdr = lo->header();
        ASSERT_TRUE(hdr.set_frame_id("camera_optical_frame"));
        hdr.set_stamp(9.5);

        /* "相机直写": 直接往共享内存里填, 没有中转缓冲。 */
        auto px = lo->alloc_data(kBytes);
        ASSERT_EQ(px.size(), kBytes);
        for (std::uint32_t i = 0; i < kBytes; ++i) px[i] = static_cast<std::uint8_t>(i & 0xFF);

        ASSERT_TRUE(lo.ok());
        published = pub.publish_loaned(std::move(lo));
        if (!published) std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(published) << "B 级发布始终失败";
    EXPECT_GT(dzIPC::DzFlatPublishCount(), 0u);

    dzIPC::Msg::StdImage got;
    ASSERT_TRUE(drain_until<dzIPC::Msg::StdImage>(
        sub, sub_td, [&](const dzIPC::Msg::StdImage& m) { return m.data.size() == kBytes; }, got))
        << "B 级构造的消息未送达";

    EXPECT_EQ(got.width, kW);
    EXPECT_EQ(got.height, kH);
    EXPECT_EQ(got.step, kStep);
    EXPECT_EQ(got.encoding, "rgb8");
    EXPECT_EQ(got.header.frame_id, "camera_optical_frame");
    EXPECT_DOUBLE_EQ(got.header.stamp, 9.5);
    ASSERT_EQ(got.data.size(), kBytes);
    for (std::uint32_t i = 0; i < kBytes; ++i)
    {
        ASSERT_EQ(got.data[i], static_cast<std::uint8_t>(i & 0xFF)) << "i=" << i;
    }
}

/* ① Tier-0 元素数组: alloc 返回可写的 C 数组, 雷达可以直接往里写点。 */
TEST(DzFlatBuilder, Tier0ElementArrayWrittenInPlace)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("cloud");
    constexpr std::uint32_t kMsgId = 43;
    constexpr std::uint32_t kN = 3000;

    auto pub_td =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), kMsgId);
    auto sub_td =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const std::uint32_t budget =
        kN * sizeof(dzIPC::Msg::StdVector3dRoot) + kN * sizeof(double) + 512;

    bool published = false;
    for (int attempt = 0; attempt < 40 && !published; ++attempt)
    {
        auto lo = pub.loan<dzIPC::Msg::StdPointCloudFlat>(budget);
        if (!lo.valid())
        {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        auto hdr = lo->header();
        hdr.set_frame_id("lidar_top");
        hdr.set_stamp(7.25);

        auto pts = lo->alloc_points(kN);
        ASSERT_EQ(pts.size(), kN);
        /* 元素记录连续: 相邻间距恰为 sizeof(ElemRoot) —— 就是一段 double[3] 数组。 */
        if (kN >= 2)
        {
            EXPECT_EQ(reinterpret_cast<const std::uint8_t*>(&pts[1])
                          - reinterpret_cast<const std::uint8_t*>(&pts[0]),
                      static_cast<std::ptrdiff_t>(sizeof(dzIPC::Msg::StdVector3dRoot)));
        }
        for (std::uint32_t i = 0; i < kN; ++i)
        {
            pts[i].data[0] = double(i);
            pts[i].data[1] = double(i) * 2.0;
            pts[i].data[2] = double(i) * 3.0;
        }
        auto ch = lo->alloc_channels(kN);
        ASSERT_EQ(ch.size(), kN);
        for (std::uint32_t i = 0; i < kN; ++i) ch[i] = double(i) * 0.5;

        ASSERT_TRUE(lo->alloc_channel_names(2));
        ASSERT_TRUE(lo->set_channel_names_at(0, "intensity"));
        ASSERT_TRUE(lo->set_channel_names_at(1, "ring"));

        ASSERT_TRUE(lo.ok());
        published = pub.publish_loaned(std::move(lo));
        if (!published) std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(published);

    dzIPC::Msg::StdPointCloud got;
    ASSERT_TRUE(drain_until<dzIPC::Msg::StdPointCloud>(
        sub, sub_td, [&](const dzIPC::Msg::StdPointCloud& m) { return m.points.size() == kN; },
        got))
        << "B 级点云未送达";

    EXPECT_EQ(got.header.frame_id, "lidar_top");
    EXPECT_DOUBLE_EQ(got.header.stamp, 7.25);
    ASSERT_EQ(got.points.size(), kN);
    EXPECT_DOUBLE_EQ(got.points[kN / 2].data[1], double(kN / 2) * 2.0);
    EXPECT_DOUBLE_EQ(got.points[kN - 1].data[2], double(kN - 1) * 3.0);
    ASSERT_EQ(got.channels.size(), kN);
    EXPECT_DOUBLE_EQ(got.channels[7], 3.5);
    ASSERT_EQ(got.channel_names.size(), 2u);
    EXPECT_EQ(got.channel_names[0], "intensity");
    EXPECT_EQ(got.channel_names[1], "ring");
}

/* ① Tier-2: 元素自身带变长字段(pose 含 string), 需要元素子 builder。 */
TEST(DzFlatBuilder, Tier2ElementBuildersRoundTrip)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("tier2");
    constexpr std::uint32_t kMsgId = 44;
    constexpr std::uint32_t kN = 5;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::RobotState>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::RobotState>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    bool published = false;
    for (int attempt = 0; attempt < 40 && !published; ++attempt)
    {
        auto lo = pub.loan<dzIPC::Msg::RobotStateFlat>(4096);
        if (!lo.valid())
        {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        ASSERT_TRUE(lo->set_name("arm0"));
        ASSERT_TRUE(lo->set_note("note text"));
        auto cur = lo->current_pose();
        cur.set_x(1.0);
        cur.set_y(2.0);
        cur.set_z(3.0);
        ASSERT_TRUE(cur.set_frame_id("base_link"));

        auto hist = lo->alloc_pose_history(kN);
        ASSERT_EQ(hist.size(), kN);
        for (std::uint32_t i = 0; i < kN; ++i)
        {
            auto e = lo->pose_history_at(i);
            ASSERT_TRUE(e.valid());
            e.set_x(double(i));
            e.set_y(double(i) * 10.0);
            e.set_z(double(i) * 100.0);
            ASSERT_TRUE(e.set_frame_id("frame_" + std::to_string(i)));
        }

        ASSERT_TRUE(lo.ok());
        published = pub.publish_loaned(std::move(lo));
        if (!published) std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(published);

    dzIPC::Msg::RobotState got;
    ASSERT_TRUE(drain_until<dzIPC::Msg::RobotState>(
        sub, sub_td, [&](const dzIPC::Msg::RobotState& m) { return m.pose_history.size() == kN; },
        got))
        << "Tier-2 B 级消息未送达";

    EXPECT_EQ(got.name, "arm0");
    EXPECT_EQ(got.note, "note text");
    EXPECT_EQ(got.current_pose.frame_id, "base_link");
    EXPECT_DOUBLE_EQ(got.current_pose.y, 2.0);
    ASSERT_EQ(got.pose_history.size(), kN);
    for (std::uint32_t i = 0; i < kN; ++i)
    {
        EXPECT_EQ(got.pose_history[i].frame_id, "frame_" + std::to_string(i)) << "i=" << i;
        EXPECT_DOUBLE_EQ(got.pose_history[i].z, double(i) * 100.0) << "i=" << i;
    }
}

/* ② 生命周期: 未投递的借样必须在析构时归还 chunk。
 *
 * 判据是"反复借了不发也不会耗干池子"。每档位只有 32 块, 若析构不归还, 第 33 次就借不到。 */
TEST(DzFlatBuilder, UnpublishedLoanIsReturnedOnDestruction)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("raii");
    constexpr std::uint32_t kMsgId = 45;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    /* 32 块 × 5 轮: 只有每次析构都归还才能全部成功。 */
    for (int i = 0; i < 32 * 5; ++i)
    {
        auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(8192);
        ASSERT_TRUE(lo.valid()) << "第 " << i << " 次借样失败 —— 未投递的借样没有归还 chunk";
        lo->set_width(1);
        auto px = lo->alloc_data(1024);
        EXPECT_EQ(px.size(), 1024u);
        /* 故意不发布, 让它析构 */
    }
}

/* ② move 之后 Builder 里的 Writer 指针必须重指到新对象 —— 否则悬垂。
 * 这是 LoanedMessage 里唯一需要手工维护的不变式。 */
TEST(DzFlatBuilder, MoveKeepsBuilderUsable)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("move");
    constexpr std::uint32_t kMsgId = 46;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto a = pub.loan<dzIPC::Msg::StdImageFlat>(4096);
    ASSERT_TRUE(a.valid());
    a->set_width(7);

    auto b = std::move(a);   /* move 构造 */
    EXPECT_FALSE(a.valid()) << "move 之后源对象仍持有 chunk —— 会双重归还";
    ASSERT_TRUE(b.valid());
    /* 继续通过 b 写入: 若 Writer 指针没重指, 这里会写到已析构对象的成员上。 */
    auto px = b->alloc_data(2048);
    ASSERT_EQ(px.size(), 2048u);
    for (std::uint32_t i = 0; i < px.size(); ++i) px[i] = 0x3C;
    EXPECT_TRUE(b.ok());
    EXPECT_EQ(b->root()->width, 7u) << "move 前写入的字段丢失";
    EXPECT_GT(b.size(), 2048u);
}

/* ③ 超出变长预算: 必须写不出坏段, 且 chunk 归还。 */
TEST(DzFlatBuilder, ExceedingBudgetFailsCleanly)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("budget");
    constexpr std::uint32_t kMsgId = 47;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    {
        auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(1024);
        ASSERT_TRUE(lo.valid());
        /* 请求远超预算(借到的容量按档位取整, 所以要求得足够离谱才必然越界)。 */
        auto px = lo->alloc_data(64 * 1024 * 1024);
        EXPECT_TRUE(px.empty()) << "超预算的 alloc 仍返回了可写区间";
        EXPECT_FALSE(lo.ok()) << "超预算后 ok() 应为假";
        EXPECT_FALSE(pub.publish_loaned(std::move(lo))) << "超预算的段被投递了";
    }
    /* 失败后池子必须干净: 还能正常借到并发布。 */
    auto lo2 = pub.loan<dzIPC::Msg::StdImageFlat>(4096);
    ASSERT_TRUE(lo2.valid()) << "上一次失败泄漏了 chunk";
    lo2->set_width(3);
    auto px2 = lo2->alloc_data(1024);
    EXPECT_EQ(px2.size(), 1024u);
    EXPECT_TRUE(pub.publish_loaned(std::move(lo2)));
}

/* ③ 开关关闭时借样必须失败, 让调用方回退 A 级 publish。 */
TEST(DzFlatBuilder, LoanFailsWhenSwitchOff)
{
    DzFlatSwitch off{false};
    const std::string topic = unique_topic("off");
    constexpr std::uint32_t kMsgId = 48;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(4096);
    EXPECT_FALSE(lo.valid()) << "开关关闭时不应借出 chunk";
    EXPECT_FALSE(pub.publish_loaned(std::move(lo)));

    /* A 级路径仍然可用。 */
    auto m = std::make_shared<dzIPC::Msg::StdImage>();
    m->set_msg_id(kMsgId);
    m->encoding = "rgb8";
    m->data.assign(1024, 0x5A);
    EXPECT_TRUE(pub.publish(m));
}

/* ③ 无接收方时借样失败(chunk 无人回收)。 */
TEST(DzFlatBuilder, LoanFailsWithoutSubscriber)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("nosub");
    constexpr std::uint32_t kMsgId = 49;

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();
    std::this_thread::sleep_for(200ms);

    auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(4096);
    EXPECT_FALSE(lo.valid()) << "无接收方时不应借出 chunk";
}
