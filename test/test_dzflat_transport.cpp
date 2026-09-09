/* DZFlat 传输层回归 (docs/dzflat_shm.md Step 2)
 *
 * Step 1 只验证了布局本身(离线, 不碰传输)。这里验证收益进入真实 SHM 路径之后
 * 的三件事:
 *
 *   ① 借样发布 → 跨句柄接收 → 值一致。发布端把消息**直接按平坦布局写进借来的
 *      chunk**, 不再有 serialize() 的整包 new 与 send() 的二次 memcpy;
 *   ② **双 wire 判别**。订阅侧无条件同时认 DZFlat 与 TLV(按段首 magic), 与发布侧
 *      开关无关 —— 这是"先升级订阅方、再升级发布方"这个灰度顺序的地基。同一个
 *      订阅者在一次会话里必须能交替收下两种 wire;
 *   ③ **回退是常态**。开关未开 / 类型不支持 / 无接收方 / chunk 池耗尽都必须静默
 *      回退整包序列化并照常送达, 而不是丢消息或报错。
 *
 * 开关默认 OFF, 每个用例自己开关并在结束时恢复(RAII), 避免污染同进程内的其他用例。
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"

namespace {

using namespace std::chrono_literals;

/* 开关的 RAII 守卫: 用例之间不互相污染(gtest 同进程顺序执行)。 */
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

/* 回退是静默的 —— 开关开着但类型不支持 / 无接收方 / 池耗尽都会回落 TLV 且照常送达。
 * 所以"测试通过"本身并不能证明 DZFlat 路径被走到了; 必须查计数器。没有这道断言,
 * 整个文件在实现完全失效的情况下也会全绿。 */
void expect_dzflat_was_used(const char* what)
{
    EXPECT_GT(dzIPC::DzFlatPublishCount(), 0u)
        << what << ": 一条也没走 DZFlat —— 全部静默回退了整包序列化";
}

void expect_dzflat_never_used(const char* what)
{
    EXPECT_EQ(dzIPC::DzFlatPublishCount(), 0u) << what << ": 不该走 DZFlat 却走了";
}

std::string unique_topic(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/dzflat_tx/") + tag + "_" + std::to_string(n.fetch_add(1));
}

