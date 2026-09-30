/* [W07] ipc_info_pool 容量与共享布局契约（§6.1 有效规模 1000 路 / §10.5 版本协商）。
 *
 * 本文件只做**不安装陈旧段**的判据（纯函数 + 本进程容量），因此可以与
 * test_ipc_info_pool.cpp 并行跑; 真正"同段名 + 旧布局"的混合版本用例在
 * test_ipc_info_pool_version.cpp（那里必须独占段名, 见该文件说明）。
 *
 * 容量判据为什么不看线程数: 注册表是全机共享的**槽位**资源, 与接收线程数无关。
 * 基线 kMaxEntries=512 时第 513 路起注册失败并打限流诊断, 1000 路注册不满
 * （证据: test/perf/out/20260927_t6_fd/w1000_pub.log 的 register_entry 失败）。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dzIPC/ipc_info_pool.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::info_pool;

/* 按**已公开的段头偏移契约**(init_state@0 / magic@4 / version@8 / max_entries@12,
 * 由 ipc_info_pool.cc 的 offsetof static_assert 钉住)拼一个段头前缀镜像。 */
void put_header(std::vector<std::uint8_t>& buf, std::uint32_t magic, std::uint32_t version,
                std::uint32_t max_entries)
{
    std::uint32_t head[4] = {2u /* kInitReady */, magic, version, max_entries};
    std::memcpy(buf.data(), head, sizeof(head));
}

}   // namespace

/* ① 段头字段 + 段长三件事都要能被**拒绝**, 而不是"看一眼 magic 就按旧 size 用"。
 * 这是 D-4 约束 2「拒绝而非静默错解」的纯函数面证据。 */
TEST(IpcInfoPoolLayout, LayoutMismatchRejectsOldVersionOldCapacityAndShortSegment)
{
    const std::size_t bytes = segment_bytes();
    ASSERT_GT(bytes, 0u);

    std::vector<std::uint8_t> buf(bytes, 0);
    put_header(buf, kLayoutMagic, kLayoutVersion, static_cast<std::uint32_t>(kMaxEntries));
    EXPECT_EQ(layout_mismatch(buf.data(), bytes), nullptr) << "本版本布局应判为相容";

    put_header(buf, kLayoutMagic, 1u, static_cast<std::uint32_t>(kMaxEntries));
    EXPECT_NE(layout_mismatch(buf.data(), bytes), nullptr) << "旧 layout_ver 必须被拒";
    EXPECT_NE(std::string(layout_mismatch(buf.data(), bytes)).find("layout_ver"), std::string::npos);

    put_header(buf, kLayoutMagic, kLayoutVersion, 512u);
    EXPECT_NE(layout_mismatch(buf.data(), bytes), nullptr) << "旧 max_entries(512) 必须被拒";
    EXPECT_NE(std::string(layout_mismatch(buf.data(), bytes)).find("max_entries"), std::string::npos);

    put_header(buf, 0xDEADBEEFu, kLayoutVersion, static_cast<std::uint32_t>(kMaxEntries));
    EXPECT_NE(layout_mismatch(buf.data(), bytes), nullptr) << "magic 不符必须被拒";

    /* 段短于本版本 kRegionSize ⇒ 直接拒(⛔不得再按本版本长度遍历 entries)。 */
    put_header(buf, kLayoutMagic, kLayoutVersion, static_cast<std::uint32_t>(kMaxEntries));
    EXPECT_NE(layout_mismatch(buf.data(), bytes - 1), nullptr) << "段长不足必须被拒";
    EXPECT_NE(layout_mismatch(buf.data(), 8), nullptr) << "连段头都放不下必须被拒";
    EXPECT_NE(layout_mismatch(nullptr, bytes), nullptr) << "空指针必须被拒";
}

/* ② 容量表对账: 段字节数 = 段头 + kMaxEntries × PoolEntry, 且 1000 路后仍有明确余量。
 * 数字来自编译期布局(static_assert 钉住), 不是手抄。 */
TEST(IpcInfoPoolLayout, SegmentBytesMatchPinnedLayoutAndLeaveHeadroom)
{
    const std::size_t entries_bytes = kMaxEntries * kPoolEntryBytes;
    EXPECT_GE(segment_bytes(), entries_bytes) << "段至少装得下 entries 数组";
    EXPECT_LE(segment_bytes(), entries_bytes + 4096) << "段头不应吃掉超过一页";
    EXPECT_GE(kMaxEntries, 1000u) << "验收要求 ≥1000 路有效注册";
    EXPECT_EQ(sizeof(std::uint32_t), 4u);

    std::printf("[W07] kMaxEntries=%zu kPoolEntryBytes=%zu segment_bytes=%zu (%.2f KiB) headroom=%zu\n",
                kMaxEntries, kPoolEntryBytes, segment_bytes(),
                static_cast<double>(segment_bytes()) / 1024.0, kMaxEntries - 1000u);
}

/* ③ 1000 路有效注册 + 释放后可恢复。
 * 与 test_ipc_info_pool.cpp 的分工: 硬边界(填满整表 ⇒ 第 kMaxEntries+1 次返回 -1 +
 * 限流诊断 + 同锁回收)由那边的 UF-006 用例守; 这里只钉本工作包要的容量下限与可恢复性,
 * 避免两处同时填整表互相踩。 */
TEST(IpcInfoPoolLayout, CapacityCoversThousandLanesAndRecovers)
{
    std::vector<int32_t> slots;
    slots.reserve(1000);
    for (int i = 0; i < 1000; ++i)
    {
        const int32_t s = IpcInfoPool::instance().register_entry(
            {EntryKind::SocketSub, std::string("w07_cap_") + std::to_string(i), "", "", 0, ""});
        ASSERT_GE(s, 0) << "第 " << i << " 路注册失败 —— 容量不足";
        slots.push_back(s);
    }

    std::size_t mine = 0;
    for (const auto& e : IpcInfoPool::instance().snapshot(false))
    {
        if (e.pid == static_cast<int32_t>(::getpid()) && e.kind == EntryKind::SocketSub) ++mine;
    }
    EXPECT_EQ(mine, 1000u) << "注册返回成功但快照里看不到 —— 有效注册不成立";

    /* 恢复: 归还一个槽后必须能立即再注册(表满不是"永久拒绝")。 */
    IpcInfoPool::instance().unregister_entry(slots.back());
    slots.pop_back();
    const int32_t again = IpcInfoPool::instance().register_entry(
        {EntryKind::SocketSub, "w07_cap_recover", "", "", 0, ""});
    EXPECT_GE(again, 0) << "释放槽位后仍注册失败 —— 注册失败不可恢复";
    if (again >= 0) slots.push_back(again);

    for (const int32_t s : slots) IpcInfoPool::instance().unregister_entry(s);
}
