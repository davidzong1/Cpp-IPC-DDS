/* 接收侧拒收计数的回归 (docs/dzflat_shm.md §3.5 的升级顺序靠它可运维)
 *
 * 要守的是什么: 接收侧的四种拒收原本都是裸 continue —— 不记日志、不抛错、无计数。于是
 * "版本错配"这种一定会发生的部署事故, 现场表现是"消息量对不上, 但两端都不报错"。
 *
 * 每条用例都断言**恰好那一项 +1 且其余五项不动**。只断言"目标项涨了"是没有牙的: 一个
 * 把所有事件都记到同一项上的实现、或者干脆返回常量的实现, 都能过。
 *
 * 计数埋在 AcceptWire 里(pub/sub 与 ser/cli 共用), 所以这里直接测 AcceptWire 就覆盖了
 * 两条传输路径; 末尾另有一条走**真实 SHM 传输**的用例, 证明这个函数确实在链路上, 而
 * 不是只被单测调到。
 */
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {

using namespace std::chrono_literals;
using dzIPC::DzFlatRxStats;

constexpr std::uint32_t kMsgId = 77;

/* 计数快照 + 差分。EXPECT_ONLY 断言"只有指定那一项动了 delta", 这是本文件的牙。 */
class Snap
{
public:
    Snap() { dzIPC::ResetDzFlatRxCounters(); }

    DzFlatRxStats delta() const { return dzIPC::DzFlatRxCounters(); }
};

#define EXPECT_ONLY(st, field, n)                                                        \
    do                                                                                   \
    {                                                                                    \
        const auto& _s = (st);                                                           \
        EXPECT_EQ(_s.field, static_cast<std::uint64_t>(n)) << #field " 应为 " << (n);     \
        std::uint64_t _total = _s.dzflat_accepted + _s.dzflat_id_skipped                  \
                               + _s.dzflat_header_bad + _s.dzflat_schema_drop             \
                               + _s.tlv_accepted + _s.tlv_id_skipped                     \
                               + _s.tlv_corrupt_drop;                                    \
        EXPECT_EQ(_total, static_cast<std::uint64_t>(n))                                  \
            << "除 " #field " 外还有别的项被记了 —— 事件分派串了: " << _s.dzflat_accepted \
            << "/" << _s.dzflat_id_skipped << "/" << _s.dzflat_header_bad << "/"         \
            << _s.dzflat_schema_drop << "/" << _s.tlv_accepted << "/"                    \
            << _s.tlv_id_skipped << "/" << _s.tlv_corrupt_drop;                          \
    } while (0)

dzIPC::Msg::StdImage make_image(std::size_t payload = 64)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "cam0";
    img.header.stamp = 1.5;
    img.width = 8;
    img.height = 4;
    img.step = 24;
    img.encoding = "rgb8";
    img.data.assign(payload, 0x3C);
    img.set_msg_id(kMsgId);
    return img;
}

ipc::buffer to_buffer(std::vector<std::uint8_t>& v)
{
    return ipc::buffer(v.data(), v.size(), [](void*, std::size_t) {});
}

/* 一个合法的 DZFlat 段, msg_id = kMsgId。 */
std::vector<std::uint8_t> make_dzflat_seg()
{
    auto img = make_image();
    std::vector<std::uint8_t> seg(img.dzflat_size());
    EXPECT_TRUE(img.dzflat_write(seg.data(), static_cast<std::uint32_t>(seg.size())));
    return seg;
}

}   // namespace

/* ---------------------------------------------------------------- DZFlat 三种归宿 */

TEST(WireAcceptCounters, DzFlatGoodSegmentCountsAccepted)
{
    auto seg = make_dzflat_seg();
    auto buf = to_buffer(seg);
    dzIPC::Msg::StdImage sink;
    sink.set_msg_id(kMsgId);

    Snap snap;
    EXPECT_TRUE(dzIPC::AcceptWire(buf, kMsgId, sink));
    EXPECT_ONLY(snap.delta(), dzflat_accepted, 1);
    EXPECT_EQ(sink.encoding, "rgb8") << "收下了就该解出来";
}

TEST(WireAcceptCounters, DzFlatWrongMsgIdCountsIdSkipped)
{
    auto seg = make_dzflat_seg();
    auto buf = to_buffer(seg);
    dzIPC::Msg::StdImage sink;

    Snap snap;
    /* 同一通道上多种消息混跑时本就该跳过 —— 这是**正常**过滤, 与缺陷分开计。 */
    EXPECT_FALSE(dzIPC::AcceptWire(buf, kMsgId + 1, sink));
    EXPECT_ONLY(snap.delta(), dzflat_id_skipped, 1);
}

TEST(WireAcceptCounters, DzFlatSchemaMismatchCountsSchemaDrop)
{
    auto seg = make_dzflat_seg();
    /* 只改 schema_hash(段头偏移 4), msg_id 保持正确 —— 这正是"两端 msg 定义不是同一份"
     * 在 wire 上的样子: 前面每一道门都过, 只有结构指纹对不上。 */
    std::uint32_t bad = 0xDEADBEEFu;
    std::memcpy(seg.data() + 4, &bad, sizeof(bad));

    auto buf = to_buffer(seg);
    dzIPC::Msg::StdImage sink;
    sink.set_msg_id(kMsgId);

    Snap snap;
    EXPECT_FALSE(dzIPC::AcceptWire(buf, kMsgId, sink))
        << "结构不符必须拒收 —— 按错误的定长布局解出的是能通过所有校验的垃圾值";
    EXPECT_ONLY(snap.delta(), dzflat_schema_drop, 1);
}