dzIPC::Msg::StdImage make_image(std::size_t w, std::size_t h, std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "camera_optical_frame";
    img.header.stamp = 1234.5;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.step = static_cast<std::uint32_t>(w * 3);
    img.encoding = "rgb8";
    img.data.resize(w * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
        img.data[i] = static_cast<std::uint8_t>((i + seed) & 0xFF);
    return img;
}

bool same_image(const dzIPC::Msg::StdImage& a, const dzIPC::Msg::StdImage& b)
{
    return a.width == b.width && a.height == b.height && a.step == b.step
           && a.encoding == b.encoding && a.header.frame_id == b.header.frame_id
           && a.header.stamp == b.header.stamp && a.data == b.data;
}

bool same_cloud(const dzIPC::Msg::StdPointCloud& a, const dzIPC::Msg::StdPointCloud& b)
{
    if (a.points.size() != b.points.size()) return false;
    for (std::size_t i = 0; i < a.points.size(); ++i)
    {
        if (a.points[i].data != b.points[i].data) return false;
    }
    return a.channel_names == b.channel_names && a.channels == b.channels
           && a.header.frame_id == b.header.frame_id && a.header.stamp == b.header.stamp;
}

/* 生成类型 → 其 Flat(用于把借来的 Sample bind 成 XxxView 再 copy_to owning)。
 * 订阅线程现在把 DZFlat 段分流进**视图队列**(get/try_get 借样零拷贝), TLV 才进
 * 物化队列(get_clone/try_get_clone)。所以 DZFlat 用例要收到 owning 对象做内容比对,
 * 得先 try_get(Sample), 把 view copy_to 成 owning —— 这里只做功能比对, 拷贝无妨。 */
template<typename Msg>
struct FlatOf;
template<>
struct FlatOf<dzIPC::Msg::StdImage>
{
    using type = dzIPC::Msg::StdImageFlat;
};
template<>
struct FlatOf<dzIPC::Msg::StdPointCloud>
{
    using type = dzIPC::Msg::StdPointCloudFlat;
};

/* 反复发布直到**收到与 src 内容一致的一条**, 或超时。
 *
 * 两个必须这样写的理由:
 *   - SHM 通道要先完成握手, 早期几条会落空 —— 这与 DZFlat 无关, 是既有 SHM 路径
 *     的既定行为(见 shm_pub_sub_ipc.cc 的注释);
 *   - 订阅队列有深度(本文件用 8), 上一阶段积压的旧消息会先被 try_get 取出。切换
 *     wire 之后如果只取一条就断言, 取到的多半是切换前的那条。所以判据是"内容
 *     匹配", 不是"取到了一条"。
 *
 * msg_id 必须显式设到**被发布的那个消息对象**上: TopicData 只把 id 设给它自己持有
 * 的 topic_/topic_cache, 而 publish() 发的是调用方新建的对象。两侧不一致时订阅端
 * 的 check_id / check_dzflat_id 会全部拒收, 表现为"一条都收不到"。 */
template<typename Msg, typename Pub, typename Sub, typename Eq>
bool pump_until(Pub& pub, Sub& sub, std::shared_ptr<dzIPC::TopicData>& sink, const Msg& src,
                Msg& out, std::uint32_t msg_id, Eq eq,
                std::chrono::milliseconds budget = 4000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<Msg>(src);
        m->set_msg_id(msg_id);
        pub.publish(m);
        /* 把队列里能取的都取掉, 逐条比对 —— 只要出现一条匹配的就算送达。 */
        /* 双 drain: DZFlat 段在视图队列(借样), TLV 在物化队列。逐条取、按内容匹配。 */
        while (sub.try_get_clone(sink))
        {
            auto got = sink->topic()->template msgcast<Msg>();
            if (got && eq(src, *got))
            {
                out = *got;
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
            Msg tmp{};
            v.copy_to(tmp);
            if (eq(src, tmp))
            {
                out = tmp;
                return true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

}   // namespace

/* ① 借样发布 → 接收 → 值一致(大 blob)。 */
TEST(DzFlatTransport, ImageRoundTripOverLoanedChunk)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("img");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 11);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 11);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(320, 240, 0x10);
    dzIPC::Msg::StdImage got;
    ASSERT_TRUE(pump_until(pub, sub, sub_td, src, got, 11, same_image)) << "DZFlat 借样路径未送达";
    EXPECT_TRUE(same_image(src, got)) << "DZFlat 往返值不一致";
    expect_dzflat_was_used("image 往返");
}

/* ① 点云: 嵌套数组是 DZFlat 收益最大的形态(TLV 每元素一次堆分配)。 */
TEST(DzFlatTransport, PointCloudRoundTripOverLoanedChunk)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("cloud");

    auto pub_td =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), 12);
    auto sub_td =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), 12);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    constexpr std::size_t kN = 5000;
    dzIPC::Msg::StdPointCloud src;
    src.header.frame_id = "lidar_top";
    src.header.stamp = 42.5;
    src.points.resize(kN);
    for (std::size_t i = 0; i < kN; ++i)
        src.points[i].data = {double(i), double(i) * 2.0, double(i) * 3.0};
    src.channel_names = {"intensity", "ring"};
    src.channels.assign(kN, 0.75);

    dzIPC::Msg::StdPointCloud got;
    ASSERT_TRUE(pump_until(pub, sub, sub_td, src, got, 12, same_cloud)) << "DZFlat 点云未送达";
    ASSERT_EQ(got.points.size(), kN);
    EXPECT_EQ(got.points[kN / 2].data, src.points[kN / 2].data);
    EXPECT_EQ(got.channel_names, src.channel_names);
    EXPECT_EQ(got.channels, src.channels);
    EXPECT_EQ(got.header.frame_id, src.header.frame_id);
    EXPECT_DOUBLE_EQ(got.header.stamp, src.header.stamp);
    expect_dzflat_was_used("point cloud 往返");
}

