/* DZFlat 正确性与安全性回归 (docs/dzflat_shm.md §3)
 *
 * 三类断言:
 *   ① 往返一致 —— 覆盖全部七种字段形态。ComplexMessage 覆盖标量/各类标量数组/
 *      string/string[]; RobotState 覆盖 Tier-2(元素自身带变长字段的嵌套数组);
 *      StdPointCloud 覆盖 Tier-0 元素数组; StdImage 覆盖嵌套 + 大 blob。
 *   ② 视图访问器 —— 零拷贝读到的值必须与源一致, 且 span 指向段内而非副本。
 *   ③ **不信任段内容** —— 段是别的进程写进共享内存的。schema_hash 不匹配、magic
 *      不对、长度被截断、VarRef 偏移越界, 都必须安全拒绝或返回空视图, 不得崩溃或
 *      读出段外的字节。这一组是文档 §3.7 / dzflat.h Reader 那些承诺的兜底。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"
#include "ipc_msg/test_messages/complex_message.hpp"
#include "ipc_msg/test_nested/robot_state.hpp"

namespace {

/* 按 Flat::size() 分配并写入, 返回整段字节。 */
template<typename Flat, typename Msg>
std::vector<std::uint8_t> encode(const Msg& m, std::uint32_t msg_id = 0)
{
    const std::uint32_t cap = Flat::size(m);
    std::vector<std::uint8_t> seg(cap);
    EXPECT_TRUE(Flat::write(m, seg.data(), cap, msg_id));
    const auto* h = reinterpret_cast<const dzflat::SegHeader*>(seg.data());
    /* size() 是上界: 实际写入量不得超过它, 也不得小于头 + Root。 */
    EXPECT_LE(h->total_size, cap);
    EXPECT_GE(h->total_size, sizeof(dzflat::SegHeader) + h->root_size);
    seg.resize(h->total_size);
    return seg;
}

std::uint32_t total_size_of(const std::vector<std::uint8_t>& seg)
{
    return reinterpret_cast<const dzflat::SegHeader*>(seg.data())->total_size;
}

}   // namespace

/* ---------------------------------------------------------------- ① 往返一致 */

TEST(DzFlat, ComplexMessageRoundTripCoversEveryFieldKind)
{
    dzIPC::Msg::ComplexMessage m;
    m.status = true;
    m.tiny_int = -42;
    m.tiny_uint = 200;
    m.small_int = -3000;
    m.small_uint = 60000;
    m.normal_int = -123456;
    m.normal_uint = 4000000000u;
    m.big_int = -1234567890123LL;
    m.big_uint = 12345678901234567890ULL;
    m.single_precision = 3.5f;
    m.double_precision = 2.718281828459045;
    m.message = "hello dzflat";
    m.status_array = {true, false, true, true, false};
    m.tiny_int_array = {-1, 0, 1, 127, -128};
    m.tiny_uint_array = {0, 1, 255};
    m.small_int_array = {-32768, 0, 32767};
    m.small_uint_array = {0, 65535};
    m.normal_int_array = {-1, 2, -3};
    m.normal_uint_array = {1u, 2u, 4294967295u};
    m.big_int_array = {-1LL, 1LL};
    m.big_uint_array = {0ULL, 18446744073709551615ULL};
    m.single_precision_array = {1.5f, -2.5f};
    m.double_precision_array = {1.25, -2.5, 1e300};
    m.message_array = {"a", "", "much longer string with spaces", "z"};

    const auto seg = encode<dzIPC::Msg::ComplexMessageFlat>(m);
    auto v = dzIPC::Msg::ComplexMessageView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());

    /* 视图直读 */
    EXPECT_EQ(v.status(), m.status);
    EXPECT_EQ(v.tiny_int(), m.tiny_int);
    EXPECT_EQ(v.big_uint(), m.big_uint);
    EXPECT_DOUBLE_EQ(v.double_precision(), m.double_precision);
    EXPECT_EQ(v.message(), m.message);
    ASSERT_EQ(v.status_array().size(), m.status_array.size());
    for (std::uint32_t i = 0; i < v.status_array().size(); ++i)
    {
        EXPECT_EQ(v.status_array()[i] != 0, m.status_array[i]) << "status_array[" << i << "]";
    }
    ASSERT_EQ(v.double_precision_array().size(), m.double_precision_array.size());
    EXPECT_DOUBLE_EQ(v.double_precision_array()[2], 1e300);
    ASSERT_EQ(v.message_array_count(), m.message_array.size());
    for (std::uint32_t i = 0; i < v.message_array_count(); ++i)
    {
        EXPECT_EQ(v.message_array(i), m.message_array[i]) << "message_array[" << i << "]";
    }

    /* 兼容桥拷回 owning struct */
    dzIPC::Msg::ComplexMessage back;
    v.copy_to(back);
    EXPECT_EQ(back.status, m.status);
    EXPECT_EQ(back.tiny_int, m.tiny_int);
    EXPECT_EQ(back.small_uint, m.small_uint);
    EXPECT_EQ(back.big_int, m.big_int);
    EXPECT_EQ(back.big_uint, m.big_uint);
    EXPECT_FLOAT_EQ(back.single_precision, m.single_precision);
    EXPECT_DOUBLE_EQ(back.double_precision, m.double_precision);
    EXPECT_EQ(back.message, m.message);
    EXPECT_EQ(back.status_array, m.status_array);
    EXPECT_EQ(back.tiny_int_array, m.tiny_int_array);
    EXPECT_EQ(back.small_int_array, m.small_int_array);
    EXPECT_EQ(back.normal_uint_array, m.normal_uint_array);
    EXPECT_EQ(back.big_uint_array, m.big_uint_array);
    EXPECT_EQ(back.single_precision_array, m.single_precision_array);
    EXPECT_EQ(back.double_precision_array, m.double_precision_array);
    EXPECT_EQ(back.message_array, m.message_array);
}

