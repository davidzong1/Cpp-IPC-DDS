/* T1: 话题发布/订阅的 UDP 订阅接收端 —— try_get 走内部借样, try_get_clone 保持拷贝。
 *
 * 背景(为什么 socket 侧也需要一条视图路径): SHM 订阅端有两支互补队列 —— DZFlat 段进
 * 视图队列(get/try_get 借样), TLV 进物化队列(get_clone/try_get_clone); 而 socket 订阅端
 * 原来只有物化一条, 且收到 DZFlat 段会被当成 TLV **反序列化** —— 段尾是借来的 chunk 里
 * 没写到的部分, 按 TLV 读必然走偏, 且这条路上没有任何判别。T1 把 SHM 的那道分流搬到
 * UDP 接收端: 段首 magic 判别 + 严格二选一投递 + Sample 生命周期对齐。
 *
 * 两个传输的**成本不同**, 这一点也在这里钉住:
 *   SHM  —— 真零拷贝, Sample 借的是发布方写好的共享 chunk(见 test_dzflat_rx.cpp);
 *   UDP  —— 恒有一次**去帧拷贝**: 接收缓冲是本进程复用的临时内存(platform/posix/udp.h
 *           的 "Temp buffer avoid copying"), 且分帧会把 12 字节页尾插进段中间, 段在
 *           wire 上不连续。所以本文件断言的是"内容与生命周期正确", 不是"span 落在网卡
 *           缓冲里" —— 后者在 UDP 上根本不成立, 断言它只会写出一个恒真的假判据。
 *
 * 本文件另外定义并钉住**发送侧的封装约定**(UDP 上没有现成的 DZFlat 发布端, 所以由测试
 * 按约定自造 wire): 每 1460 字节数据后跟 12 字节页尾 {page_cnt, now_page, total_size,
 * msg_id}(大端), total_size = 分帧流总长(含全部页尾) —— 与 IpcMsgBase::correct_total_size
 * 同式。跨页用例(②)专门覆盖"页尾插在段中间"这一条: 去帧若漏掉页尾补偿, 段内偏移会整体
 * 错位, 断言必然挂。
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/udp.h"

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 37;
constexpr std::size_t kDomain = 0;
constexpr std::size_t kPageSize = 1'472;
constexpr std::size_t kTailSize = 12;
constexpr std::size_t kDataPerPage = kPageSize - kTailSize;   // 与 data_rev.cc 一致

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
    return std::string("/socket_borrow/") + tag + "_" + std::to_string(n.fetch_add(1));
}

/* 载荷带位置相关图案: 漏页 / 错位 / 页尾没补偿都会让某个字节对不上, 而不是"看着差不多"。 */
dzIPC::Msg::StdImage make_image(std::size_t w, std::size_t h)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "cam0";
    img.header.stamp = 3.25;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.step = static_cast<std::uint32_t>(w * 3);
    img.encoding = "rgb8";
    img.data.resize(w * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
    {
        img.data[i] = static_cast<std::uint8_t>((i * 7 + (i >> 8)) & 0xFF);
    }
    img.set_msg_id(kMsgId);
    return img;
}

std::vector<std::uint8_t> make_segment(const dzIPC::Msg::StdImage& img)
{
    std::vector<std::uint8_t> seg(img.dzflat_size());
    EXPECT_TRUE(img.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));
    return seg;
}

/* 段头声明的段长(写端封口时按 Writer 实际水位写, 见 dzflat::write_header)。
 * 发送侧的"载荷"就是这个长度 —— 不是分配用的上界。 */
std::uint32_t declared_seg_len(const std::vector<std::uint8_t>& seg)
{
    dzflat::SegHeader h{};
    std::memcpy(&h, seg.data(), sizeof(h));
    return h.total_size;
}

void put_u16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

void put_u32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

/* 发送侧封装约定(见文件头)。msg_id 单独传, 好构造"段头 msg_id 与话题不符"的负例。 */
std::vector<std::vector<std::uint8_t>> frame_segment(const std::vector<std::uint8_t>& seg, std::uint32_t msg_id)
{
    const std::size_t pages = seg.size() / kDataPerPage + 1;   // 与 correct_total_size 同式
    const std::size_t wire_size = seg.size() + pages * kTailSize;
    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(pages);
    std::size_t off = 0;
    for (std::size_t page = 1; page <= pages; ++page)
    {
        const std::size_t n = std::min(kDataPerPage, seg.size() - off);
        std::vector<std::uint8_t> chunk(n + kTailSize);
        std::memcpy(chunk.data(), seg.data() + off, n);
        auto* tail = chunk.data() + n;
        put_u16(tail, static_cast<std::uint16_t>(pages));
        put_u16(tail + 2, static_cast<std::uint16_t>(page));
        put_u32(tail + 4, static_cast<std::uint32_t>(wire_size));
        put_u32(tail + 8, msg_id);
        off += n;
        out.push_back(std::move(chunk));
    }
    return out;
}

