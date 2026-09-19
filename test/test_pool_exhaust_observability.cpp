/* 步骤① 回归: chunk 池耗尽的**观测面** (docs/shm_chunk_pool_occupancy_plan.md §3 步骤①)
 *
 * 被测的不是池的行为, 而是"池空了这件事能不能被看见" —— 在 note_pool_exhausted
 * 落地之前, 池空是**完全静默**的: send / no_member_send 退化成 64 字节分片(那两处
 * 原有的 log 被注释掉了), loan 直接返回空。test_chunk_hold.cpp 的注释里也写着
 * "chunk 池的状态无法从公开 API 直接观测" —— 本文件补的就是这一格。
 *
 * 两个方向都必须有, 缺一不可:
 *   ExhaustionIsReported      —— 阳性: 池真被取空时**必须**报出来;
 *   NoReportWhenPoolCycles    —— 阴性: 池运行在容量内时**不许**报。
 *
 * ⛔ 只有阳性对照能证"看得见"; 只有阴性对照不能证"没问题" —— 那是"没触发"与
 *    "没观测"同形。所以阴性用例里带一个 stderr 捕获探针, 先证捕获面本身非退化。
 *
 * 池是按**尺寸类**分段、且**默认 prefix 为空**的:
 *   段名 = __IPC_SHM__CHUNK_INFO__<calc_chunk_size(size)>
 * (见 ipc.cpp 的 get_info / resource.h 的 make_prefix)。
 * ⇒ 同一个尺寸类的池**跨话题、跨进程共享**。因此本文件刻意避开其他用例已用的尺寸类
 *   (test_chunk_hold / test_lap_safety / test_uf007 用 5120 / 9216 / 13312), 本文件
 *   自己用 7168 / 5120 / 3072, 外加一档**带非空前缀**的 4096(前缀 poolobs_a ⇒ 段名
 *   独立, 不占用默认池的任何一档)。阳性用例会把一整池 32 块**永久钉死**(接收方从不
 *   recv, 没有任何 buff_t 析构), 同池跑阴性必假红。
 *
 * 每个用例开头先 reset_chunk_pool() 清自己那一档: 正常退出时段会被 unlink(实测跑完
 * /dev/shm 里不剩 7168/3072), 但**异常终止**(kill -9 / 崩溃)会留下已钉干的残段 ——
 * 那时"第 33 条才耗尽"这个前提就没了, 用例会退化成恒真。
 *
 * 已知无关缺陷: 本文不走 force_push/套圈路径, 不触及 test_chunk_hold.cpp 头注释里
 * 登记的那个"套圈重读致 id 重复入池"的独立缺陷。
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/ipc.h"
#include "libipc/shm.h"

namespace {

/* ipc::id_pool<>::max_count == ipc::large_msg_cache == 32。每个尺寸类一个池。 */
constexpr int kChunkPoolSize = 32;

/* 大于 ipc::large_msg_limit(=64) 才走 chunk 路径。
 *
 * 尺寸类的算法(ipc.cpp 的 calc_chunk_size): align_chunk_size(16 + size), 再按
 * alignof(max_align_t) 取整; align_chunk_size 是按 large_msg_align(1024) 向上取整。
 *   calc_chunk_size(6144) = align1K(6160) = 7168
 *   calc_chunk_size(2048) = align1K(2064) = 3072
 * 两个类都不被 test_chunk_hold(5120/9216/13312)、test_lap_safety(5120/9216)、
 * test_uf007(13312) 使用。 */
constexpr std::size_t kPayloadExhaust = 6144;  // 尺寸类 7168
constexpr std::size_t kPayloadCycle   = 2048;  // 尺寸类 3072

constexpr char kMarker[] = "chunk pool exhausted";
constexpr char kProbe[]  = "POOLOBS_CAPTURE_PROBE_MARKER";

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(n, fill);
}

/* 段名规则同 src/libipc/memory/resource.h 的 make_prefix:
 *   <prefix> + "__IPC_SHM__" + "CHUNK_INFO__" + <chunk_size>
 * make_prefix 不在公开头里, 这里照抄一遍。默认 prefix 是空串 ⇒ 名字以 __IPC_SHM__ 开头。 */
std::string chunk_pool_segment(char const *prefix, std::size_t chunk_size)
{
    return std::string(prefix) + "__IPC_SHM__CHUNK_INFO__" + std::to_string(chunk_size);
}

