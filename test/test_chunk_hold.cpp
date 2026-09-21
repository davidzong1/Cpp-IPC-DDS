/* Step 0 回归: 覆写槽位时 chunk 的条件归还 (docs/dzflat_shm.md §5.1)
 *
 * 被测不变式来自 ipc.cpp: discard_storage —— force_push 覆写一个承载大消息的槽位时:
 *   ① 已经 pop 过该槽位、可能仍持有 buff_t 的接收方, 其 chunk 不得被提前归还
 *      (否则 id 回池被复用, 持有者手里的内存会被下一帧覆写);
 *   ② 永远读不到该消息的接收方(尚未 pop), 其位必须被清掉, 否则 chunk 永久泄漏。
 *
 * 两个用例的性质不同, 别混为一谈:
 *   HeldChunkSurvivesOverwriteWithLaggingPeer  —— ①, 复现旧实现的缺陷(旧代码必失败);
 *   OverwrittenChunksAreReclaimed              —— ②, 防止新实现引入泄漏。旧代码无条件
 *                                                 release 所以本来不会泄漏, 这一条是给
 *                                                 "清 rem_cc" 这个改动兜底的回归护栏。
 *
 * 两个用例用不同的 payload 尺寸, 因为 chunk 池是**按尺寸类**分段的
 * (CHUNK_INFO__<chunk_size>, 见 ipc.cpp: chunk_storage_info)。分开尺寸类 = 分开
 * 池子, 用例之间互不污染。
 *
 * 已知的无关缺陷(本文件刻意绕开, 不在 Step 0 射程内): 被套圈(lapped)的接收方在
 * 读空时会把同一个环槽位读到两次 —— cur 落后写指针超过 256 时, cur 与 cur+256 映
 * 射到同一 index_of。若该槽位承载大消息, 同一个 storage id 会产生两个 buff_t, 于
 * 是 recycle_storage 走两次: 第一次把 id 放回 id_pool, 第二次在 conns 已为 0 的情
 * 况下再放一次, 空闲链表出现重复项(id_pool::release 是 next_[id] = cursor_ 的头插,
 * 重复入池会把链表接成自环并丢掉其后的全部 id)。这属于 prod_cons.h force_push 注
 * 释里登记的"套圈/覆写与 pop 无互斥"同一族, 需要在 pop() 侧加覆写检测才能根除。
 */
#include <cstdint>
#include <cstring>
#include <ios>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/ipc.h"

namespace {

/* ipc::id_pool<>::max_count = ipc::large_msg_cache, 每个尺寸类全进程共享。
 * ⛔ 由常量导出: 写死魔数在容量变更时会让洪泛校准与 static_assert 全部失真。 */
constexpr int kChunkPoolSize = static_cast<int>(ipc::large_msg_cache);

/* 环的槽位数: circ::elem_array::elem_max = numeric_limits<uint8>::max() + 1。 */
constexpr int kRingSlots = 256;

/* 每个槽位承载的字节数: ipc::data_length。超过它(large_msg_limit 同值)才走 chunk。 */
constexpr int kSlotBytes = 64;

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

/* tm = 0: push 失败即刻退化到 force_push, 不等待。用默认 100ms 会让本文件里上百次
 * 必然失败的推送变成分钟级。 */
bool blast(ipc::route& tx, const std::vector<std::uint8_t>& payload)
{
    return tx.send(payload.data(), payload.size(), 0);
}

int drain(ipc::route& rx, std::uint64_t tm = 60)
{
    int n = 0;
    for (;;)
    {
        ipc::buff_t got = rx.recv(tm);
        if (got.empty()) break;
        ++n;
    }
    return n;
}

}   // namespace

/* ① 提前归还: 一个接收方持有 chunk 期间, 该消息所在槽位被 force_push 覆写, 持有
 *    的内容不得改变。
 *
 * 必须有**两个进度不同的接收方**才能触及这条路径, 单接收方构造不出来:
 *   - 覆写回调(clear_message)只在 push() 失败、退化到 force_push() 时才被调用;
 *   - push() 失败的条件是该槽位仍有接收方没 pop 过;
 *   - 而"没 pop 过该槽位"的接收方不可能持有这条消息的 chunk。
 * 所以单接收方时, 它 pop 过的槽位会被普通 push() 静默覆写(无回调, 什么都不释放),
 * 旧实现的缺陷根本不会显形。
 *
 * 真正的场景是: rx_fast pop 到 A 并握着它的 buff_t(chunk conns 里它的位还在),
 * rx_slow 从不读(卡住同一槽位) → 环绕回来时 push() 因 rx_slow 而失败 → force_push
 * → clear_message。旧实现在这里无条件 release_storage, 于是把 rx_fast 正握着的
 * chunk id 放回池子, 下一条大消息 acquire 到同一 id 并 memcpy 覆盖。 */
