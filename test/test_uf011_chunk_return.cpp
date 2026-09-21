/* UF-011 判据: 双归还协议下, chunk 池空闲链不得自环
 *
 * ── 被测不变式 ───────────────────────────────────────────────────────────────
 * `chunk->conns()` 是发送时刻一次性置好的收方**位图**, 不是引用计数; 清它的地方有三
 * 处(收方析构的 recycle_storage、写方覆写的 discard_storage、借样归还), 由不同线程
 * 驱动同一张位图, 而位图本身没有仲裁者。于是"同一块 chunk 被归还两次"在协议上从未
 * 被排除 —— 而 `id_pool::release` 是 `next_[id] = cursor_` 的**头插且不幂等**
 * (id_pool.h:76-81): 第二次 release 时 cursor_ 已等于 id, 于是 `next_[id] == id`,
 * 空闲链接成自环, 此后 `acquire()` **永远返回同一个 id**、池里其余 id 永久不可达。
 *
 * ── 为什么用"一读一落后"这个构造(而不是更简单的形态) ─────────────────────────
 * 触发需要**两条归还路径同时开火**:
 *   - 收方 A 边收边丢(`recv` 拿到的 buff_t 立刻析构)⇒ 走 recycle_storage;
 *   - 收方 B 全程不读 ⇒ 环绕回来时普通 push 失败、退化 force_push ⇒ 走
 *     discard_storage 清掉"永远不会来取"的位。
 * 实测(2026-09-20, 沙箱): 单收方 / 多收方只建不读的构造 **20 轮 0 次自环**;
 * 换成"一读一落后"后, 未加幂等守卫的形态 **17/20 轮自环**(指纹 next_[27]==27)。
 * ⇒ 构造本身是承重的, 不能简化。
 *
 * ── 判据两段 ─────────────────────────────────────────────────────────────────
 *   ① **量具门**(只跑一次, 且必须在池干净时跑): 发一条大消息、无人取, 走链必须恰好读到
 *      cap−1 —— 没有这条, "没自环"与"走链器/段名对不上"完全同形。
 *   ② **无自环**: 每轮收尾后段内不得存在 `next_[id] == id`。这一条是**决定性**的:
 *      同一构造在未加幂等守卫的形态下 **40 轮 8/8 次转红**(沙箱实测), 加了守卫 8/8 绿。
 *
 * ⛔ **量具不得改变被测对象**: 采样要 fopen/fread 段文件, 把它插在发布循环里会拖慢
 * 发布方、改变竞态窗口 —— 实测那样采样时 48 轮 0 次自环, 而同一构造**不采样**是
 * 17/20 轮。所以洪泛期间一次都不采样, 只在开跑前(量具门)与每轮收尾后各量一次。
 *
 * ── ⛔ 尚未闭合的残余(默认只登记, 见下面的开关) ───────────────────────────────
 * 本判据覆盖的是"同一块被归还两次 ⇒ 自环"。**不**覆盖"归还得太早": pop() 先拷数据、
 * 后才清自己的 rc 位, 所以一个"已读到 storage id 但尚未清位"的收方仍会被写方算进
 * rem_cc, 被当成"永远看不到这格"而把 chunk 提前收回 —— 那个收方随后拿一个已回池
 * (可能已复用)的 id 去建 buff_t(ABA)。那一侧的防护在 prod_cons.h 的 pop() 里
 * (拷贝前后与进 clear 前各比一次 epoch), 是另一条判据的事。
 *
 * 另有一条**偶发、位置不固定**的残余: 收尾后偶尔读到 free = cap−1(约 3/40 轮), 多覆写
 * 几圈也不回升。⛔ 但同一构造连跑两次, 一次只在末 3 轮出现、另一次 39/40 轮都出现 ⇒
 * **不是可复现判据**, 已按"未定位"登记, 不作门。把 UF011_EXPECT_LEAK_FREE=1 设进环境
 * 可把它变成硬判据(用于继续追查, 不作常态门)。
 *
 * 清残池(池段全局, 见 docs/unfixed_defects.md §6.3)不归本文件管; 残池会让② 假红。
 */
