/* 反序列化的**分配**必须有界 (docs/dzflat_known_issues.md 第 3 条的另一半)
 *
 * 第 3 条修的是"越界读 → SIGSEGV", 手法是 adapt_memcpy_tods 逐段校验、越界即止。但生成的
 * 反序列化长这样:
 *
 *     int32_t n;                       // 未初始化
 *     adapt_memcpy_tods(&n, ...);      // 越界 → 一个字节都不拷
 *     vec.resize(n);                   // ← 分配, 发生在上面那道检查**之后**却在拷贝之前
 *
 * 于是有两条独立的路径能绕过那道防线, 而且都不是读越界而是**无界分配**:
 *   ① count 读失败 → n 保持未初始化的栈垃圾 → resize(垃圾);
 *   ② count 读成功但值荒谬(缓冲被覆写/篡改) → resize(几亿) 直接提交 GB 级内存。
 * 两者的结局都是 bad_alloc / length_error, 而这条路径上无人捕获 —— 进程 abort。实测在
 * ipc_benchmark 的 262144B 档偶发命中过(exit=134)。
 *
 * 本文件把两条都钉成**确定性**用例: 间歇性崩溃不能靠"跑通一次"来验收。
 */
#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

#include "ipc_msg/test_msg2/test_msg.hpp"

namespace {

constexpr uint32_t kMsgId = 9;

dzIPC::Msg::TestMsg sample()
{
    dzIPC::Msg::TestMsg m;
    m.data1.assign(4, 1.0);
    m.data2.assign(4, 7);
    m.data3.push_back("abc");
    m.data4 = true;
    m.set_msg_id(kMsgId);
    return m;
}

std::vector<uint8_t> serialized(const dzIPC::Msg::TestMsg& m)
{
    auto ser = const_cast<dzIPC::Msg::TestMsg&>(m).serialize();
    const auto* p = static_cast<const uint8_t*>(ser.data());
    return std::vector<uint8_t>(p, p + ser.size());
}

ipc::buffer wrap(std::vector<uint8_t>& v)
{
    return ipc::buffer(v.data(), v.size(), [](void*, std::size_t) {});
}

/* data1 的 count 字段在 wire 上的偏移: [int32 name_size][name][uint8 type][int32 count] */
std::size_t data1_count_offset(const std::vector<uint8_t>& buf)
{
    int32_t name_size = 0;
    std::memcpy(&name_size, buf.data(), sizeof(name_size));
    return sizeof(int32_t) + static_cast<std::size_t>(name_size) + sizeof(uint8_t);
}

}   // namespace

/* ② 读得到但值荒谬 —— 不得据此分配 */
TEST(DeserAllocGuard, AbsurdCountMustNotAllocate)
{
    auto buf = serialized(sample());
    const std::size_t at = data1_count_offset(buf);
    const int32_t huge = 0x2000'0000;   /* 5.37e8 个 double = 4.3GB */
    std::memcpy(buf.data() + at, &huge, sizeof(huge));

    auto raw = wrap(buf);
    dzIPC::Msg::TestMsg out;
    out.set_msg_id(kMsgId);
    out.deserialize(raw);

    EXPECT_FALSE(out.deserialize_ok()) << "被篡改的 count 必须置位, 让调用方丢整条消息";
    EXPECT_EQ(out.data1.size(), 0u)
        << "按 wire 上的 count 分配了 " << out.data1.size()
        << " 个元素 —— count 未经校验就进了 resize。修复前实测 536870912(4.3GB), 内存紧张"
           "时抛 bad_alloc 而无人捕获 → 进程 abort";
}

/* 负数 count: 转成 size_t 是天文数字, 同样不得分配 */
TEST(DeserAllocGuard, NegativeCountMustNotAllocate)
{
    auto buf = serialized(sample());
    const std::size_t at = data1_count_offset(buf);
    const int32_t neg = -1;
    std::memcpy(buf.data() + at, &neg, sizeof(neg));

    auto raw = wrap(buf);
    dzIPC::Msg::TestMsg out;
    out.set_msg_id(kMsgId);
    out.deserialize(raw);

    EXPECT_FALSE(out.deserialize_ok());
    EXPECT_EQ(out.data1.size(), 0u) << "负 count 转 size_t 后是 2^64-1 量级";
}