/* ③ 开关关闭(默认态): 走既有 TLV 路径, 行为不得改变。 */
TEST(DzFlatTransport, SwitchOffKeepsTlvPathIntact)
{
    DzFlatSwitch off{false};
    const std::string topic = unique_topic("tlv");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 13);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 13);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(160, 120, 0x20);
    dzIPC::Msg::StdImage got;
    ASSERT_TRUE(pump_until(pub, sub, sub_td, src, got, 13, same_image)) << "TLV 路径未送达";
    EXPECT_TRUE(same_image(src, got));
    expect_dzflat_never_used("开关关闭");
}

/* ② 双 wire 判别: 同一个订阅者在一次会话里交替收下 DZFlat 与 TLV。
 *
 * 这是灰度的地基。段首 4 字节 DZFlat 是 magic, TLV 是首字段名的长度(小整数),
 * 结构上不可能碰撞, 所以订阅侧可以无条件同时认两种。 */
TEST(DzFlatTransport, OneSubscriberAcceptsBothWiresAlternately)
{
    const std::string topic = unique_topic("dual");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 14);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 14);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    /* 先用 DZFlat 打通(顺带完成握手)。 */
    dzIPC::Msg::StdImage got;
    {
        DzFlatSwitch on{true};
        const auto a = make_image(64, 48, 0x31);
        ASSERT_TRUE(pump_until(pub, sub, sub_td, a, got, 14, same_image)) << "DZFlat 段未被接受";
        EXPECT_TRUE(same_image(a, got));
        expect_dzflat_was_used("双 wire 第一段");
    }
    /* 同一个订阅者, 立刻切回 TLV。 */
    {
        DzFlatSwitch off{false};
        const auto b = make_image(64, 48, 0x77);
        ASSERT_TRUE(pump_until(pub, sub, sub_td, b, got, 14, same_image)) << "切回 TLV 后订阅者收不到了";
        EXPECT_TRUE(same_image(b, got));
        expect_dzflat_never_used("双 wire 第二段(TLV)");
    }
    /* 再切回 DZFlat, 确认不是"只在第一次生效"。 */
    {
        DzFlatSwitch on{true};
        const auto c = make_image(64, 48, 0xB5);
        ASSERT_TRUE(pump_until(pub, sub, sub_td, c, got, 14, same_image)) << "再次切到 DZFlat 后收不到";
        EXPECT_TRUE(same_image(c, got));
        expect_dzflat_was_used("双 wire 第三段");
    }
}

/* ③ 不支持 DZFlat 的类型必须静默回退 TLV。GenericMessage 是通用 TLV 走查器
 * (Python / topic_echo 用的就是它), 不由 generator 发射 DZFlat 覆写。 */
