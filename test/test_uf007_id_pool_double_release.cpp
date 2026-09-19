/* UF-007 独立复现: "同一 storage_id 重复归还 → id_pool 空闲链表自环"
 *
 * ⛔ 本文件是**取证**用的, 不是产品修复。它回答两个必须分开的问题:
 *
 *   (A) **后果链是否真实** —— 对同一个 id 调两次 `release()`, 空闲链表真的会接成
 *       自环、并丢掉其后的全部 id 吗?
 *   (B) **触发条件今天是否可达** —— 套圈(lapped)读空这条路径, 还能不能让同一个
 *       `storage_id` 归还两次?
 *
 * 为什么必须分开: `docs/dzflat_known_issues.md` §4 与 §8 **互相矛盾** ——
 * §4 标题(`:184`)写【已修, 见 §8】, 而 §4 的"现状"段(`:198`)写"未修, 既存"。
 * 只跑端到端用例的话, "绿"既可能是"§8 的守卫挡住了", 也可能是"这条路径根本没被
 * 走到" —— 两种情况不可区分。故:
 *
 *   (A) 用**栈上的局部 `id_pool`** 复现后果: 不碰任何共享段 ⇒ 与 UF-003(崩溃残池)
 *       **零耦合**, 结构上不可能被残池效应污染;
 *   (B) 端到端走真实 chunk 池, 但**先立基线门**、**结束后重复三轮** ⇒ 红绿可解释
 *       (docs/unfixed_defects.md 0.3 第 4 条; 残池清理见 §6.3)。
 *
 * 与 UF-003 的隔离手段(三条, 缺一不可):
 *   ① 独立尺寸档(kPayload=12288 ⇒ 与 test_lap_safety 的 4096、test_chunk_hold 的
 *      4096/8192 都不同段), 别的用例的残池不会串进本用例;
 *   ② 基线门: 开跑前连发 kProbe 条大消息并全部读回 —— 池若已被前次崩溃啃过, 这
 *      些消息会退化成 12288/64=192 个分片/条, 256 槽的环保不住 ⇒ 收不满 kProbe;
 *   ③ 三轮重复: 一次性自环会被第 2/3 轮抓到, 不依赖"某一轮恰好命中"。
 *
 * 运行前提(否则基线门会红, 而那不是产品缺陷):
 *   ls /dev/shm | grep CHUNK_INFO          # 看有无残池段
 *   grep -l CHUNK_INFO /proc/<PID>/maps    # 有输出 ⇒ 有活进程在用, 不要删
 *                                          # (此处写 shell 通配 * 会提前闭合本注释: "*" + "/")
 *   ls /dev/shm/__IPC_SHM__*CHUNK_INFO__* | xargs -r rm -f
 */
#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/ipc.h"
#include "libipc/utility/id_pool.h"

namespace {

constexpr int kRingSlots = 256;  /* circ::elem_array::elem_max —— 环长, 与池容量无关 */

/* 池容量**只从模板语义取**, 不在本文件复查一个魔数:
 *     id_pool<>::max_count = min(large_msg_cache, uint8_t 上限)   (id_pool.h:39-47)
 * 写死魔数的害处不是"以后再改", 而是**判据失真**: 容量一变(改 large_msg_cache, 或
 * 换了尺寸档), 写死的 32 会让下面"其余 id 丢失"和"探针全读回"两条判据在错误的前提上
 * 照样给出结论。故本文件所有条数(取空上限、探针条数、自环探测次数)一律由 kCap 导出。
 */
constexpr std::size_t kCap = static_cast<std::size_t>(ipc::id_pool<>::max_count);
static_assert(kCap >= 2, "池容量 <2, \"重复归还丢其余 id\"这条判据构造不出来");

std::string uniq(const char* tag)
{
    static std::atomic<int> n{0};
    /* ⛔ 名字里不得出现 '/' —— 前导的也不行。
     * topic 名不是独立的名字, 它被**嵌进** shm 对象名的中段:
     *     make_prefix(prefix_, {"CC_CONN__", name_})        (ipc.cpp:165)
     *     => <prefix>"__IPC_SHM__""CC_CONN__"<name>"__"...
     * 平台层只做"缺前导 '/' 就补一个"这一种规范化(platform/posix/shm_posix.cpp:48-55),
     * **不净化内部斜杠**; 而 ::shm_open() 要的是 /单段名, 内部斜杠会被当成路径分隔符
     * ⇒ 它去找一级并不存在的子目录 ⇒ 段建不起来。此时用例连叫都叫不起来(route/recv
     * 全失败), 报出来的错与 UF-007 毫无关系 —— 纯假红。
     * 老写法 "/uf007/" + tag: 前导与内部各一个斜杠, 两处都踩。 */
    return std::string("uf007_") + tag + "_" + std::to_string(n.fetch_add(1));
}

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(n, fill);
}