struct Endpoint
{
    std::string ip;
    uint16_t port{};
};

Endpoint endpoint_for(const std::string& topic)
{
    return Endpoint{dzIPC::common::udp_discovery_addr_calculate(topic),
                    dzIPC::common::udp_discovery_port_calculate(topic, kDomain)};
}

/* 按约定把一帧 DZFlat 段从 UDP 发出去。返回 false = 环境没有组播, 用例应跳过。 */
bool send_segment(const std::string& topic, const std::vector<std::uint8_t>& seg, std::uint32_t msg_id)
{
    const Endpoint ep = endpoint_for(topic);
    ipc::socket::UDPNode tx("borrow_tx", ep.ip.c_str(), ep.port, ipc::socket::NodeRole::SendOnly);
    if (!tx.connect())
    {
        return false;
    }
    for (auto& chunk : frame_segment(seg, msg_id))
    {
        ipc::buffer b(chunk.data(), chunk.size());
        tx.send(b);
    }
    return true;
}

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

/* 轮询 try_get 直到有借样或超时。 */
bool wait_borrow(dzIPC::socket::socket_sub_ipc& sub, dzIPC::Sample& out, std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (sub.try_get(out))
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

}   // namespace

/* ① 借样: DZFlat 段走 UDP 时必须落进视图队列, 字段/大数组都要读得出来。
 *
 * 段长 > 1460 ⇒ **跨页** ⇒ 覆盖去帧(页尾插在段中间)。
 * 严格二选一: 同一条消息**不得**同时出现在物化队列。 */
TEST(SocketBorrow, DzFlatSegmentOverUdpLandsOnTheViewPath)
{
    const std::string topic = unique_topic("view");
    auto sub_td = make_td();
    dzIPC::socket::socket_sub_ipc sub{sub_td, topic, kDomain, 8, false};
    sub.InitChannel("borrow");
    std::this_thread::sleep_for(500ms);

    const auto src = make_image(160, 120);   // data ≈ 57 KB ⇒ 段约 57.6 KB ⇒ 40 页
    const auto seg = make_segment(src);
    ASSERT_EQ(declared_seg_len(seg), seg.size()) << "dzflat_size() 应是精确值, 不是上界";
    ASSERT_GT(seg.size(), 2 * kDataPerPage) << "本用例要求跨页(去帧路径)";
    dzIPC::ResetDzFlatRxCounters();

    dzIPC::Sample sample;
    bool got = false;
    for (int attempt = 0; attempt < 20 && !got; ++attempt)
    {
        if (!send_segment(topic, seg, kMsgId))
        {
            GTEST_SKIP() << "UDP multicast socket is not available in this environment";
        }
        got = wait_borrow(sub, sample, 100ms);
    }
    ASSERT_TRUE(got) << "DZFlat 段没有通过借样路径送达 (视图队列为空)";

    EXPECT_EQ(sample.msg_id(), kMsgId);
    EXPECT_EQ(sample.schema_hash(), src.dzflat_schema_hash());
    EXPECT_EQ(sample.size(), seg.size()) << "借样段必须与发送侧的段等长(去帧后不含页尾)";
    EXPECT_GT(dzIPC::DzFlatRxCounters().dzflat_accepted, 0u);

    auto v = sample.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(v.valid()) << "bind 应通过";
    EXPECT_EQ(v.width(), src.width);
    EXPECT_EQ(v.height(), src.height);
    EXPECT_EQ(v.encoding(), "rgb8");
    EXPECT_EQ(v.header().frame_id(), "cam0");

    auto px = v.data();
    ASSERT_EQ(px.size(), src.data.size());
    /* 逐字节比对: 页尾漏补偿会让第一页之后的所有偏移错 12 字节 —— 这条断言就是它的牙。 */
    std::size_t bad = src.data.size();
    for (std::size_t i = 0; i < src.data.size(); ++i)
    {
        if (px[i] != src.data[i])
        {
            bad = i;
            break;
        }
    }
    EXPECT_EQ(bad, src.data.size()) << "借样段内容与源不一致, 首个不符字节 = " << bad;

    /* 严格二选一: 借样段绝不能再出现在物化队列。 */
    auto sink = make_td();
    EXPECT_FALSE(sub.try_get_clone(sink)) << "同一条消息同时进了两条队列 —— 违反严格分流";
}