/* Tier-2: pose_history 是 Pose[], 而 Pose 自己带 string frame_id —— 元素 Root 连续
 * 排布, 各元素的变长负载再追加到变长区并回填其 VarRef。 */
TEST(DzFlat, Tier2NestedArrayWithVarlenElements)
{
    dzIPC::Msg::RobotState m;
    m.name = "arm0";
    m.current_pose.x = 1.0;
    m.current_pose.y = 2.0;
    m.current_pose.z = 3.0;
    m.current_pose.frame_id = "base_link";
    m.note = "note text";
    m.pose_history.resize(4);
    for (std::size_t i = 0; i < m.pose_history.size(); ++i)
    {
        m.pose_history[i].x = double(i);
        m.pose_history[i].y = double(i) * 10.0;
        m.pose_history[i].z = double(i) * 100.0;
        m.pose_history[i].frame_id = "frame_" + std::to_string(i);
    }

    const auto seg = encode<dzIPC::Msg::RobotStateFlat>(m);
    auto v = dzIPC::Msg::RobotStateView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());

    EXPECT_EQ(v.name(), m.name);
    EXPECT_EQ(v.note(), m.note);
    EXPECT_EQ(v.current_pose().frame_id(), m.current_pose.frame_id);
    EXPECT_DOUBLE_EQ(v.current_pose().y(), m.current_pose.y);
    ASSERT_EQ(v.pose_history_count(), m.pose_history.size());
    for (std::uint32_t i = 0; i < v.pose_history_count(); ++i)
    {
        EXPECT_DOUBLE_EQ(v.pose_history(i).z(), m.pose_history[i].z) << "i=" << i;
        EXPECT_EQ(v.pose_history(i).frame_id(), m.pose_history[i].frame_id) << "i=" << i;
    }

    dzIPC::Msg::RobotState back;
    v.copy_to(back);
    EXPECT_EQ(back.name, m.name);
    EXPECT_EQ(back.note, m.note);
    EXPECT_EQ(back.current_pose.frame_id, m.current_pose.frame_id);
    ASSERT_EQ(back.pose_history.size(), m.pose_history.size());
    for (std::size_t i = 0; i < back.pose_history.size(); ++i)
    {
        EXPECT_EQ(back.pose_history[i].frame_id, m.pose_history[i].frame_id);
        EXPECT_DOUBLE_EQ(back.pose_history[i].x, m.pose_history[i].x);
    }
}

/* Tier-0 元素数组: 变长区里就是一段连续的 C 数组, 且 span 必须指向段内(零拷贝),
 * 不是副本。 */
