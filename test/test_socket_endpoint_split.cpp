/* 端点分离的核心断言。
 *
 * 背景: 组播 socket 的 IP_MULTICAST_LOOP 必须保持 1, 否则同机其他进程收不到 ——
 * 那是主机级开关, 不是"只屏蔽自己"。于是收发共用一条入组 socket 的发送端, 会把
 * 自己发出的每一个分片都收回来(1 MB = 713 片), 对端的 ACK 排在它们之后, 等待
 * 窗口必然先超时。实测吞吐塌到 1 msg/s 而丢包率 0.00% —— 数据面通, 确认面坏。
 *
 * 解法是让只发送的端点不加入组播组(IP_ADD_MEMBERSHIP 只影响接收, 不影响发送)。
 * 下面的测试就是在验证这一点确实成立, 且没有波及别人。 */
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "ipc_msg/ipc_msg_base/udp_rtps_ack_msg.hpp"
#include "libipc/udp.h"

#include <gtest/gtest.h>

namespace {

struct Endpoint
{
    std::string ip;
    uint16_t port{};
};

Endpoint endpoint_for(const std::string& name)
{
    return Endpoint{dzIPC::common::udp_discovery_addr_calculate(name),
                    dzIPC::common::udp_discovery_port_calculate(name, 1)};
}

/* 发若干个 1472 字节的包。改动前这些包会全部回绕到发送者自己的接收队列。 */
void blast(ipc::socket::UDPNode& node, int count)
{
    std::vector<uint8_t> payload(1'472, 0xA5);
    for (int i = 0; i < count; ++i)
    {
        ipc::buffer buf(payload.data(), payload.size());
        node.send(buf);
    }
}

int drain_count(ipc::socket::UDPNode& node, int cap = 4'096)
{
    int seen = 0;
    for (int i = 0; i < cap; ++i)
    {
        ipc::buffer got = node.receive_nowait();
        if (got.empty())
        {
            break;
        }
        ++seen;
    }
    return seen;
}

}   // namespace

/* SendOnly 节点收不到自己发出去的包。这是端点分离的全部要点。
 *
 * 局限: `receive_nowait()` 对 SendOnly 会直接短路返回空, 所以本用例证明的是
 * **契约**("SendOnly 永远不产出数据", data_rev 的 ACK 等待循环依赖这一点),
 * 而不是"确实没有调用 IP_ADD_MEMBERSHIP"。若哪天误加了入组, 这里仍会通过,
 * 症状会变成内核收包缓冲被无人排空的回绕分片填满 —— 浪费但不致错。
 * 真正证明"没入组"需要绕过公开 API 直接查 socket 状态, 不值得为此开洞。 */
TEST(EndpointSplit, SendOnlyDoesNotReceiveItsOwnTraffic)
{
    const Endpoint ep = endpoint_for("endpoint_split_self");
    ipc::socket::UDPNode sender("sender", ep.ip.c_str(), ep.port, ipc::socket::NodeRole::SendOnly);
    if (!sender.connect())
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }

    blast(sender, 200);
    /* 回绕是内核在 sendto 内部完成的, 但给调度留一点余量, 避免"还没到"被误判成
     * "屏蔽成功"。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(drain_count(sender), 0);
}

/* 但屏蔽只能作用于自己 —— 同机的接收端必须照常收到。
 * 这一条守住的是"没有误用 IP_MULTICAST_LOOP=0"。 */
TEST(EndpointSplit, SendOnlyStillReachesOtherLocalSockets)
{
    const Endpoint ep = endpoint_for("endpoint_split_peer");
    ipc::socket::UDPNode receiver("receiver", ep.ip.c_str(), ep.port, ipc::socket::NodeRole::RecvOnly);
    ipc::socket::UDPNode sender("sender", ep.ip.c_str(), ep.port, ipc::socket::NodeRole::SendOnly);
    if (!receiver.connect() || !sender.connect())
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }

    blast(sender, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GT(drain_count(receiver), 0);
    EXPECT_EQ(drain_count(sender), 0);   // 发送者自己仍然什么都收不到
}

/* 缺省角色必须保持历史行为(入组、收发共用), 否则 test_socket.cpp 这类
 * 依赖同进程回环的既有用法会静默失效。 */
TEST(EndpointSplit, DefaultRoleKeepsLoopback)
{
    const Endpoint ep = endpoint_for("endpoint_split_default");
    ipc::socket::UDPNode node("duplex", ep.ip.c_str(), ep.port);
    if (!node.connect())
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }
    ASSERT_EQ(node.role(), ipc::socket::NodeRole::SendRecv);

    blast(node, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GT(drain_count(node), 0);   // 缺省仍然收得到自己的回绕
}

/* Heartbeat 的线格式往返。它携带的 sequence 是 12 字节 tail 装不下的字段,
 * 接收端只能从这里拿到 —— 序列化写错会让 ACK 的 sequence 校验永远不匹配。 */
TEST(Heartbeat, WireRoundTrip)
{
    IpcRtpsHeartbeatMsg hb;
    hb.page_cnt = 713;
    hb.total_size = 1'049'200;
    hb.data_msg_id = 4'242;
    hb.sender_id = 0xAB'CD'12'34;
    hb.sequence = 99;
    hb.flags = IpcRtpsHeartbeatMsg::kFlagReliable;
    hb.round = 1;

    ipc::buffer wire = hb.serialize();
    EXPECT_EQ(wire.size(), IpcRtpsHeartbeatMsg::kWireSize);

    IpcRtpsHeartbeatMsg rx;
    ASSERT_TRUE(rx.check_hb_id(wire));
    rx.deserialize(wire);

    EXPECT_EQ(rx.page_cnt, 713);
    EXPECT_EQ(rx.total_size, 1'049'200u);
    EXPECT_EQ(rx.data_msg_id, 4'242u);
    EXPECT_EQ(rx.sender_id, 0xAB'CD'12'34u);
    EXPECT_EQ(rx.sequence, 99u);
    EXPECT_NE(rx.flags & IpcRtpsHeartbeatMsg::kFlagReliable, 0);
    EXPECT_EQ(rx.round, 1);
}

/* 控制帧必须能被识别出来, 否则 chunk_rev_sniff 会把 33 字节的 heartbeat
 * 当成一条合法的单页用户消息输出。 */
TEST(Heartbeat, ControlFramesAreRecognised)
{
    EXPECT_TRUE(is_rtps_control_frame(IpcRtpsHeartbeatMsg::kRtpsHeartbeatMsgId));
    EXPECT_TRUE(is_rtps_control_frame(IpcRtpsAckMsg::kRtpsAckMsgId));
    EXPECT_TRUE(is_rtps_control_frame(IpcRtpsNackMsg::kRtpsNackMsgId));
    EXPECT_TRUE(is_rtps_control_frame(IpcRtpsNackBitmapMsg::kRtpsNackBitmapMsgId));
    EXPECT_FALSE(is_rtps_control_frame(4'242));   // 普通用户消息
}

/* ---------------- NACK 位图压缩 ---------------- */

/* 位图往返。bit i 表示 base_page + i 缺失 —— 这个约定写反(改成"最后收到的页")
 * 会让发送端重传一批完全不相干的分片, 且不会有任何报错。 */
TEST(NackBitmap, WireRoundTrip)
{
    IpcRtpsNackBitmapMsg nb;
    nb.page_cnt = 713;
    nb.total_size = 1'049'200;
    nb.data_msg_id = 4'242;
    nb.receiver_id = 0x11'22'33'44;
    nb.sequence = 7;
    nb.reset_window(/*base=*/100, /*bits=*/300);
    nb.set_missing(100);   // 窗口第一位
    nb.set_missing(101);
    nb.set_missing(255);
    nb.set_missing(399);   // 窗口最后一位

    ipc::buffer wire = nb.serialize();

    IpcRtpsNackBitmapMsg rx;
    ASSERT_TRUE(rx.check_nb_id(wire));
    rx.deserialize(wire);

    EXPECT_EQ(rx.page_cnt, 713);
    EXPECT_EQ(rx.total_size, 1'049'200u);
    EXPECT_EQ(rx.data_msg_id, 4'242u);
    EXPECT_EQ(rx.receiver_id, 0x11'22'33'44u);
    EXPECT_EQ(rx.sequence, 7u);
    EXPECT_EQ(rx.base_page, 100);

    EXPECT_TRUE(rx.is_missing(100));
    EXPECT_TRUE(rx.is_missing(101));
    EXPECT_TRUE(rx.is_missing(255));
    EXPECT_TRUE(rx.is_missing(399));
    EXPECT_FALSE(rx.is_missing(102));
    EXPECT_FALSE(rx.is_missing(254));
    EXPECT_FALSE(rx.is_missing(398));
    EXPECT_FALSE(rx.is_missing(99));    // 窗口之前
    EXPECT_FALSE(rx.is_missing(500));   // 窗口之后
}

/* §7.1 实测的丢包形态: 255 片连续空洞。这正是位图存在的理由 ——
 * 显式列表要 510 B 且恰好顶满 256 片上限, 位图只要 32 B。 */
TEST(NackBitmap, BurstGapIsMuchSmallerThanExplicitList)
{
    IpcRtpsNackBitmapMsg nb;
    nb.page_cnt = 713;
    nb.reset_window(/*base=*/50, /*bits=*/255);
    for (uint16_t p = 50; p < 50 + 255; ++p)
    {
        nb.set_missing(p);
    }
    ipc::buffer bitmap_wire = nb.serialize();

    IpcRtpsNackMsg list;
    list.page_cnt = 713;
    for (uint16_t p = 50; p < 50 + 255; ++p)
    {
        list.missing_pages.push_back(p);
    }
    ipc::buffer list_wire = list.serialize();

    EXPECT_LT(bitmap_wire.size(), list_wire.size() / 4);
    /* 且必须仍是单页 —— 分片后的 NACK 会被发送端逐 datagram 反序列化出半截垃圾。 */
    EXPECT_LE(bitmap_wire.size(), IpcRtpsNackBitmapMsg::kMaxWireBytes + 12);
}

/* 全域缺失(713 片全丢)也必须落在一页内。这是位图相对显式列表最大的优势:
 * 显式列表一轮只能报 256 片, 剩下的要多等一整个 round_wait_ms。 */
TEST(NackBitmap, FullMessageGapFitsInOnePage)
{
    IpcRtpsNackBitmapMsg nb;
    nb.page_cnt = 713;
    nb.reset_window(/*base=*/1, /*bits=*/713);
    for (uint16_t p = 1; p <= 713; ++p)
    {
        nb.set_missing(p);
    }
    ipc::buffer wire = nb.serialize();
    EXPECT_LE(wire.size(), IpcRtpsNackBitmapMsg::kMaxWireBytes + 12);

    IpcRtpsNackBitmapMsg rx;
    rx.deserialize(wire);
    for (uint16_t p = 1; p <= 713; ++p)
    {
        ASSERT_TRUE(rx.is_missing(p)) << "page " << p;
    }
}

/* 畸形帧不得越界读。adapt_memcpy_tods 不做边界检查, 所以 deserialize 必须自己把
 * 线上声明的 map_bytes 夹在实际可读字节内。这个用例在 ASan 下才有完整意义,
 * 但即使不开 ASan, 读出的 bit_cnt 也必须是被夹过的合理值。 */
TEST(NackBitmap, TruncatedFrameIsRejectedNotOverread)
{
    IpcRtpsNackBitmapMsg nb;
    nb.page_cnt = 713;
    nb.reset_window(/*base=*/1, /*bits=*/713);
    nb.set_missing(1);
    ipc::buffer wire = nb.serialize();

    /* 砍掉一半载荷, 但保留尾部 12 字节 tail, 使 check_nb_id 仍然通过 ——
     * 模拟"map_bytes 字段声称的长度远大于实际可读字节"。 */
    const std::size_t full = wire.size();
    ASSERT_GT(full, 12u + IpcRtpsNackBitmapMsg::kFixedFieldBytes);
    const std::size_t cut = IpcRtpsNackBitmapMsg::kFixedFieldBytes + 20 + 12;
    ASSERT_LT(cut, full);

    std::vector<uint8_t> shrunk(cut);
    std::memcpy(shrunk.data(), wire.data(), cut - 12);
    std::memcpy(shrunk.data() + cut - 12, static_cast<const uint8_t*>(wire.data()) + full - 12, 12);

    ipc::buffer bad(shrunk.data(), shrunk.size());
    IpcRtpsNackBitmapMsg rx;
    ASSERT_TRUE(rx.check_nb_id(bad));
    rx.deserialize(bad);

    EXPECT_LE(rx.bit_cnt, 20u * 8u);
    EXPECT_LE(rx.bitmap.size(), 20u);
}

/* 太短的帧整帧拒绝, 不留下半读状态。 */
TEST(NackBitmap, UndersizedFrameLeavesFieldsUntouched)
{
    std::vector<uint8_t> tiny(16, 0);
    ipc::buffer bad(tiny.data(), tiny.size());

    IpcRtpsNackBitmapMsg rx;
    rx.page_cnt = 0xFFFF;   // 哨兵: deserialize 若真的动了字段, 这里会变
    rx.deserialize(bad);
    EXPECT_EQ(rx.page_cnt, 0xFFFF);
    EXPECT_TRUE(rx.bitmap.empty());
}

/* 能力协商: 只有 HB 带 kFlagBitmapNack 时接收端才可以发位图。
 * 旧版发送端的 check_id 会丢弃 DZNB —— 那一轮等于没发 NACK, Reliable 会从
 * "能重传"退化成"必然超时", 比多花几百字节严重得多。 */
TEST(NackBitmap, CapabilityFlagSurvivesHeartbeatRoundTrip)
{
    IpcRtpsHeartbeatMsg hb;
    hb.page_cnt = 713;
    hb.total_size = 1'049'200;
    hb.data_msg_id = 4'242;
    hb.sequence = 5;
    hb.flags = IpcRtpsHeartbeatMsg::kFlagReliable | IpcRtpsHeartbeatMsg::kFlagBitmapNack;

    ipc::buffer wire = hb.serialize();
    EXPECT_EQ(wire.size(), IpcRtpsHeartbeatMsg::kWireSize);   // 加位不得改变线格式尺寸

    IpcRtpsHeartbeatMsg rx;
    rx.deserialize(wire);
    EXPECT_NE(rx.flags & IpcRtpsHeartbeatMsg::kFlagBitmapNack, 0);
    EXPECT_NE(rx.flags & IpcRtpsHeartbeatMsg::kFlagReliable, 0);
    EXPECT_EQ(rx.flags & IpcRtpsHeartbeatMsg::kFlagFinal, 0);
}

/* 两种 NACK 必须是不同的 msg_id, 否则旧版发送端会拿 IpcRtpsNackMsg 去解析
 * 位图帧: 格式字段被当成 miss_cnt 高字节, clamp 到 256 后从几十字节的 buffer
 * 里读 512 字节 —— 一次真实的越界读。 */
TEST(NackBitmap, DistinctMsgIdFromExplicitList)
{
    EXPECT_NE(IpcRtpsNackBitmapMsg::kRtpsNackBitmapMsgId, IpcRtpsNackMsg::kRtpsNackMsgId);

    IpcRtpsNackBitmapMsg nb;
    nb.page_cnt = 10;
    nb.reset_window(1, 10);
    nb.set_missing(3);
    ipc::buffer nb_wire = nb.serialize();

    IpcRtpsNackMsg old_parser;
    EXPECT_FALSE(old_parser.check_nk_id(nb_wire));   // 旧版发送端会安全地忽略整帧
}

/* ---------------- 编码择优判据 ----------------
 *
 * send_nack_auto 是 data_rev.cc 的内部函数(匿名 namespace), 测不到。这里复现它
 * 的判据本身 —— 判据写错的后果不是崩溃, 而是"某些丢包形态下每轮少报一批片",
 * 表现为吞吐莫名偏低, 极难从现象反推。所以判据值得单独钉住。 */
namespace {

struct NackChoice
{
    bool use_bitmap{false};
    std::size_t bitmap_cover{0};
    std::size_t list_cover{0};
};

/* 与 data_rev.cc::send_nack_auto 保持一致的判据: 先比覆盖率, 再比字节数。 */
NackChoice choose_nack_encoding(const std::vector<uint8_t>& received, uint16_t page_cnt)
{
    std::size_t miss_cnt = 0;
    uint16_t first_missing = 0;
    uint16_t last_missing = 0;
    for (uint16_t i = 1; i <= page_cnt; ++i)
    {
        if (received[i] == 0)
        {
            ++miss_cnt;
            if (first_missing == 0)
            {
                first_missing = i;
            }
            last_missing = i;
        }
    }

    NackChoice out;
    if (miss_cnt == 0)
    {
        return out;
    }

    const std::size_t span = static_cast<std::size_t>(last_missing) - first_missing + 1;
    const std::size_t bitmap_bits = std::min(span, IpcRtpsNackBitmapMsg::kMaxBitmapBits);
    const std::size_t bitmap_bytes = (bitmap_bits + 7) / 8;

    out.bitmap_cover = miss_cnt;
    if (span > IpcRtpsNackBitmapMsg::kMaxBitmapBits)
    {
        out.bitmap_cover = 0;
        const std::size_t window_end = static_cast<std::size_t>(first_missing) + bitmap_bits;
        for (std::size_t i = first_missing; i < window_end && i <= page_cnt; ++i)
        {
            if (received[i] == 0)
            {
                ++out.bitmap_cover;
            }
        }
    }

    out.list_cover = std::min(miss_cnt, IpcRtpsNackMsg::kMaxMissingPages);
    const std::size_t list_bytes = out.list_cover * 2;
    out.use_bitmap = (out.bitmap_cover > out.list_cover)
                     || (out.bitmap_cover == out.list_cover && bitmap_bytes < list_bytes);
    return out;
}

std::vector<uint8_t> all_received(uint16_t page_cnt)
{
    return std::vector<uint8_t>(static_cast<std::size_t>(page_cnt) + 1, 1);
}

}   // namespace

/* §7.1 实测形态: 255 片连续空洞。位图 32 B vs 显式列表 510 B, 覆盖率相同,
 * 应当选位图。 */
TEST(NackChoice, BurstGapPrefersBitmap)
{
    constexpr uint16_t kPages = 713;
    auto received = all_received(kPages);
    for (uint16_t p = 50; p < 50 + 255; ++p)
    {
        received[p] = 0;
    }

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_TRUE(c.use_bitmap);
    EXPECT_EQ(c.bitmap_cover, c.list_cover);   // 都报得完
}

/* 单片丢失: 位图窗口 1 位 = 1 B, 显式列表 2 B。覆盖率相同, 位图更省, 选位图。
 * (差别微不足道, 但判据应当一致, 不留"小случай走另一条路"的暗角。) */
TEST(NackChoice, SingleGapPrefersBitmap)
{
    constexpr uint16_t kPages = 100;
    auto received = all_received(kPages);
    received[42] = 0;

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_TRUE(c.use_bitmap);
    EXPECT_EQ(c.bitmap_cover, 1u);
    EXPECT_EQ(c.list_cover, 1u);
}

/* 稀疏散布但跨度在窗口内: 两者都报得完, 比字节数。
 * 300 片散布在 1000 片跨度内 -> 位图 125 B, 列表 512 B(截断到 256 片)。
 * 注意此时 list_cover(256) < bitmap_cover(300), 位图靠**覆盖率**取胜。 */
TEST(NackChoice, SparseWithinWindowPrefersBitmapOnCoverage)
{
    constexpr uint16_t kPages = 1'200;
    auto received = all_received(kPages);
    for (uint16_t p = 1; p <= 300; ++p)
    {
        received[static_cast<uint16_t>(p * 3)] = 0;   // 每 3 片丢 1 片, 跨度 900
    }

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_TRUE(c.use_bitmap);
    EXPECT_EQ(c.bitmap_cover, 300u);            // 跨度 900 < 11496, 全在窗口内
    EXPECT_EQ(c.list_cover, 256u);              // 顶到上限
    EXPECT_GT(c.bitmap_cover, c.list_cover);    // 位图靠覆盖率胜出
}

/* 关键反例: 缺片稀疏地散布在**超大**消息上, 跨度超出位图窗口(11496 位)。
 * 此时位图只罩得住消息前段, 显式列表反而能报满 256 片 —— 必须选列表。
 *
 * 这正是我最初版本写错的地方: 当时只要 miss_cnt > 256 就无条件选位图,
 * 在这个形态下每轮少报一百多片, 每少报一批就多等一个 round_wait_ms。 */
TEST(NackChoice, SparseBeyondWindowPrefersExplicitList)
{
    /* 45000 片(约 64 MB), 均匀丢 300 片, 间隔 150 -> 跨度 45000 远超 11496 */
    constexpr uint16_t kPages = 45'000;
    auto received = all_received(kPages);
    for (uint16_t i = 0; i < 300; ++i)
    {
        received[static_cast<uint16_t>(100 + i * 150)] = 0;
    }

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_EQ(c.list_cover, 256u);
    EXPECT_LT(c.bitmap_cover, c.list_cover) << "窗口只罩得住前 11496 片内的缺片";
    EXPECT_FALSE(c.use_bitmap) << "位图覆盖率更低时不得选位图 —— 少报一片就多等一轮";
}

/* 密集但跨度超窗口: 位图虽然截断, 覆盖率仍远高于列表的 256, 选位图。 */
TEST(NackChoice, DenseBeyondWindowStillPrefersBitmap)
{
    constexpr uint16_t kPages = 45'000;
    auto received = all_received(kPages);
    for (uint16_t p = 1; p <= 20'000; ++p)
    {
        received[p] = 0;   // 前 20000 片全丢, 跨度 20000 > 11496
    }

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_TRUE(c.use_bitmap);
    EXPECT_EQ(c.bitmap_cover, IpcRtpsNackBitmapMsg::kMaxBitmapBits);   // 窗口内全缺
    EXPECT_GT(c.bitmap_cover, c.list_cover);
}

/* 无缺片时两种都不发。 */
TEST(NackChoice, NoMissingSendsNothing)
{
    constexpr uint16_t kPages = 100;
    const auto received = all_received(kPages);

    const NackChoice c = choose_nack_encoding(received, kPages);
    EXPECT_FALSE(c.use_bitmap);
    EXPECT_EQ(c.bitmap_cover, 0u);
    EXPECT_EQ(c.list_cover, 0u);
}