TEST(ChunkHold, HeldChunkSurvivesOverwriteWithLaggingPeer)
{
    constexpr std::size_t kPayload = 4096;   // 尺寸类 calc_chunk_size(4096) = 5120
    constexpr std::size_t kTiny = 32;        // <= large_msg_limit, 不占 chunk, 1 槽位/条
    const std::string name = "dzflat_step0_hold";
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx_fast{name.c_str(), ipc::receiver};
    ipc::route rx_slow{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(2, 2000)) << "两个接收方未在超时内连上";

    const auto pattern_a = make_payload(kPayload, 0xA5);
    const auto pattern_b = make_payload(kPayload, 0x5B);
    const auto tiny = make_payload(kTiny, 0x11);

    /* A 落在槽位 0, 拿到 chunk id 0(id_pool 的 cursor_ 从 0 开始)。 */
    ASSERT_TRUE(blast(tx, pattern_a));

    /* rx_fast pop 到 A 并**一直持有**: 它在槽位 0 上的 rc 位已清, 但 chunk conns
     * 里它的位要到这个 buff_t 析构才清 —— 即"读了还握着"。rx_slow 全程不读。 */
    ipc::buff_t held = rx_fast.recv(2000);
    ASSERT_FALSE(held.empty()) << "rx_fast 未收到 A";
    ASSERT_TRUE(all_bytes_are(held, kPayload, 0xA5)) << "A 的内容本身就不对";

    /* 用小消息把写指针推满一圈(每条恰好 1 个槽位), 让第 kRingSlots 次推送正好落回
     * 槽位 0。槽位 1..255 此前是空的(rc_ = 0) → 普通 push 成功; 槽位 0 因 rx_slow
     * 尚未 pop → push 失败 → force_push → clear_message(rem_cc = {rx_slow})。 */
    for (int i = 0; i < kRingSlots; ++i)
    {
        ASSERT_TRUE(blast(tx, tiny)) << "第 " << i << " 条小消息发送失败";
    }

    /* 若上一步把 A 的 chunk id 归还了, 它此刻正躺在空闲链表头部(release 是头插,
     * 且此前只分配过 id 0), 下一条大消息就会 acquire 到它并覆写 held 的内存。 */
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(blast(tx, pattern_b));
    }

    EXPECT_TRUE(all_bytes_are(held, kPayload, 0xA5))
        << "被持有的 chunk 在覆写后遭到复用 —— 覆写路径把仍被 rx_fast 持有的 "
           "chunk id 提前归还了(首字节 0x"
        << std::hex << int(*static_cast<const std::uint8_t*>(held.data())) << ")";

    /* 刻意不 drain: 两个接收方此刻都已被套圈, 读空会踩到文件头注释里那个无关的重复
     * 入池缺陷。held 析构即归还 A 的 chunk; 本尺寸类(5120)不被其他用例使用。 */
}

/* ② 泄漏护栏: 覆写掉"没人 pop 过"的大消息时, 其 chunk 必须被回收。
 *
 * chunk 池的状态无法从公开 API 直接观测, 用分片悬崖作判别式: 池子健康时一条大消息
 * 占 1 个槽位, 池子耗尽时退化成 kPayload/64 个槽位。因此"接收方不读的情况下 N 条
 * 消息能有几条留在环里"就区分了两种状态。取 N < 32 让健康分支不受池容量影响。 */