TEST(DzFlat, Tier0ElementArrayIsContiguousInSegment)
{
    constexpr std::size_t kN = 1000;
    dzIPC::Msg::StdPointCloud m;
    m.header.frame_id = "lidar";
    m.header.stamp = 7.5;
    m.points.resize(kN);
    for (std::size_t i = 0; i < kN; ++i)
        m.points[i].data = {double(i), double(i) + 0.5, double(i) - 0.5};
    m.channel_names = {"intensity", "ring"};
    m.channels.assign(kN, 1.5);

    const auto seg = encode<dzIPC::Msg::StdPointCloudFlat>(m);
    auto v = dzIPC::Msg::StdPointCloudView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());

    auto raw = v.points_raw();
    ASSERT_EQ(raw.size(), kN);
    /* 零拷贝: 元素必须落在段内存区间里。 */
    const auto* lo = seg.data();
    const auto* hi = seg.data() + seg.size();
    const auto* first = reinterpret_cast<const std::uint8_t*>(raw.data());
    EXPECT_GE(first, lo);
    EXPECT_LE(first + raw.size() * sizeof(dzIPC::Msg::StdVector3dRoot), hi);
    /* 元素记录连续: 相邻元素间距恰为 sizeof(ElemRoot)。 */
    EXPECT_EQ(reinterpret_cast<const std::uint8_t*>(&raw[1])
                  - reinterpret_cast<const std::uint8_t*>(&raw[0]),
              static_cast<std::ptrdiff_t>(sizeof(dzIPC::Msg::StdVector3dRoot)));
    for (std::uint32_t i = 0; i < kN; ++i)
    {
        EXPECT_DOUBLE_EQ(raw[i].data[1], m.points[i].data[1]) << "i=" << i;
    }
    EXPECT_EQ(v.channel_names(0), "intensity");
    EXPECT_EQ(v.channels().size(), kN);

    /* wire 不含字段名与 per-field tag, 膨胀应当贴近 1.0。 */
    const std::size_t payload = kN * 3 * sizeof(double) + kN * sizeof(double);
    EXPECT_LT(total_size_of(seg), payload + 256u);
}

TEST(DzFlat, ImageRoundTripAndEmptyFields)
{
    dzIPC::Msg::StdImage m;
    m.header.frame_id = "cam";
    m.header.stamp = 1.0;
    m.width = 4;
    m.height = 2;
    m.step = 12;
    m.encoding = "rgb8";
    m.data.resize(24);
    for (std::size_t i = 0; i < m.data.size(); ++i) m.data[i] = std::uint8_t(i);

    auto seg = encode<dzIPC::Msg::StdImageFlat>(m, 0xABCD);
    auto v = dzIPC::Msg::StdImageView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());
    EXPECT_EQ(v.width(), 4u);
    EXPECT_EQ(v.data().size(), 24u);
    EXPECT_EQ(v.encoding(), "rgb8");
    EXPECT_EQ(reinterpret_cast<const dzflat::SegHeader*>(seg.data())->msg_id, 0xABCDu);

    /* 全空: 每个变长字段的 VarRef 应为 {0,0}, 视图返回空而不是崩。 */
    dzIPC::Msg::StdImage empty;
    auto seg2 = encode<dzIPC::Msg::StdImageFlat>(empty);
    auto v2 = dzIPC::Msg::StdImageView::bind(seg2.data(), total_size_of(seg2));
    ASSERT_TRUE(v2.valid());
    EXPECT_TRUE(v2.data().empty());
    EXPECT_TRUE(v2.encoding().empty());
    EXPECT_TRUE(v2.header().frame_id().empty());
    dzIPC::Msg::StdImage back;
    v2.copy_to(back);
    EXPECT_TRUE(back.data.empty());
    EXPECT_TRUE(back.encoding.empty());
}

/* --------------------------------------------------- ② schema 指纹与 wire 判别 */

TEST(DzFlat, SchemaHashesDifferAcrossTypes)
{
    /* 不同结构必须给出不同指纹, 否则定长布局会被错解成另一种消息。 */
    EXPECT_NE(dzIPC::Msg::StdImageFlat::kSchemaHash, dzIPC::Msg::StdPointCloudFlat::kSchemaHash);
    EXPECT_NE(dzIPC::Msg::StdImageFlat::kSchemaHash, dzIPC::Msg::StdHeaderFlat::kSchemaHash);
    EXPECT_NE(dzIPC::Msg::RobotStateFlat::kSchemaHash, dzIPC::Msg::PoseFlat::kSchemaHash);
    EXPECT_NE(dzIPC::Msg::StdVector3dFlat::kSchemaHash, dzIPC::Msg::StdHeaderFlat::kSchemaHash);
    EXPECT_NE(dzIPC::Msg::StdImageFlat::kSchemaHash, 0u);
}

TEST(DzFlat, BindRejectsWrongSchemaHash)
{
    dzIPC::Msg::StdImage m;
    m.encoding = "rgb8";
    m.data = {1, 2, 3, 4};
    auto seg = encode<dzIPC::Msg::StdImageFlat>(m);

    /* 用错类型的 View 去 bind 同一段 —— 指纹不符, 必须拒绝。 */
    auto wrong = dzIPC::Msg::StdPointCloudView::bind(seg.data(), total_size_of(seg));
    EXPECT_FALSE(wrong.valid());

    /* 就地篡改指纹, 正确类型也必须拒绝。 */
    auto* h = reinterpret_cast<dzflat::SegHeader*>(seg.data());
    h->schema_hash ^= 0x1u;
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(seg.data(), total_size_of(seg)).valid());
}