/* ② 不回归: DZFlat 关闭 ⇒ 话题恒发 TLV ⇒ 视图队列必须**永远为空**, 物化路径照常。 */
TEST(SocketBorrow, TlvTopicKeepsTheMaterializedPath)
{
    DzFlatSwitch off{false};
    const std::string topic = unique_topic("tlv");
    auto pub_td = make_td();
    auto sub_td = make_td();
    dzIPC::socket::socket_pub_ipc pub{pub_td, topic, kDomain, false};
    dzIPC::socket::socket_sub_ipc sub{sub_td, topic, kDomain, 8, false};
    pub.InitChannel("borrow");
    sub.InitChannel("borrow");
    std::this_thread::sleep_for(500ms);

    const auto src = make_image(8, 4);
    auto sink = make_td();
    bool got = false;
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (!got && std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        if (sub.try_get_clone(sink))
        {
            got = true;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(got) << "TLV 物化路径没有送达";

    dzIPC::Sample sample;
    EXPECT_FALSE(sub.try_get(sample)) << "TLV 消息不得出现在视图路径";
    /* 并且带超时的视图取必须**超时返回**, 不是永久阻塞 —— 判据是时间窗, 不是返回值:
     * 一个直接 return false 的实现也能过"返回值"那一条。 */
    const auto t0 = std::chrono::steady_clock::now();
    const bool timed = sub.get(sample, 200);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    EXPECT_FALSE(timed);
    EXPECT_GE(elapsed, 150) << "立刻返回 ⇒ 有数据时会漏消息";
    EXPECT_LT(elapsed, 2000) << "超过预算还没回来 ⇒ 又变回挂死形态";
}

/* ③ 生命周期与 SHM 一致: Sample 活着期间内容必须稳定(段是它自己的独立缓冲, 后续
 *    消息不能改写到它)。 */
TEST(SocketBorrow, HeldSampleStaysIntactWhileMoreTrafficArrives)
{
    const std::string topic = unique_topic("hold");
    auto sub_td = make_td();
    dzIPC::socket::socket_sub_ipc sub{sub_td, topic, kDomain, 8, false};
    sub.InitChannel("borrow");
    std::this_thread::sleep_for(500ms);

    const auto src_a = make_image(64, 48);          // 约 9 KB ⇒ 跨页
    const auto seg_a = make_segment(src_a);
    dzIPC::Sample held;
    bool got = false;
    for (int attempt = 0; attempt < 20 && !got; ++attempt)
    {
        if (!send_segment(topic, seg_a, kMsgId))
        {
            GTEST_SKIP() << "UDP multicast socket is not available in this environment";
        }
        got = wait_borrow(sub, held, 100ms);
    }
    ASSERT_TRUE(got);

    /* 再猛发别的段, 然后回来读 held。 */
    const auto seg_b = make_segment(make_image(32, 24));
    for (int i = 0; i < 30; ++i)
    {
        send_segment(topic, seg_b, kMsgId);
        dzIPC::Sample extra;
        wait_borrow(sub, extra, 20ms);
    }

    auto v = held.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(v.valid()) << "持有期间卷承载的段必须仍然有效";
    EXPECT_EQ(v.width(), src_a.width);
    auto px = v.data();
    ASSERT_EQ(px.size(), src_a.data.size());
    EXPECT_EQ(std::memcmp(px.data(), src_a.data.data(), px.size()), 0)
        << "持有中的借样段被后续流量改写 —— 生命周期契约破了";
}

/* ④ 段头 msg_id 与话题不符 ⇒ 丢弃(计数 +1), 两条队列都不得收下。 */
TEST(SocketBorrow, MismatchedSegmentIdIsDropped)
{
    const std::string topic = unique_topic("badid");
    auto sub_td = make_td();
    dzIPC::socket::socket_sub_ipc sub{sub_td, topic, kDomain, 8, false};
    sub.InitChannel("borrow");
    std::this_thread::sleep_for(500ms);
    dzIPC::ResetDzFlatRxCounters();

    /* 页尾的 msg_id 必须对上(否则连组装配送都进不来), 但**段头**的 msg_id 不符:
     * 这才是"同一通道上跑着别的消息类型"的真实形态。
     * 段头布局(dzflat.h 的 SegHeader): magic(0) / schema_hash(4) / root_off(8) /
     * root_size(12) / total_size(16) / layout_ver(20) / flags(22) / **msg_id(24)**。 */
    const auto seg = make_segment(make_image(8, 4));
    std::vector<std::uint8_t> bad = seg;
    bad[24] = 0xEE;
    bad[25] = 0xEE;
    for (int i = 0; i < 3; ++i)
    {
        if (!send_segment(topic, bad, kMsgId))
        {
            GTEST_SKIP() << "UDP multicast socket is not available in this environment";
        }
        std::this_thread::sleep_for(30ms);
    }

    dzIPC::Sample sample;
    EXPECT_FALSE(sub.try_get(sample)) << "段头 msg_id 不符的段不得进视图队列";
    auto sink = make_td();
    EXPECT_FALSE(sub.try_get_clone(sink)) << "更不得进物化队列(它不是 TLV, 反序列化只会读出错数据)";
    EXPECT_GT(dzIPC::DzFlatRxCounters().dzflat_id_skipped, 0u) << "拒收必须可观测";
}