/* 删掉某档池段, 让本用例每次从"32 块全新可用"开始。
 *
 * 为什么必须做: 池段**不随进程退出而消失**(实测 /dev/shm 里留着 1024/2048/132096/
 * 929792 四个无主段), 而阳性用例会故意把整池 32 块永久钉死(接收方从不 recv ⇒ 没有
 * 任何 buff_t 析构 ⇒ 没有 recycle_storage)。不重置的话第二次跑用例时池一开始就是空
 * 的, "第 33 条才耗尽"这个前提消失, 用例退化成恒真。
 *
 * ⛔ 前置条件(与 unfixed_defects.md §4 的残池清理同一条): 段名 = 公开 API
 *    ipc::shm::handle::clear_storage ⇒ shm_unlink, 会打断**任何**正在用该段的进程。
 *    只能在确认无活持有者(在 /proc/<pid>/maps 里 grep CHUNK_INFO, 见 unfixed_defects
 *    §4 的命令)、且该尺寸类为本文件专用时调用。
 * ⛔ 必须在创建 route 之前调用: chunk_storages() 里每个尺寸类的 shm::handle 一旦
 *    attach 就活到进程结束(get_info 里的 h.valid() 短路), 之后再 unlink 只会让本进程
 *    继续用那份旧映射。 */
void reset_chunk_pool(char const *prefix, std::size_t chunk_size)
{
    const std::string seg = chunk_pool_segment(prefix, chunk_size);
    ipc::shm::handle::clear_storage(seg.c_str());
}

/* gtest 的 CaptureStderr 不可重入, 中途 return 会把全局捕获状态留在打开态并污染
 * 后续用例 —— 用一个守卫保证任何路径下都取回。 */
class StderrCapture
{
public:
    StderrCapture() { testing::internal::CaptureStderr(); }
    ~StderrCapture()
    {
        if (!done_) testing::internal::GetCapturedStderr();
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string finish()
    {
        done_ = true;
        return testing::internal::GetCapturedStderr();
    }

private:
    bool done_ = false;
};

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

}   // namespace

/* 阳性对照: 把池取空, 断言报得出来。
 *
 * 构造方式: 接收方连上但**从不 recv** ⇒ 没有任何 buff_t 被构造, 也就没有任何
 * recycle_storage 会把 chunk 还给池。于是前 kChunkPoolSize 条各取走一块, 第
 * kChunkPoolSize+1 条必然命中 id_pool::acquire() 返回的 -1。
 *
 * 注意这条 send **仍然返回 true** —— 池空不是发送失败, 是静默降级成分片。
 * 本用例刻意把这句断言写出来: 它同时钉住了"静默"这个前提, 哪天降级路径改成
 * 返回失败, 这里会先红。 */