TEST(ChunkHold, OverwrittenChunksAreReclaimed)
{
    constexpr std::size_t kPayload = 8192;   // 独立尺寸类 calc_chunk_size(8192) = 9216
    constexpr int kProbe = 20;
    static_assert(kProbe < kChunkPoolSize, "探针条数须小于 chunk 池容量");

    const std::string name = "dzflat_step0_reclaim";
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    const auto payload = make_payload(kPayload, 0xC3);

    /* 分片后每条占的槽位数; 覆写 slot 0 需要写指针推进满一圈。 */
    constexpr int kSlotsPerFragmented = static_cast<int>(kPayload) / kSlotBytes;
    constexpr int kFloodMsgs = kRingSlots / kSlotsPerFragmented + 2;

    {
        ipc::route rx1{name.c_str(), ipc::receiver};
        ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "rx1 未在超时内连上";

        /* 吃干池子: 全部大消息无人 pop, 各占 1 个槽位。 */
        for (int i = 0; i < kChunkPoolSize; ++i) ASSERT_TRUE(blast(tx, payload));

        /* 池已空 → 后续消息分片, 每条 kSlotsPerFragmented 个槽位, 绕环覆写掉上面
         * 那 32 条。它们谁都没 pop 过 → rem_cc 含 rx1 的位 → 必须被回收。 */
        for (int i = 0; i < kFloodMsgs; ++i) blast(tx, payload);

        /* rx1 全程不读, 就地销毁 —— 不走它的读空路径(见文件头注释的无关缺陷)。 */
    }

    /* 换一个全新接收方: 它的游标从当前写指针开始, 不存在套圈, 读空是干净的。 */
    ipc::route rx2{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "rx2 未在超时内连上";
    drain(rx2);   // 丢掉握手期间可能落入的残留

    for (int i = 0; i < kProbe; ++i) ASSERT_TRUE(blast(tx, payload));
    const int received = drain(rx2);

    EXPECT_EQ(received, kProbe)
        << "洪泛后 chunk 池未恢复(" << received << "/" << kProbe
        << ") —— 覆写路径漏掉了没人 pop 过的大消息的 chunk, "
           "大消息已退化为 " << kSlotsPerFragmented << " 槽位/条的分片";
}

/* ③ 大消息 buff_t 活过它的接收方: 析构时不得解引用一个已释放的 conn_info。
 *
 * 接收方析构(chan_impl::destroy)会 mem::free(conn_info), 而大消息(storage 路径)
 * 的 buff_t 析构器旧实现里握着 conn_info_t*: 于是"先析构接收方、后析构消息"这个
 * 完全合法的顺序就是 use-after-free —— 析构器要用前缀去查 CHUNK_INFO 段名, 读到
 * 已释放内存里的字节, "字符串长度"变成指针值(实测 ~1.35e14) → allocate() 因 huge
 * count 返回空(noexcept, 不抛) → libstdc++ 在 nullptr 上 memmove → SIGSEGV at 0
 * (error 6, in libc.so.6)。内核日志只剩一个 ip, 现场完全看不出是哪一行。
 *
 * 这就是本次观察到的那次"一次性段错误": 它的偶发只是因为"释放内存是否已被复用"
 * 取决于堆运气(ASAN 下必报 heap-use-after-free, 见 docs/shm_defect_fixes.md 第 7 条)。
 *
 * 为了让本用例**每次都真的踩到**(而不是等运气): 接收方析构后立即用一串同尺寸类的
 * 分配把刚释放的 conn_info 块拿到手中并填 0xAB。旧实现在下一步必然在毒化后的
 * 内存上算出一个巨大长度, 整个用例二进制当场挂掉; 修复后这里没有任何解引用。
 */
TEST(ChunkHold, HeldLargeMessageSurvivesItsReceiver)
{
    constexpr std::size_t kPayload = 12288;   // 独立尺寸类: 与上面两条用例(4096/8192)不共享池子
    const std::string name = "dzflat_uaf_hold";
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::buff_t held;

    {
        ipc::route rx{name.c_str(), ipc::receiver};
        ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

        const auto payload = make_payload(kPayload, 0xC7);
        ASSERT_TRUE(blast(tx, payload));

        held = rx.recv(2000);
        ASSERT_FALSE(held.empty()) << "未收到大消息(应走 chunk 路径)";
        ASSERT_TRUE(all_bytes_are(held, kPayload, 0xC7));
    }   // rx 析构 → conn_info 被 mem::free; held 仍持有那条消息

    /* 毒化: conn_info_t 在这套策略下是 224 字节(尺寸类 240), 它在 tcache 里是
     * "最近释放的那块", 所以同尺寸类的第一次分配就会把它交出来。扫一段区间是为了
     * 不把尺寸类写死在用例里(策略/编译器变化时仍能命中)。 */
    std::vector<std::vector<std::uint8_t>> poison;
    for (std::size_t n = 128; n <= 512; n += 8)
    {
        poison.emplace_back(n, 0xAB);
    }

    /* 崩点(修复前) / 正常归还 chunk(修复后)。 */
    held = ipc::buff_t{};

    /* 归还后 chunk 必须仍可正常复用: 同一尺寸类再走一轮收发, 内容逐字节一致。 */
    {
        ipc::route rx{name.c_str(), ipc::receiver};
        ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "第二轮接收方未连上";
        const auto payload = make_payload(kPayload, 0x5D);
        ASSERT_TRUE(blast(tx, payload));
        ipc::buff_t got = rx.recv(2000);
        ASSERT_FALSE(got.empty()) << "chunk 未能复用: 第二轮大消息没收到";
        EXPECT_TRUE(all_bytes_are(got, kPayload, 0x5D)) << "复用后的 chunk 内容不对";
    }

    /* poison 必须活到 held 析构之后, 毒化才有意义(否则它自己先被释放回收)。 */
    EXPECT_EQ(poison.size(), 49u) << "毒化块数量变了, 检查上面的扫描区间";
}