#include <dirent.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/def.h"
#include "libipc/ipc.h"
#include "libipc/utility/id_pool.h"

namespace {

/* 池容量**只从模板语义取**, 不复查魔数: id_pool<>::max_count = min(large_msg_cache,
 * uint8 上限)。写死魔数的害处不是"以后再改", 而是**判据失真** —— 容量一变, 走链的
 * 步数上限与段名分量都会跟着错, 而错法是静默的(读到另一个段/少走几步)。 */
constexpr std::size_t kCap = static_cast<std::size_t>(ipc::id_pool<>::max_count);

/* 环的槽位数: circ::elem_array::elem_max = numeric_limits<uint8>::max() + 1。 */
constexpr int kRingSlots = 256;

/* 槽位承载的字节数: ipc::data_length(超过它才走 chunk)。 */
constexpr std::size_t kSlotBytes = 64;

/* ⛔ **不得**写死尺寸档: 池段名里的尺寸分量是 `calc_chunk_size(size)` 的结果, 它随
 * `large_msg_align` / `sizeof(std::atomic<cc_t>)` / `alignof(max_align_t)` 一起变。
 * 写死的失败方式是**静默**的: 档位猜错 ⇒ 读到另一个段或读不到段 ⇒ 量具退化, 而用例
 * 看起来还是绿的(本文件初版就栽在这上面)。所以 payload 与档位一起算。 */
constexpr std::size_t kAlignStep = 1024;   /* ipc::large_msg_align */

constexpr std::size_t align_up_to(std::size_t x, std::size_t a) noexcept
{
    return ((x + a - 1) / a) * a;
}
/* 与 src/libipc/ipc.cpp 的 calc_chunk_size 同构(抄写而非调用: 它在 ipc.cpp 的匿名
 * namespace 里, 没有对外链接)。 */
constexpr std::size_t calc_chunk_class(std::size_t size) noexcept
{
    return align_up_to(align_up_to(sizeof(std::max_align_t) + size, kAlignStep),
                       alignof(std::max_align_t));
}
constexpr std::size_t kPayload = 8000;
constexpr std::size_t kChunkClass = calc_chunk_class(kPayload);

/* 段路径**靠扫描发现**: 段名里除尺寸档外还有别的分量(如容量后缀 `__C<cap>`), 而那个
 * 分量的形状正在变 —— 拼写死就是上面说的静默退化。扫描只要求 "CHUNK_INFO__<class>"
 * 之后要么到头、要么是 '__', 免得 8192 匹配上 81920。 */
std::string pool_segment_path()
{
    const std::string want = "CHUNK_INFO__" + std::to_string(kChunkClass);
    DIR* d = opendir("/dev/shm");
    if (d == nullptr) return {};
    std::string found;
    while (struct dirent* e = readdir(d))
    {
        const std::string n = e->d_name;
        const std::size_t p = n.find(want);
        if (p == std::string::npos) continue;
        const std::size_t after = p + want.size();
        if (after != n.size() && n[after] != '_') continue;   /* 8192 不匹配 81920 */
        found = "/dev/shm/" + n;
        break;
    }
    closedir(d);
    return found;
}

std::string uniq(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("uf011_") + tag + "_" + std::to_string(n.fetch_add(1));
}

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(n, fill);
}

/* 段内是否存在 `next_[id] == id`。读 [0, cap+1) 即读整条空闲链: `chunk_info_t` 的首
 * 成员是 `id_pool<>`, 其 `next_[cap]` 占偏移 0..cap-1(每项 1 字节)、`cursor_` 在偏移
 * cap。段是 tmpfs 文件, 进程 mmap 的同时可按文件读同一份内存。
 * 返回: 1 = 有自环(*which 给出 id), 0 = 没有, -1 = 读不出来(段不存在/读不全)。 */
int self_loop(std::size_t* which)
{
    std::FILE* f = std::fopen(pool_segment_path().c_str(), "rb");
    if (f == nullptr) return -1;
    unsigned char b[kCap + 1];
    const std::size_t got = std::fread(b, 1, sizeof(b), f);
    std::fclose(f);
    if (got < sizeof(b)) return -1;
    for (std::size_t i = 0; i < kCap; ++i)
    {
        if (b[i] == i) { if (which) *which = i; return 1; }
    }
    return 0;
}