TEST(DzFlat, TlvWireIsNotMistakenForDzFlat)
{
    /* TLV 段首 4 字节是首字段名的长度(小整数), 与 magic 结构上不可能碰撞。
     * 这条判别式是双 wire 共存的地基(docs/dzflat_shm.md §3.7)。 */
    dzIPC::Msg::StdImage m;
    m.header.frame_id = "cam";
    m.encoding = "rgb8";
    m.data.assign(100, 7);
    ipc::buffer tlv = m.serialize();
    EXPECT_FALSE(dzflat::looks_like_dzflat(tlv.data(), tlv.size()));

    auto seg = encode<dzIPC::Msg::StdImageFlat>(m);
    EXPECT_TRUE(dzflat::looks_like_dzflat(seg.data(), seg.size()));
}

/* ------------------------------------------------- ③ 不信任段内容(边界与篡改) */

TEST(DzFlat, BindRejectsTruncatedOrGarbageSegment)
{
    dzIPC::Msg::StdImage m;
    m.encoding = "rgb8";
    m.data.assign(64, 3);
    const auto seg = encode<dzIPC::Msg::StdImageFlat>(m);
    const std::uint32_t n = total_size_of(seg);

    /* 声明长度大于实际可读长度 → 拒绝。 */
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(seg.data(), n - 1).valid());
    /* 比 SegHeader 还短 → 拒绝, 不得越界读。 */
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(seg.data(), 8).valid());
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(seg.data(), 0).valid());
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(nullptr, 1024).valid());

    /* 全零段(chunk 刚初始化的样子) → magic 不符, 拒绝。 */
    std::vector<std::uint8_t> zeros(256, 0);
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(zeros.data(), zeros.size()).valid());

    /* 随机垃圾 → 拒绝。 */
    std::vector<std::uint8_t> junk(256);
    for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = std::uint8_t(i * 37 + 11);
    EXPECT_FALSE(dzIPC::Msg::StdImageView::bind(junk.data(), junk.size()).valid());
}

TEST(DzFlat, OutOfRangeVarRefYieldsEmptyViewNotOverread)
{
    dzIPC::Msg::StdImage m;
    m.header.frame_id = "cam";
    m.encoding = "rgb8";
    m.data.assign(64, 9);
    auto seg = encode<dzIPC::Msg::StdImageFlat>(m);
    const std::uint32_t n = total_size_of(seg);

    auto* root = reinterpret_cast<dzIPC::Msg::StdImageRoot*>(seg.data()
                                                             + sizeof(dzflat::SegHeader));

    /* 偏移推到段外 */
    root->data.off = n + 4096;
    {
        auto v = dzIPC::Msg::StdImageView::bind(seg.data(), n);
        ASSERT_TRUE(v.valid());
        EXPECT_TRUE(v.data().empty()) << "越界偏移必须返回空视图";
    }
    /* 偏移合法但长度溢出段尾 */
    root->data.off = n - 8;
    root->data.cnt = 4096;
    {
        auto v = dzIPC::Msg::StdImageView::bind(seg.data(), n);
        ASSERT_TRUE(v.valid());
        EXPECT_TRUE(v.data().empty()) << "长度溢出必须返回空视图";
    }
    /* 字符串同理 */
    root->encoding.off = 0xFFFFFFF0u;
    root->encoding.cnt = 16;
    {
        auto v = dzIPC::Msg::StdImageView::bind(seg.data(), n);
        ASSERT_TRUE(v.valid());
        EXPECT_TRUE(v.encoding().empty());
    }

    /* copy_to 在被篡改的段上也必须安全收敛(空字段, 不崩不越界)。 */
    dzIPC::Msg::StdImage back;
    auto v = dzIPC::Msg::StdImageView::bind(seg.data(), n);
    ASSERT_TRUE(v.valid());
    v.copy_to(back);
    EXPECT_TRUE(back.data.empty());
    EXPECT_TRUE(back.encoding.empty());
}