TEST(DzFlatTransport, UnsupportedTypeFallsBackToTlv)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("generic");

    auto probe = std::make_shared<dzIPC::GenericMessage>();
    ASSERT_FALSE(probe->dzflat_supported()) << "GenericMessage 不应支持 DZFlat";

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), 15);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), 15);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    dzIPC::GenericMessage src;
    src.set_uint32("width", 7);
    src.set_string("encoding", "mono8");
    src.set_uint8_array("data", std::vector<std::uint8_t>(2048, 0x5C));

    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    bool ok = false;
    while (!ok && std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::GenericMessage>(src);
        m->set_msg_id(15);
        pub.publish(m);
        if (sub.try_get_clone(sub_td))
        {
            auto got = sub_td->topic()->msgcast<dzIPC::GenericMessage>();
            if (got && got->field_count() == 3)
            {
                EXPECT_EQ(got->get_uint32("width"), 7u);
                EXPECT_EQ(got->get_string("encoding"), "mono8");
                ok = true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(ok) << "不支持 DZFlat 的类型未能回退 TLV 送达";
    expect_dzflat_never_used("GenericMessage");
    EXPECT_GT(dzIPC::DzFlatFallbackCount(), 0u) << "回退计数未记录";
}

/* ③ 无订阅者时 publish 必须成功(走 sniffer 环, 而 sniffer 侧解析 TLV, 所以
 * 这条路径不能用 DZFlat)。 */
TEST(DzFlatTransport, PublishWithoutSubscriberStillSucceeds)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("nosub");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 16);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    /* sniffer 环的负载上限是 sniffer_payload_limit(16 KB), 用小图。 */
    auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(32, 24, 0x40));
    m->set_msg_id(16);
    EXPECT_TRUE(pub.publish(m)) << "无订阅者时发布失败 —— 借样路径没有正确回退";
}

/* ③ chunk 池耗尽时必须回退整包路径而不是丢消息。
 *
 * 构造: 用一个独立的 route 句柄把同一尺寸档位的 32 块全部借走并**不归还**, 此时
 * DZFlat 借样必然失败; 消息仍须通过 TLV 送达。 */
TEST(DzFlatTransport, ChunkPoolExhaustionFallsBackToTlv)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("exhaust");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 17);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 17);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    /* 先确认通道通了(此时池子还是满的)。 */
    const auto src = make_image(96, 72, 0x51);
    dzIPC::Msg::StdImage got;
    ASSERT_TRUE(pump_until(pub, sub, sub_td, src, got, 17, same_image));
    EXPECT_TRUE(same_image(src, got));

    /* 借空该消息所落的尺寸档位。dzflat_size 决定档位, 用同一条消息去算。 */
    dzIPC::Msg::StdImage sizer = src;
    sizer.set_msg_id(17);
    const std::uint32_t need = sizer.dzflat_size();

    /* shm_open 的名字不能含路径分隔符(topic 里带 "/"), 另起一个扁平名。 */
    const std::string hog_name = "dzflat_tx_hog_pool";
    ipc::route hog_tx{hog_name.c_str(), ipc::sender};
    ipc::route hog_rx{hog_name.c_str(), ipc::receiver};
    ASSERT_TRUE(hog_tx.wait_for_recv(1, 2000));
    std::vector<ipc::loan_t> hogged;
    for (int i = 0; i < 64; ++i)
    {
        auto lo = hog_tx.loan(need);
        if (!lo.valid()) break;
        hogged.push_back(lo);
    }
    ASSERT_FALSE(hogged.empty()) << "未能借到任何 chunk, 无法构造耗尽场景";
    EXPECT_FALSE(hog_tx.loan(need).valid()) << "池子应已耗尽";

    /* 池空 ⇒ DZFlat 借样失败 ⇒ 必须回退 TLV 并照常送达。 */
    const auto src2 = make_image(96, 72, 0x9A);
    dzIPC::Msg::StdImage got2;
    EXPECT_TRUE(pump_until(pub, sub, sub_td, src2, got2, 17, same_image))
        << "chunk 池耗尽时消息丢失 —— 借样路径没有回退整包序列化";
    EXPECT_TRUE(same_image(src2, got2));

    for (const auto& lo : hogged) hog_tx.discard_loan(lo);
}

/* ② msg_id 不匹配的 DZFlat 段必须被拒(与 TLV 的 check_id 同语义), 不能错投给
 * 另一个话题类型。 */
TEST(DzFlatTransport, MismatchedMsgIdIsRejected)
{
    DzFlatSwitch on{true};
    const std::string topic = unique_topic("wrongid");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 21);
    /* 订阅者期望 msg_id = 22, 发布者用 21。 */
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 22);

    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8};
    pub.InitChannel();
    sub.InitChannel();

    const auto src = make_image(64, 48, 0x61);
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    bool delivered = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        pub.publish(m);
        if (sub.try_get_clone(sub_td))
        {
            delivered = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_FALSE(delivered) << "msg_id 不匹配的 DZFlat 段被投递了";
}