bool all_bytes_are(const ipc::buff_t& b, std::size_t n, std::uint8_t fill)
{
    if (b.size() != n) return false;
    const auto* p = static_cast<const std::uint8_t*>(b.data());
    for (std::size_t i = 0; i < n; ++i)
    {
        if (p[i] != fill) return false;
    }
    return true;
}

/* 取空池: 返回按 acquire 顺序拿到的 id。
 * n_limit 由调用方按**真实容量**给出(kCap+1), 不再用 4096 这种与容量无关的魔数兜底:
 *   - 池健康 ⇒ 取够 kCap 个后 acquire() 返回负值, 循环提前结束;
 *   - 池已成自环 ⇒ acquire() 永不返回负值, 取到 kCap+1 停下, 越额由调用方判。
 * 两种情况下循环都必然终止, 且终止点只由容量决定。 */
std::vector<ipc::storage_id_t> drain_all(ipc::id_pool<>& pool, std::size_t n_limit)
{
    std::vector<ipc::storage_id_t> got;
    got.reserve(n_limit);
    for (std::size_t i = 0; i < n_limit; ++i)
    {
        const ipc::storage_id_t id = pool.acquire();
        if (id < 0) break;
        got.push_back(id);
    }
    return got;
}

std::size_t distinct_count(const std::vector<ipc::storage_id_t>& v)
{
    return std::set<ipc::storage_id_t>(v.begin(), v.end()).size();
}

}   // namespace

/* (A) 后果链是否真实 —— 局部 id_pool, 不碰共享段。
 *
 * 这条是**表征用例**(characterization): 它记录的是 `id_pool::release` 今天的语义
 * —— **不幂等**。它今天应当**通过**; 若哪天有人把 `release` 改成幂等的, 它会红,
 * 那时 §4/§8.4 关于"重复入池 ⇒ 自环"的整条后果链需要重写。
 *
 * 自环的精确机制(`src/libipc/utility/id_pool.h:83-88`, release 是**头插**):
 *     release(id): next_[id] = cursor_; cursor_ = id;
 * 第一次 release(X) 令 next_[X] = 旧链头; 第二次 release(X) 时 cursor_ 已经**等于
 * X**, 于是 next_[X] = X —— 链表指向自己。此后每次 acquire 都返回 X 且 `empty()`
 * 永不成立 ⇒ 池里**其余全部 id 永久不可达**。
 */