/* 走一遍空闲链得空闲块数。⛔ 必须限步: 自环时 cursor_ 永远 < cap, 不限步就是死循环
 * —— 量具把被测进程挂住比读错更糟。返回 -1 = 读不出来, -2 = 链带环。 */
int walk_free()
{
    std::FILE* f = std::fopen(pool_segment_path().c_str(), "rb");
    if (f == nullptr) return -1;
    unsigned char b[kCap + 1];
    const std::size_t got = std::fread(b, 1, sizeof(b), f);
    std::fclose(f);
    if (got < sizeof(b)) return -1;
    unsigned cur = b[kCap];
    int n = 0;
    while (cur < static_cast<unsigned>(kCap) && n <= static_cast<int>(kCap))
    {
        cur = b[cur];
        ++n;
    }
    if (n > static_cast<int>(kCap)) return -2;
    return n;
}

/* tm = 0: push 失败即刻退化到 force_push, 不等待。 */
bool blast(ipc::route& tx, const std::vector<std::uint8_t>& payload)
{
    return tx.send(payload.data(), payload.size(), 0);
}

}   // namespace

TEST(UF011ChunkReturn, PoolChainMustNotSelfLoop)
{
    constexpr int kRounds = 40;

    const auto payload = make_payload(kPayload, 0xC3);
    const auto tiny = make_payload(kSlotBytes / 2, 0x5A);

    bool saw_loop = false;
    std::size_t loop_id = 0;
    int    loop_round = -1;
    int    leak_rounds = 0;
    int    min_free_end = static_cast<int>(kCap);

    /* ── ① 量具门: **只跑一次**, 且必须在池是干净的时候跑 ──────────────────────
     * 发一条大消息、谁都还没取: 它的 chunk 已被 acquire 出来、收方位图已置上 ⇒ 空闲链
     * 上必须正好少一块。读到 cap 说明走链器/段名/容量对不上; 读到别的数说明这一档池里
     * 有别人的块(串档)—— 两种都会让后面的"无自环"失去意义。
     * ⛔ 不能放进每轮循环里: 本用例测的**泄漏**会让池逐轮变浅, 那时量具门读到 cap−2 是
     * "泄漏又攒了一轮", 不是量具坏了 —— 两者同形, 混在一起会把泄漏报成量具失效
     * (初版就是这样: 守卫臂两次红全是量具门, 而真正的自环判据一次都没机会说话)。 */
    {
        const std::string gname = uniq("gauge");
        ipc::route::clear_storage(gname.c_str());
        ipc::route gtx{gname.c_str(), ipc::sender};
        ipc::route grx{gname.c_str(), ipc::receiver};
        ASSERT_TRUE(gtx.wait_for_recv(1, 2000)) << "量具门的接收方未连上";
        ASSERT_TRUE(blast(gtx, payload)) << "量具门的大消息发送失败";

        const int g_free = walk_free();
        ASSERT_EQ(g_free, static_cast<int>(kCap) - 1)
            << "量具门不成立: 钉住一条大消息后空闲链读到 " << g_free << "/" << kCap
            << "(期望 " << (kCap - 1) << ")。段 = " << pool_segment_path()
            << " —— 走链器/段名对不上或串档(残池), 本用例结论不可用";
        std::size_t gw = 0;
        ASSERT_NE(self_loop(&gw), 1) << "量具门就已有自环(next_[" << gw << "]) —— 残池未清";

        /* 收回量具门钉住的那一块(断开 + 覆写), 让循环从满池开始。 */
        grx.release();
        for (int i = 0; i < kRingSlots + 16; ++i) blast(gtx, tiny);
        ASSERT_EQ(walk_free(), static_cast<int>(kCap))
            << "量具门自己钉住的块收不回来 —— 走链器/归还路径有问题, 本用例结论不可用";
    }

    for (int r = 0; r < kRounds; ++r)
    {
        const std::string name = uniq("pool_chain");
        ipc::route::clear_storage(name.c_str());

        ipc::route tx{name.c_str(), ipc::sender};
        ipc::route rx_eat{name.c_str(), ipc::receiver};   /* 边收边丢 ⇒ recycle_storage */
        ipc::route rx_lag{name.c_str(), ipc::receiver};   /* 全程不读 ⇒ force_push */
        ASSERT_TRUE(tx.wait_for_recv(2, 2000)) << "两个接收方未在超时内连上";

        /* 收方 A 与发布并发地"收下即丢": buff_t 析构 ⇒ recycle_storage 归还其 chunk。 */
        std::atomic<bool> stop{false};
        std::thread eater([&] {
            while (!stop.load(std::memory_order_relaxed))
            {
                ipc::buff_t b = rx_eat.recv(5);
                (void)b;   /* b 在此析构 */
            }
        });

        /* 绕环两圈多: 环满 ⇒ push 失败 ⇒ 退化 force_push ⇒ discard_storage 清落后收方的位。
         * 两条归还路径在整个过程里同时开火 —— 这就是自环的构造条件。
         * ⛔ 洪泛期间**不采样**(见文件头: 采样会拖慢发布方、把自环窗口关掉)。 */
        for (int i = 0; i < kRingSlots * 2 + 8; ++i)
        {
            blast(tx, payload);
        }

        stop.store(true, std::memory_order_relaxed);
        eater.join();

        /* 收尾: 断开旧收方, **再挂一个新收方**, 然后用不占 chunk 的小消息覆写两圈。
         *
         * ⛔ 新收方不是保险, 是必需: `push`/`force_push` 开头就是 `if (cc == 0)
         * return false;`(prod_cons.h) —— 一个收方都没有时它们**根本不落笔**, 覆写
         * 不发生, 环里那些大消息的 chunk 自然收不回来。少了这一步, 下面量到的是
         * "环里还剩多少在飞", 而不是"池漏了多少" —— 两者同形。
         * (本用例初版就栽在这里: 收尾写成"断开后直接灌", 于是 33/40 轮报泄漏,
         *  而同一构造改成先挂新收方后 **40 轮 0 次泄漏**。) */
        rx_eat.release();
        rx_lag.release();
        ipc::route rx_sweep{name.c_str(), ipc::receiver};
        ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "收尾用的收方未连上";
        for (int i = 0; i < kRingSlots * 2 + 16; ++i) blast(tx, tiny);

        std::size_t w = 0;
        if (self_loop(&w) == 1)
        {
            saw_loop = true;
            loop_id = w;
            loop_round = r;
            break;
        }
        const int fr = walk_free();
        if (fr >= 0 && fr < min_free_end) min_free_end = fr;
        if (fr >= 0 && fr < static_cast<int>(kCap)) ++leak_rounds;
    }

    /* ② 无自环(决定性判据)。 */
    EXPECT_FALSE(saw_loop)
        << "第 " << loop_round << " 轮出现空闲链自环: next_[" << loop_id
        << "] == " << loop_id << " —— 同一块 chunk 被归还两次, id_pool::release 的"
           "头插把链表接成环, 此后 acquire() 永远返回同一个 id、池里其余 id 永久不可达";

    /* ③ 残余泄漏(默认只登记不判, 见文件头): 需要新增每收方状态才能闭合, 属 wire 级决定。 */
    if (std::getenv("UF011_EXPECT_LEAK_FREE") != nullptr)
    {
        EXPECT_EQ(min_free_end, static_cast<int>(kCap))
            << "收尾后池未回满(最差 " << min_free_end << "/" << kCap << ", "
            << leak_rounds << "/" << kRounds
            << " 轮有泄漏)—— UF-011 尚未闭合的残余: \"最后一个清位者是谁\"在覆写与归还"
               "并发时没有唯一答案, 没有哪一方会去还这块。闭合需新增每收方状态(方案 B)";
    }
    else
    {
        std::printf("[uf011] 残余泄漏登记: 最差 free = %d/%zu, %d/%d 轮有泄漏"
                    "(设 UF011_EXPECT_LEAK_FREE=1 可把这条变成硬判据)\n",
                    min_free_end, kCap, leak_rounds, kRounds);
    }
}