/* ① count 读失败时目标必须被清零, 不能留栈垃圾 */
TEST(DeserAllocGuard, FailedCountReadYieldsZeroNotGarbage)
{
    /* 截断到 count 字段**中间**, 使读那 4 个字节必然落在缓冲之外。
     *
     * 注意不能像别处那样再把 12 字节页尾接回去: 补上之后 at+4 反而重新落进缓冲范围,
     * 读取会**成功**并读到页尾字节 —— 那测到的就是"值荒谬"那条路(上面两例已覆盖),
     * 而不是这里要测的"读失败"。本用例直接调 deserialize、不过 check_id, 所以页尾对它
     * 没有用处。 */
    auto full = serialized(sample());
    const std::size_t at = data1_count_offset(full);
    ASSERT_GT(full.size(), at + 4);

    std::vector<uint8_t> bad(full.begin(), full.begin() + static_cast<long>(at) + 2);

    auto raw = wrap(bad);
    dzIPC::Msg::TestMsg out;
    out.set_msg_id(kMsgId);
    /* 先塞进非零内容, 确认"清零"不是靠对象本来就是空的。 */
    out.data1.assign(3, 42.0);
    out.deserialize(raw);

    EXPECT_FALSE(out.deserialize_ok()) << "越界读必须置位";
    EXPECT_LT(out.data1.size(), 1000u)
        << "count 读失败后仍分配了 " << out.data1.size()
        << " 个元素 —— 用的是未初始化的栈垃圾(adapt_memcpy_tods 越界时不拷任何字节, "
           "所以变量保持未初始化)";
}

/* 直接测 adapt_memcpy_tods 的清零, 而不是透过生成的 deserialize。
 *
 * 为什么要单独测: 上面第三条其实测不到清零 —— count 闸跑在它后面, 把未初始化的垃圾值
 * 也一并拦下了(实测把清零删掉, 那三条仍全绿)。于是清零成了**无测试覆盖的防线**, 而这
 * 类防线会随重构悄悄失效。
 *
 * 清零的射程与 count 闸不同, 这是它存在的理由: count 闸由 generator 发射, 只存在于重新
 * 生成过的头文件里; 清零在 IpcMsgBase 里, 手写的、以及尚未重新生成的 deserialize 也受
 * 它保护。 */
class TodsProbe : public IpcMsgBase
{
public:
    /* 摆出一个"缓冲只有 n 字节"的现场, 然后读越界处 —— 与 deserialize 第一句同构。 */
    void read_out_of_range(std::uint8_t* dst, std::uint32_t len, std::uint32_t buffer_size)
    {
        deserialize_data_cut(buffer_size);          /* _total_size = 缓冲长度 */
        std::vector<std::uint8_t> src(buffer_size, 0xAB);
        std::uint32_t offset = buffer_size;          /* 起点就在缓冲末尾之外 */
        adapt_memcpy_tods(dst, src.data(), offset, len);
    }
};

TEST(DeserAllocGuard, OverflowZeroesDestination)
{
    std::uint32_t sentinel = 0xFFFF'FFFFu;   /* 若不清零, 这个值会被原样留下 */
    TodsProbe probe;
    probe.read_out_of_range(reinterpret_cast<std::uint8_t*>(&sentinel), sizeof(sentinel), 8);

    EXPECT_FALSE(probe.deserialize_ok()) << "越界必须置位";
    EXPECT_EQ(sentinel, 0u)
        << "越界后目标没被清零(仍是 0x" << std::hex << sentinel << std::dec
        << ") —— 生成的 deserialize 会拿这个未初始化值去 resize";
}

/* 未被篡改的同一条必须仍然解得开 —— 证明上面三条改的是判据而不是把功能改坏了 */
TEST(DeserAllocGuard, CleanBufferStillDeserializes)
{
    auto buf = serialized(sample());
    auto raw = wrap(buf);
    dzIPC::Msg::TestMsg out;
    out.set_msg_id(kMsgId);
    out.deserialize(raw);

    EXPECT_TRUE(out.deserialize_ok());
    EXPECT_EQ(out.data1.size(), 4u);
    EXPECT_EQ(out.data2.size(), 4u);
    ASSERT_EQ(out.data3.size(), 1u);
    EXPECT_EQ(out.data3[0], "abc");
    EXPECT_TRUE(out.data4);
}