TEST(WireAcceptCounters, DzFlatTruncatedSegmentCountsHeaderBad)
{
    /* magic 还在但 total_size 声称比缓冲还长 —— 段被截断或被覆写的样子(套圈时会出现)。
     *
     * 这条守的是一个易犯的错: 判别失败就直接落进 TLV 分支。那里的尾部校验必然失配, 于是
     * 一个**损坏的 DZFlat 段**会被记成"这条不是我的话题"这种正常过滤, 真信号就此消失。 */
    auto seg = make_dzflat_seg();
    std::uint32_t huge = 0xFFFFFF00u;
    std::memcpy(seg.data() + 16, &huge, sizeof(huge));   /* total_size 在偏移 16 */

    auto buf = to_buffer(seg);
    dzIPC::Msg::StdImage sink;
    sink.set_msg_id(kMsgId);

    Snap snap;
    EXPECT_FALSE(dzIPC::AcceptWire(buf, kMsgId, sink));
    EXPECT_ONLY(snap.delta(), dzflat_header_bad, 1);
}

/* ------------------------------------------------------------------- TLV 三种归宿 */

TEST(WireAcceptCounters, TlvGoodBufferCountsAccepted)
{
    auto img = make_image();
    auto ser = img.serialize();
    dzIPC::Msg::StdImage sink;

    Snap snap;
    EXPECT_TRUE(dzIPC::AcceptWire(ser, kMsgId, sink));
    EXPECT_ONLY(snap.delta(), tlv_accepted, 1);
    EXPECT_EQ(sink.encoding, "rgb8");
}

TEST(WireAcceptCounters, TlvWrongMsgIdCountsIdSkipped)
{
    auto img = make_image();
    auto ser = img.serialize();
    dzIPC::Msg::StdImage sink;

    Snap snap;
    EXPECT_FALSE(dzIPC::AcceptWire(ser, kMsgId + 1, sink));
    EXPECT_ONLY(snap.delta(), tlv_id_skipped, 1);
}

TEST(WireAcceptCounters, TlvTruncatedBufferCountsCorruptDrop)
{
    /* 构造错乱缓冲: 保留完整的 12 字节页尾(于是 check_id 会通过), 但把前面的数据截掉一
     * 大块。TLV 的偏移全由缓冲自身内容算出 —— 前缀里声明的数组长度还是原来那么大, 于
     * 是读取会推到缓冲之外。这就是 known_issues 第 3 条那条崩溃路径的最小复现。 */
    auto img = make_image(4096);
    auto ser = img.serialize();
    ASSERT_GT(ser.size(), 2048u);

    std::vector<std::uint8_t> bad;
    const auto* p = static_cast<const std::uint8_t*>(ser.data());
    const std::size_t keep = ser.size() / 4;
    bad.assign(p, p + keep);
    bad.insert(bad.end(), p + ser.size() - 12, p + ser.size());   /* 补回页尾 */

    auto buf = to_buffer(bad);
    dzIPC::Msg::StdImage sink;

    Snap snap;
    EXPECT_FALSE(dzIPC::AcceptWire(buf, kMsgId, sink))
        << "越界缓冲必须整条丢弃 —— 否则 deserialize 会读到映射之外(实测能到 SIGSEGV)";
    EXPECT_ONLY(snap.delta(), tlv_corrupt_drop, 1);
}

/* ------------------------------------------------- GenericMessage 是直通体, 不设结构门 */

TEST(WireAcceptCounters, GenericMessageAcceptsForeignSchema)
{
    /* Python 侧的载体没有 schema, 于是对任何格式合法的段都收下 —— C++ 层的
     * dzflat_schema_drop 在 Python 进程里恒为 0。这不是缺陷, 是分工: 版本错配到
     * Python 按指纹查表时才暴露, 计数在 dzipc.dzflat.rx_stats()。
     * 本用例把这个分工钉住, 免得有人以为 C++ 计数已经覆盖了 Python 进程。 */
    auto seg = make_dzflat_seg();
    std::uint32_t bad = 0xDEADBEEFu;
    std::memcpy(seg.data() + 4, &bad, sizeof(bad));

    auto buf = to_buffer(seg);
    dzIPC::GenericMessage sink;
    sink.set_msg_id(kMsgId);

    Snap snap;
    EXPECT_TRUE(dzIPC::AcceptWire(buf, kMsgId, sink))
        << "直通体应当收下未知指纹的段(留给持有 schema 的一侧去判)";
    EXPECT_ONLY(snap.delta(), dzflat_accepted, 1);
    EXPECT_TRUE(sink.has_dzflat());
    EXPECT_EQ(sink.dzflat_seg_schema_hash(), bad) << "指纹要原样带出去, 供上层查表";
}

/* --------------------------------------------------- 计数确实挂在真实传输路径上 */

TEST(WireAcceptCounters, RealShmTransportFeedsTheCounters)
{
    /* 前面全是直接调 AcceptWire。这一条走真实 SHM pub/sub, 用来证明订阅循环换成
     * AcceptWire 之后计数真的在动 —— 否则以上六条可以全绿而线上一个数都不涨。 */
    const std::string topic = "wire_accept_real";
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 16};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    dzIPC::ResetDzFlatRxCounters();
    auto img = std::make_shared<dzIPC::Msg::StdImage>(make_image());
    for (int i = 0; i < 5; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(*img);
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }
    /* 开关默认 OFF, 所以这五条走 TLV。 */
    for (int i = 0; i < 40 && dzIPC::DzFlatRxCounters().tlv_accepted < 5; ++i)
    {
        std::this_thread::sleep_for(50ms);
    }
    const auto st = dzIPC::DzFlatRxCounters();
    EXPECT_GE(st.tlv_accepted, 5u)
        << "真实订阅循环没有喂到计数器 —— AcceptWire 没接进链路";
    EXPECT_EQ(st.defects(), 0u) << "正常链路不该有缺陷计数";
}