/* ① 真正的跨进程往返。
 *
 * 上面全部用例都在同一个进程里跑, 而 DZFlat 的意义恰恰在于跨越进程边界: 段里没有
 * 任何指针, 全是相对段首的偏移, 所以同一块 chunk 在两个进程各自映射到不同虚拟地址
 * 也照样能读(docs/dzflat_shm.md §3.1 的"位置无关")。同进程测试验证不了这一条 ——
 * 若布局里混进了绝对地址, 同进程会照常通过, 跨进程才会炸。
 *
 * fork 出子进程做发布方, 父进程订阅。选 fork 而非线程正是为了让两侧拿到**不同的
 * 地址空间**; 子进程重新 InitChannel, 不继承父进程的映射语义。 */
TEST(DzFlatTransport, CrossProcessRoundTripProvesPositionIndependence)
{
    const std::string topic = "/dzflat_xproc/img";
    constexpr std::uint32_t kMsgId = 31;
    constexpr std::size_t kW = 200, kH = 150;

    /* 父进程先建订阅方, 保证子进程发布时已有接收方(否则 loan 会拒绝)。 */
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 16};
    sub.InitChannel();
    std::this_thread::sleep_for(200ms);

    const auto expected = make_image(kW, kH, 0xC4);

    pid_t child = ::fork();
    ASSERT_GE(child, 0) << "fork 失败";
    if (child == 0)
    {
        /* ---- 子进程: 发布方 ---- */
        dzIPC::EnableDzFlat(true);
        auto pub_td =
            std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
        pub.InitChannel();
        std::this_thread::sleep_for(300ms);   /* 等握手 */

        const auto img = make_image(kW, kH, 0xC4);
        for (int i = 0; i < 60; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdImage>(img);
            m->set_msg_id(kMsgId);
            pub.publish(m);
            std::this_thread::sleep_for(20ms);
        }
        /* 用退出码回报"子进程确实走了 DZFlat 而非静默回退"。 */
        const bool used = dzIPC::DzFlatPublishCount() > 0;
        ::_exit(used ? 0 : 42);
    }

    /* ---- 父进程: 订阅方 ---- */
    dzIPC::Msg::StdImage got;
    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + 6000ms;
    while (!ok && std::chrono::steady_clock::now() < deadline)
    {
        while (sub.try_get_clone(sub_td))
        {
            auto g = sub_td->topic()->msgcast<dzIPC::Msg::StdImage>();
            if (g && same_image(expected, *g))
            {
                got = *g;
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            dzIPC::Sample sample;
            while (sub.try_get(sample))
            {
                auto v = sample.view<dzIPC::Msg::StdImageFlat>();
                if (!v.valid()) continue;
                dzIPC::Msg::StdImage g{};
                v.copy_to(g);
                if (same_image(expected, g))
                {
                    got = g;
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) std::this_thread::sleep_for(10ms);
    }

    int status = 0;
    ::waitpid(child, &status, 0);

    EXPECT_TRUE(ok) << "跨进程 DZFlat 段未送达或内容不一致";
    ASSERT_TRUE(WIFEXITED(status)) << "子进程异常退出(信号)";
    EXPECT_NE(WEXITSTATUS(status), 42)
        << "子进程一条也没走 DZFlat —— 跨进程用例其实在测 TLV";
    EXPECT_EQ(WEXITSTATUS(status), 0);
    if (ok)
    {
        EXPECT_EQ(got.data, expected.data);
        EXPECT_EQ(got.encoding, expected.encoding);
        EXPECT_EQ(got.header.frame_id, expected.header.frame_id);
    }
}