TEST(DzFlat, OutOfRangeElementIndexIsSafe)
{
    dzIPC::Msg::RobotState m;
    m.name = "arm";
    m.pose_history.resize(2);
    m.pose_history[0].frame_id = "a";
    m.pose_history[1].frame_id = "b";
    auto seg = encode<dzIPC::Msg::RobotStateFlat>(m);
    auto v = dzIPC::Msg::RobotStateView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());

    ASSERT_EQ(v.pose_history_count(), 2u);
    /* 越界下标: 返回默认视图, 其访问器不得解引用空指针。 */
    auto oob = v.pose_history(99);
    EXPECT_FALSE(oob.valid());
    /* 篡改 cnt 使其超出段容量 → raw span 为空。 */
    auto* root = reinterpret_cast<dzIPC::Msg::RobotStateRoot*>(seg.data()
                                                              + sizeof(dzflat::SegHeader));
    root->pose_history.cnt = 1000000;
    auto v2 = dzIPC::Msg::RobotStateView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v2.valid());
    EXPECT_TRUE(v2.pose_history_raw().empty());
    EXPECT_FALSE(v2.pose_history(999999).valid());

    /* 计数本身也必须过边界校验: 若 _count() 直接返回段里的 cnt, copy_to 会拿着
     * 被篡改的 100 万去 resize —— 不受信输入驱动的无界分配。 */
    EXPECT_EQ(v2.pose_history_count(), 0u)
        << "计数未经边界校验, 篡改的 cnt 会泄漏到 resize";
    dzIPC::Msg::RobotState back;
    v2.copy_to(back);
    EXPECT_TRUE(back.pose_history.empty())
        << "copy_to 按未校验的 cnt 分配了 " << back.pose_history.size() << " 个元素";
}

TEST(DzFlat, TamperedStringArrayCountDoesNotDriveUnboundedAlloc)
{
    dzIPC::Msg::StdPointCloud m;
    m.header.frame_id = "lidar";
    m.channel_names = {"intensity", "ring"};
    m.points.resize(2);
    auto seg = encode<dzIPC::Msg::StdPointCloudFlat>(m);
    auto* root = reinterpret_cast<dzIPC::Msg::StdPointCloudRoot*>(seg.data()
                                                                 + sizeof(dzflat::SegHeader));
    root->channel_names.cnt = 5000000;
    auto v = dzIPC::Msg::StdPointCloudView::bind(seg.data(), total_size_of(seg));
    ASSERT_TRUE(v.valid());
    EXPECT_EQ(v.channel_names_count(), 0u);
    dzIPC::Msg::StdPointCloud back;
    v.copy_to(back);
    EXPECT_TRUE(back.channel_names.empty())
        << "copy_to 按未校验的 cnt 分配了 " << back.channel_names.size() << " 个字符串";
}

/* 变长区不得把 chunk 里的陈旧字节泄漏给订阅方: 用非零预填的缓冲写入, 然后确认
 * 段内所有"可达"字节都来自本次消息。这里用一个弱但有效的检查 —— 相同消息写进两块
 * 预填内容不同的缓冲, 得到的段必须逐字节相同。 */
TEST(DzFlat, SegmentBytesAreDeterministicRegardlessOfBufferGarbage)
{
    dzIPC::Msg::StdImage m;
    m.header.frame_id = "cam";
    m.header.stamp = 0.25;
    m.width = 3;
    m.height = 1;
    m.step = 9;
    m.encoding = "rgb8";
    m.data.assign(9, 5);

    const std::uint32_t cap = dzIPC::Msg::StdImageFlat::size(m);
    std::vector<std::uint8_t> a(cap, 0x00), b(cap, 0xEE);
    ASSERT_TRUE(dzIPC::Msg::StdImageFlat::write(m, a.data(), cap));
    ASSERT_TRUE(dzIPC::Msg::StdImageFlat::write(m, b.data(), cap));
    const std::uint32_t na = total_size_of(a);
    ASSERT_EQ(na, total_size_of(b));
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), na))
        << "段内出现了依赖缓冲原有内容的字节 —— chunk 复用时会把上一条消息泄漏出去";
}

TEST(DzFlat, WriteFailsWhenCapacityInsufficient)
{
    dzIPC::Msg::StdImage m;
    m.encoding = "rgb8";
    m.data.assign(4096, 1);
    const std::uint32_t cap = dzIPC::Msg::StdImageFlat::size(m);
    std::vector<std::uint8_t> seg(cap);

    /* 容量不足时必须返回 false 而不是越界写。 */
    EXPECT_FALSE(dzIPC::Msg::StdImageFlat::write(m, seg.data(), cap / 2));
    EXPECT_FALSE(dzIPC::Msg::StdImageFlat::write(m, seg.data(), 0));
    EXPECT_FALSE(dzIPC::Msg::StdImageFlat::write(m, nullptr, cap));
    EXPECT_TRUE(dzIPC::Msg::StdImageFlat::write(m, seg.data(), cap));
}
