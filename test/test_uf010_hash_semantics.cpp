/* UF-010 聚焦测试: `ipc::hash<ipc::string>` / `hash<ipc::wstring>` 的哈希语义必须与
 * 容器的相等性判据(`std::equal_to<Key>`, 按内容)一致。
 *
 * 缺陷基线(docs/uf010_evidence_registration.md):
 *   resource.h 的 `hash<string>` 特化是 `std::hash<char const *>{}(val.c_str())`
 *   —— **按指针**哈希; 而 `ipc::unordered_map` 的相等性按**内容**。
 *   两个内容相同、地址不同的 `ipc::string` 因此哈希不同 ⇒ 落进不同桶 ⇒ 永远不命中
 *   已有条目 ⇒ 同一 shm 段被反复 mmap(loan/recv/~buffer 三处各建一个条目),
 *   多接收者零拷贝广播拿到**不同虚拟地址**(同一物理内存)。
 *
 * 本文件是 UF-010 的 focused 判据:
 *   - 缺陷未修时: 全部用例**红**(见 docs/uf010_evidence_registration.md 的基线);
 *   - 修复(哈希改为按内容)后: 全部用例**绿**。
 * 判据不绑定具体哈希算法, 只要求「同内容 ⇒ 同哈希、不同内容 ⇒ 不同哈希(抽样)、
 * 容器按内容去重」这三条与 equal_to 自洽的语义。
 *
 * 与既有用例的关系: 端到端那一侧由 test/test_loan.cpp 的
 * `Loan.BroadcastToMultipleReceivers`(g1.data() == g2.data()) 承担, 本文件只补
 * 「语义层」与「同一段映射次数」两个更聚焦的观测点。
 */
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

#include "libipc/ipc.h"
#include "libipc/memory/resource.h"

namespace {

/* 统计本进程 /proc/self/maps 中映射到的 shm 段行数(每行 = 一次独立映射)。 */
std::size_t mapped_lines_of(std::string const &segment_name)
{
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    std::size_t n = 0;
    while (std::getline(maps, line))
    {
        if (line.find(segment_name) != std::string::npos) ++n;
    }
    return n;
}

std::size_t mapped_chunk_info_lines()
{
    return mapped_lines_of("__IPC_SHM__CHUNK_INFO__");
}

}   // namespace

/* 内容相同 ⇒ 哈希必须相同(与 equal_to 一致)。短串走 SSO、长串走堆, 两条都测。 */
TEST(Uf010HashSemantics, ContentEqualStringsHashEqual)
{
    ipc::string a{""}, b{""};
    ipc::string c{"loan_broadcast"}, d{"loan_broadcast"};
    ipc::string e(64, 'x');
    ipc::string f(64, 'x');

    EXPECT_NE(a.c_str(), b.c_str()) << "两个独立对象不得共享同一缓冲";
    EXPECT_NE(e.data(), f.data());

    EXPECT_EQ(ipc::hash<ipc::string>{}(a), ipc::hash<ipc::string>{}(b))
        << "内容相同的空串哈希必须相同(缺陷: 按 c_str() 指针哈希)";
    EXPECT_EQ(ipc::hash<ipc::string>{}(c), ipc::hash<ipc::string>{}(d))
        << "内容相同的长串哈希必须相同";
    EXPECT_EQ(ipc::hash<ipc::string>{}(e), ipc::hash<ipc::string>{}(f))
        << "内容相同的 64B 串(堆缓冲)哈希必须相同";
}

/* 反向保险: 哈希不得退化成常量。不同内容抽样必须散列到不同值。 */
TEST(Uf010HashSemantics, DifferentContentHashesDiffer)
{
    ipc::string a{""};
    ipc::string b{"a"};
    ipc::string c{"b"};
    ipc::string d{"loan_broadcast"};

    EXPECT_NE(ipc::hash<ipc::string>{}(a), ipc::hash<ipc::string>{}(b));
    EXPECT_NE(ipc::hash<ipc::string>{}(b), ipc::hash<ipc::string>{}(c));
    EXPECT_NE(ipc::hash<ipc::string>{}(c), ipc::hash<ipc::string>{}(d));
}

/* 容器语义: 内容相同的 key 必须命中同一条目(而不是各插一条)。 */
TEST(Uf010HashSemantics, UnorderedMapDedupsByContent)
{
    ipc::unordered_map<ipc::string, int> m;

    ipc::string a{""}, b{""};
    m[a] = 1;
    m[b] = 2;   /* b 与 a 内容相同 —— 期望命中同一条目 */
    EXPECT_EQ(std::size_t{1}, m.size()) << "内容相同的 key 不得重复插入";
    EXPECT_EQ(2, m.at(a));
    EXPECT_EQ(2, m.at(b));

    ipc::string c{"loan_broadcast"}, d{"loan_broadcast"};
    ipc::unordered_map<ipc::string, int> m2;
    m2[c] = 1;
    m2[d] = 2;
    EXPECT_EQ(std::size_t{1}, m2.size());
    EXPECT_EQ(std::size_t{1}, m2.count(ipc::string{"loan_broadcast"}));
}

/* 同一个坑的第二处: hash<wstring> 是同样的指针哈希特化。 */
TEST(Uf010HashSemantics, WstringSharesTheSameSemantics)
{
    ipc::wstring a{L""}, b{L""};
    ipc::wstring c{L"loan_broadcast"}, d{L"loan_broadcast"};

    EXPECT_EQ(ipc::hash<ipc::wstring>{}(a), ipc::hash<ipc::wstring>{}(b));
    EXPECT_EQ(ipc::hash<ipc::wstring>{}(c), ipc::hash<ipc::wstring>{}(d));

    ipc::unordered_map<ipc::wstring, int> m;
    m[a] = 1;
    m[b] = 2;
    EXPECT_EQ(std::size_t{1}, m.size()) << "wstring 键必须按内容去重";
}

/* 资源映射行为: 同一 chunk 档位段, 在 loan → recv 路径上只应被映射一次。
 * 缺陷未修时 loan 与 recv 各建一个条目 ⇒ 计数为 2(之后 ~buffer 再建第三个)。
 * 这是「同一物理内存被重复映射」的最小机械观测点。 */
TEST(Uf010MappingDedup, SameChunkSegmentIsMappedOnce)
{
    const std::string name = "uf010_mapcount";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    const std::size_t before = mapped_chunk_info_lines();

    auto lo = tx.loan(4096);
    ASSERT_TRUE(lo.valid());
    std::memset(lo.data, 0x5A, lo.size);
    ASSERT_TRUE(tx.publish_loan(lo, 1000));

    ipc::buff_t g = rx.recv(2000);
    ASSERT_FALSE(g.empty());
    const std::size_t after_recv = mapped_chunk_info_lines();

    EXPECT_EQ(before + 1, after_recv)
        << "loan/recv 之后该 chunk 段应恰好新增 1 个映射, 实测 " << after_recv - before
        << " 个(缺陷: 每个 prefix 对象建一个条目、各 mmap 一次)";
}