TEST(UF007IdPoolDoubleRelease, Consequence_DoubleReleaseSelfLoopsAndLosesAllOtherIds)
{
    constexpr std::size_t kLimit = kCap + 1;   /* 健康池取满 kCap 即止; 自环池越额停在 kCap+1 */

    /* ⛔ 必须**值初始化**(`pool{}`), 不能写 `ipc::id_pool<> pool;`。
     * `prepare()` 只在 `invalid()` 为真时才 `init()`(id_pool.h:56-59), 而 `invalid()`
     * 判的是"本对象逐字节等于全零"(与一个静态默认对象 memcmp, :65-68)。默认初始化只写
     * cursor_/prepared_ 这两个**带初值**的成员, next_ 是不确定值 ⇒ invalid() 为假
     * ⇒ init() 根本不执行 ⇒ 拿到一条垃圾空闲链, cursor_ 还可能指到 next_ 之外(越界读)。
     * 而真实碎片池的语义恰恰就是"全零"(段是内核新建、内容清零, 首个 prepare() 才建链),
     * 所以 `{}` 才是**与产品一致**的新池基线。 */
    ipc::id_pool<> pool{};
    pool.prepare();

    /* 前提 1(即基线本身): 新池是满的, kCap 个 id 互不相同 */
    const auto ids = drain_all(pool, kLimit);
    ASSERT_EQ(ids.size(), kCap)
        << "新池取不满(" << ids.size() << "/" << kCap << ") —— 基线不成立, 结论不可用";
    ASSERT_EQ(distinct_count(ids), kCap) << "初始就有重复 id —— 前提不成立";

    /* 对照(阴性): 每个 id **只归还一次** ⇒ 池必须完整恢复。
     * 没有这条对照, 下面"自环"的结论就分不清是重复归还造成的还是归还本身有毛病。 */
    for (auto id : ids) pool.release(id);
    const auto again = drain_all(pool, kLimit);
    ASSERT_EQ(again.size(), kCap)
        << "单次归还的池恢复不了(" << again.size() << "/" << kCap
        << ") —— 前提被破坏, 本用例的结论不可用";
    ASSERT_EQ(distinct_count(again), kCap);

    /* 恢复到满: 此时池是空的(刚被取空), 逐个归还即得满池 */
    for (auto id : again) pool.release(id);

    /* 实验: 对同一个 storage_id 归还**两次** */
    const ipc::storage_id_t victim = again.front();
    pool.release(victim);
    pool.release(victim);   /* ← 这就是 §4/§8.4 说的"同一 storage_id 重复入池" */

    constexpr int kTries = static_cast<int>(4 * kCap);   /* 由真实容量导出, 自环时也必然终止 */
    std::set<ipc::storage_id_t> reachable;
    int n = 0;
    for (int i = 0; i < kTries; ++i)
    {
        const ipc::storage_id_t id = pool.acquire();
        if (id < 0) break;
        ++n;
        reachable.insert(id);
    }

    EXPECT_EQ(n, kTries)
        << "重复归还后 acquire 竟然还能取空(取到 " << n << " 个就返回负值)—— "
           "自环没有形成, §4/§8.4 关于\"重复入池 ⇒ 自环\"的后果链需要重写";
    EXPECT_EQ(reachable.size(), std::size_t{1})
        << "重复归还后可达 id 不止一个(" << reachable.size() << " 个)—— 自环没形成";
    EXPECT_EQ(*reachable.begin(), victim)
        << "可达的那个 id 不是被重复归还的那个";
    /* 池里其余 kCap-1 个 id 此刻永久不可达 —— 这就是"丢掉其后的全部 id"。 */
    EXPECT_EQ(reachable.size(), std::size_t{1})
        << "其余 " << (kCap - reachable.size()) << " 个 id 已不可达";
}

/* (B) 触发条件今天是否可达 —— 端到端走真实 chunk 池。
 *
 * 套圈读空路径: 读方游标落后写指针超过环长(256)时, `cur` 与 `cur+256` 映射到同一
 * 环槽位。旧实现据此把同一格内容投递两次 → 大消息产生两个 `buff_t` →
 * `recycle_storage` 走两次 → 同一 `storage_id` 两次入池。
 *
 * 判据(两段, 都不依赖任何池容量的先验知识):
 *   ① 池缩水 —— 读空之后, 连发 kProbe 条大消息必须仍能全部读到(池健康 ⇒ 1 槽/条);
 *      缩水后大消息退化成 kPayload/64 个槽位/条 ⇒ 环装不下 ⇒ 收不满。
 *      **重复三轮**: 一次性自环会被第 2/3 轮抓到。
 *   ② 共块 —— 池一旦自环, 每次 acquire 都返回同一个 id ⇒ 所有大消息共用一块 chunk
 *      互相踩踏。故连发两条**内容不同**的大消息, 先发的那条必须仍能原样读到。
 *      (这一条不依赖容量估计, 是自环最直接的观测。)
 */