TEST(PoolExhaustObservability, ExhaustionIsReported)
{
    const std::string name = "pool_obs_exhaust";
    ipc::route::clear_storage(name.c_str());

    /* 先清池, 再建 route —— 顺序不能反, 见 reset_chunk_pool 的注释。 */
    reset_chunk_pool("", 7168);

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

    const auto payload = make_payload(kPayloadExhaust, 0xC3);

    StderrCapture cap;
    for (int i = 0; i < kChunkPoolSize + 1; ++i)
    {
        ASSERT_TRUE(tx.send(payload.data(), payload.size()))
            << "第 " << i << " 条发送失败 —— 池空应静默降级成分片, 而不是返回失败";
    }
    const std::string err = cap.finish();

    /* 步骤① 的产物就是这一行, 所以把它原样打到 stdout: 用例绿了不等于"报了什么"
     * 有据可查, 捕获到的原文才是实拍。 */
    if (!err.empty())
        std::fprintf(stdout, "[pool-obs] captured stderr:\n%s\n", err.c_str());

    EXPECT_TRUE(contains(err, kMarker))
        << "池被取空却一条报告都没有 —— 观测面失效。捕获到的 stderr:\n" << err;
    EXPECT_TRUE(contains(err, "kind = send")) << err;
    /* 首报必然打出 count = 1(n == 1 无条件放行, 见 note_pool_exhausted)。
     * 必须连 "(本进程)" 一起断言: 池是全机共享的, 一个光秃秃的 count 会被读成
     * "全机饿了多少次" —— 读数能被读错与读数错了是同一类问题。 */
    EXPECT_TRUE(contains(err, "count = 1 (本进程)")) << err;
    EXPECT_TRUE(contains(err, "pool capacity = 32")) << err;
    EXPECT_TRUE(contains(err, "chunk_size = 7168")) << err;
    /* ── 归因字段 ────────────────────────────────────────────────────────────
     * 默认部署下 prefix 是空串, 于是**同一尺寸档全机只有一档池**。所以这里断言的
     * 不只是"打出了 prefix", 而是"打出了空 prefix 并点明了它的含义" —— 空前缀不是
     * "没信息", 它本身就是那条信息(元凶可能在另一个进程里)。⛔ 不点明句一起断言的话,
     * prefix = '' 与"格式化根本没打出来"完全同形, 这条会假绿。 */
    EXPECT_TRUE(contains(err, "prefix = ''")) << err;
    EXPECT_TRUE(contains(err, "空前缀")) << "空 prefix 没有点明含义 —— 现场读到空字段会当"
                                            "成'没信息', 而它恰恰是归因的唯一线索:\n" << err;

    /* ── 第二档池: 锁住"一档饿过就吞掉另一档"这个已修缺陷 ─────────────────────
     *
     * 首版 note_pool_exhausted 用的是**进程内单一计数**, 于是一旦 7168 档报过
     * (count = 1), 换个档位再饿就是"第 2 次"而被节流静默吞掉 —— 恰好把"哪一档在
     * 饿"这个最需要看见的信息抹掉。实测构造就是下面这段: 同一进程里先饿 7168 再饿
     * 5120, 旧写法第二段一条都不出。
     *
     * 现在计数按 (kind, chunk_size) 各一份, 所以第二档必须也打出 **count = 1** ——
     * count = 1 这个断言正是判别式: 全局计数下它会是 2 而被吞。
     *
     * 独立话题 + 从不 recv 的接收方 ⇒ 独立钉干第二档池。 */
    {
        const std::string name2 = "pool_obs_exhaust_2";
        ipc::route::clear_storage(name2.c_str());
        reset_chunk_pool("", 5120);

        ipc::route tx2{name2.c_str(), ipc::sender};
        ipc::route rx2{name2.c_str(), ipc::receiver};   // 同样从不 recv
        ASSERT_TRUE(tx2.wait_for_recv(1, 2000)) << "第二档接收方未在超时内连上";

        const auto payload2 = make_payload(4096, 0x3C);  // calc_chunk_size(4096) = 5120

        StderrCapture cap2;
        for (int i = 0; i < kChunkPoolSize + 1; ++i)
        {
            ASSERT_TRUE(tx2.send(payload2.data(), payload2.size()))
                << "第二档第 " << i << " 条发送失败";
        }
        const std::string err2 = cap2.finish();
        if (!err2.empty())
            std::fprintf(stdout, "[pool-obs] captured stderr (2nd size class):\n%s\n",
                         err2.c_str());

        EXPECT_TRUE(contains(err2, "chunk_size = 5120"))
            << "第二档池(5120)被取空却没有报告 —— 节流把'另一个池在饿'吞掉了。"
               "捕获到的 stderr:\n" << err2;
        EXPECT_TRUE(contains(err2, "count = 1 (本进程)"))
            << "第二档的计数不是 1 —— 说明计数仍在跨档共用(旧缺陷)。stderr:\n" << err2;
        EXPECT_TRUE(contains(err2, "pool capacity = 32")) << err2;
        /* 第二档是**另一个话题**, 但报出来的 prefix 与第一档同样是空的 —— 池段名只由
         * make_prefix(prefix, {"CHUNK_INFO__", chunk_size}) 决定, 话题名根本不进去。
         * 这就是"跨话题共享同一档池"在报错面上的可见形式(现场据此判断元凶可能在
         * 另一个进程), 不是本用例凭空断言的。 */
        EXPECT_TRUE(contains(err2, "prefix = ''")) << err2;
    }

    /* ── 第三档: **带非空 prefix** 的池 —— 让 prefix 字段成为承重断言 ────────────
     *
     * 前两档的 prefix 本来就是空串, 于是"正确打出了空前缀"与"代码里硬编码了一个空
     * 串"完全同形 —— 那两条断言其实不承重(它们只能证明格式串里有这个字段, 不能证明
     * 字段真的取了池的键)。这里给 route 一个非空前缀, 池段名随之变成
     *   poolobs_a__IPC_SHM__CHUNK_INFO__4096
     * 于是报错里的 prefix 必须是 poolobs_a 本身, 硬编码空串或漏传 inf->prefix_ 都会
     * 转红。顺带坐实一件事: prefix 就是这一档池的归属判别键(报的是池的键, 不是别的)。
     *
     * 注意第三档与前两档用的是**不同前缀**, 所以即便尺寸类撞了也是两个独立的池 ——
     * 这也是"带前缀就分池"的直接体现。 */
    {
        const std::string name3 = "pool_obs_exhaust_3";
        ipc::route::clear_storage(ipc::prefix{"poolobs_a"}, name3.c_str());
        reset_chunk_pool("poolobs_a", 4096);

        ipc::route tx3{ipc::prefix{"poolobs_a"}, name3.c_str(), ipc::sender};
        ipc::route rx3{ipc::prefix{"poolobs_a"}, name3.c_str(), ipc::receiver};  // 从不 recv
        ASSERT_TRUE(tx3.wait_for_recv(1, 2000)) << "第三档(带前缀)接收方未在超时内连上";

        const auto payload3 = make_payload(3072, 0xA5);  // calc_chunk_size(3072) = 4096

        StderrCapture cap3;
        for (int i = 0; i < kChunkPoolSize + 1; ++i)
        {
            ASSERT_TRUE(tx3.send(payload3.data(), payload3.size()))
                << "第三档第 " << i << " 条发送失败";
        }
        const std::string err3 = cap3.finish();
        if (!err3.empty())
            std::fprintf(stdout, "[pool-obs] captured stderr (prefixed pool):\n%s\n",
                         err3.c_str());

        EXPECT_TRUE(contains(err3, "chunk_size = 4096")) << err3;
        EXPECT_TRUE(contains(err3, "prefix = 'poolobs_a'"))
            << "带前缀的池没有把 prefix 原样报出来 —— 归因字段不承重"
               "(硬编码空串也会绿)。stderr:\n" << err3;
        /* "空前缀"那句点明只在默认(全机共享)部署下成立, 带前缀时不该出现 ——
         * 否则说明它是无条件拼上去的, 那么"点明"本身就是假的。 */
        EXPECT_FALSE(contains(err3, "空前缀"))
            << "非空前缀的池却打了'空前缀'的说明 —— 点明句不是条件输出。stderr:\n" << err3;
    }

    /* 刻意不 drain, 且 rx 在此析构。被钉住的 32 块留在本尺寸类(7168)里 ——
     * 这正是"接收方不读 ⇒ 池被钉干"的最小复现, 也是本用例必须独占尺寸类的原因。 */
}