TEST(UF007LappedDrain, LappedDrainMustNotShrinkChunkPoolNorShareOneChunk)
{
    constexpr std::size_t kPayload = 12288;   /* 独立尺寸档, 不与其他用例抢池子 */
    /* 探针条数: 必须 < 池容量, 否则"全部读回"这条基线门本身就装不下。
     * 由真实容量导出(而不是写死 20 再祈望容量够): kCap < 24 时自动降到 kCap-1。 */
    constexpr int kProbe = (kCap >= 24u) ? 20 : static_cast<int>(kCap) - 1;
    static_assert(kProbe >= 1, "池容量过小, 基线门构造不出来");
    static_assert(static_cast<std::size_t>(kProbe) < kCap, "探针条数须小于池容量");

    const std::string name = uniq("lap_drain");
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

    const auto payload = make_payload(kPayload, 0xC3);

    /* 探针: 连发 kProbe 条大消息(不读旧的), 再读空。返回读到的条数。 */
    auto probe = [&]() {
        for (int i = 0; i < kProbe; ++i)
        {
            if (!tx.send(payload.data(), payload.size(), 0)) return -1;
        }
        int n = 0;
        for (;;)
        {
            ipc::buff_t b = rx.recv(60);
            if (b.empty()) break;
            ++n;
        }
        return n;
    };

    /* 基线门: 池必须是满的, 否则本用例的红绿不可解释(那是 UF-003 的效应)。 */
    const int baseline = probe();
    ASSERT_EQ(baseline, kProbe)
        << "chunk 池非初始状态(基线 " << baseline << "/" << kProbe
        << ")。先按 docs/unfixed_defects.md §6.3 清残池(UF-003 的效应), 再重跑 —— "
           "本用例判的是 UF-007, 不是残池";

    /* 把接收方套圈: 连发远超环长, 全程不读 */
    for (int i = 0; i < kRingSlots * 3; ++i)
    {
        ASSERT_TRUE(tx.send(payload.data(), payload.size(), 0)) << "第 " << i << " 条发送失败";
    }

    /* 读空 —— 旧实现在这一步让同一 storage_id 二次入池 */
    for (;;)
    {
        ipc::buff_t b = rx.recv(60);
        if (b.empty()) break;
    }

    /* ① 池不得缩水(重复三轮) */
    for (int round = 0; round < 3; ++round)
    {
        const int after = probe();
        EXPECT_EQ(after, kProbe)
            << "第 " << round + 1 << " 轮: 套圈读空后 chunk 池缩水(" << after << "/" << kProbe
            << ") —— 同一 storage_id 被重复入池, id_pool 空闲链表接成自环, 其余 id 全部丢失";
        if (after != kProbe) break;
    }

    /* ② 共块检测: 两条不同内容的大消息必须都能原样读到 */
    const auto pat_a = make_payload(kPayload, 0xA5);
    const auto pat_b = make_payload(kPayload, 0x5B);
    ASSERT_TRUE(tx.send(pat_a.data(), pat_a.size(), 0));
    ASSERT_TRUE(tx.send(pat_b.data(), pat_b.size(), 0));

    int got_a = 0, got_b = 0;
    for (;;)
    {
        ipc::buff_t m = rx.recv(60);
        if (m.empty()) break;
        if (all_bytes_are(m, kPayload, 0xA5)) ++got_a;
        else if (all_bytes_are(m, kPayload, 0x5B)) ++got_b;
    }
    EXPECT_GE(got_a, 1)
        << "先发的 0xA5 那条读不到了(got_a=0, got_b=" << got_b
        << ") —— 后一条 acquire 到了同一个 chunk id, 把前一条的内容覆写了";
    EXPECT_GE(got_b, 1) << "后发的 0x5B 那条没读到";
}