/* 阴性对照: 池在容量内循环, 断言一条都不报。
 *
 * 每轮 send → recv → buff_t 析构归还, 任意时刻最多 1 块在池外, 远低于 32。
 * 轮数也刻意小于池容量, 保证环不会绕圈(不触及 force_push/覆写路径)。 */
TEST(PoolExhaustObservability, NoReportWhenPoolCycles)
{
    const std::string name = "pool_obs_cycle";
    ipc::route::clear_storage(name.c_str());
    reset_chunk_pool("", 3072);

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

    const auto payload = make_payload(kPayloadCycle, 0x5A);

    StderrCapture cap;
    /* ⛔ 捕获面自证: 不加这一句, 下面的"没报耗尽"与"根本没捕获到 stderr"完全同形,
     * 用例会假绿。 */
    std::fprintf(stderr, "%s\n", kProbe);

    constexpr int kRounds = 8;
    for (int i = 0; i < kRounds; ++i)
    {
        ASSERT_TRUE(tx.send(payload.data(), payload.size())) << "第 " << i << " 轮发送失败";
        ipc::buff_t got = rx.recv(2000);
        ASSERT_FALSE(got.empty()) << "第 " << i << " 轮未收到";
    }   // got 每轮析构 ⇒ chunk 归还池
    const std::string err = cap.finish();

    ASSERT_TRUE(contains(err, kProbe))
        << "stderr 捕获面本身失效 —— 本用例的阴性结论不成立(不能当作'没有耗尽'):\n"
        << err;
    EXPECT_FALSE(contains(err, kMarker))
        << "池运行在容量内却报了耗尽(池可能被上一次运行/其他进程污染):\n" << err;
}
