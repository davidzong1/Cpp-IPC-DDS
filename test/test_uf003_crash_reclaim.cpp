/**
 * test_uf003_crash_reclaim.cpp - UF-003 判据: 池无崩溃回收。
 *
 * 缺陷态: 收方进程被 kill -9 后, 它名下已借出的 chunk 位图位永久悬挂 —— 同档位
 * 池被逐块吃干, 发布方从池满起永久退化为 TLV 分片拷贝(run 起来是"能跑但一直
 * 拷包", 没有报错)。
 * 修法(拍板文档 docs/uf003_crash_reclaim_decision.md): elems 尾部内嵌 owner 表
 * (位 → (pid, starttime)); 池穷尽时清扫"本路由借出、已发布、且持有者**确定已死**"
 * 的块。任何不确定一律放弃(保守方向: 漏而不夺)。
 *
 * ## 判据点只有 loan
 * loan 直接回答"池里还有没有可借的块"。send 在池空时会静默退化为 TLV 分片并照样
 * 返回 true, 对本案**没有分辨力**。
 *   ① 持有者活着        ⇒ loan 必须失败(绝不夺活人的块);
 *   ② 持有者被 kill -9  ⇒ 池穷尽触发的清扫必须让 loan 恢复(且可持续多轮);
 *   ③ 多持有者只死一个  ⇒ 仍不得回收(整块放弃, 活着那位的块不可被复用)。
 * ② 的因果是封闭的: 池里的块只有两条归还路(最后持有者的 buff_t 析构 / 覆写时
 * discard)。被 SIGKILL 的子进程两条都走不到, 消息也压在环里(40 槽未环绕), 所以
 * "loan 恢复"只能由清扫解释。
 *
 * ## 构造纪律(踩过的坑, 别重犯)
 * 1. **本档位池是全机共享的**(段名按 chunk_size 分档, 不带话题/进程分量): 别的
 *    路由的残留块会让本测试借不到块, 而清扫按设计**只回收本路由的块**(位号只在
 *    同路由 owner 表里有意义, 跨路由解释位图是错的)。因此这里用**私有尺寸档**
 *    (kBigSize 取一个没人用的尺寸, 落在 20480 档), 并做池卫生前置探测: 可用块数
 *    != 池容量就显式 skip, 而不是留下一个会被误读的红/绿。
 * 2. **不要把消息发得比池子多**: 池空后大消息退化成 TLV 分片(每片 1 个环槽),
 *    环被灌满后 force_push 覆写会按 rem_cc 归还那些块 —— 池便不再保持"被持有",
 *    判据就测不到清扫。只发**恰好打满池子**的条数。
 * 3. **clear_storage 必须在本进程所有同路由 route 析构之后调用**: 它是"打断任何
 *    正在用该段进程"的破坏性操作(见 test_pool_exhaust_observability.cpp 的注记),
 *    在 route 存活时调用会让该 route 的析构踩到已清空的内存(HEAD 上即可复现的
 *    段错误, 与 UF-003 无关)。本文件用显式作用域保证顺序。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/utility/id_pool.h"

namespace {

/// 大于 large_msg_limit(= data_length) ⇒ 走 chunk 借出路径。
///
/// 尺寸档选择有两个约束, 都踩过:
///   ① 必须是**既有测试/工具都不用的档**: 本档位池是全机共享的, 撞档就会借不到块
///      (见文件头"构造纪律 1")。20480 → 档 21504, 全仓无人使用。
///   ② 必须是 **large_msg_align(1024) 的整数倍**: loan 与 send 算档的公式不同 ——
///      send 是 calc_chunk_size(P), loan 是 calc_chunk_size(loan_size_class(P));
///      两者只有在 P 已对齐时才相等(否则 loan 会落到**下一档**, 探到的池与发送
///      占的池根本不是同一个 —— 判据会以"活持有者没挡住 loan"的假红告终)。
constexpr std::size_t kBigSize = 20480;
/// 池容量(每档位)。把池打满需要恰好这么多条大消息。
constexpr std::size_t kPoolCap =
    static_cast<std::size_t>(ipc::id_pool<>::max_count);

bool wait_readable(int fd, int timeout_ms) {
    struct pollfd p;
    std::memset(&p, 0, sizeof(p));
    p.fd = fd;
    p.events = POLLIN;
    return ::poll(&p, 1, timeout_ms) == 1;
}

/// 起一个"收下并持有"的收方子进程: 连接完成(owner 槽已声明)后向父进程报信,
/// 此后持续 recv 并把 buff_t 全部**持有**在堆上(永不析构)。
/// 这样它名下的 chunk 是"合法占用"而非悬挂; 崩溃(kill -9)后位图位与 owner
/// 记录才双双悬挂 —— 正是本缺陷要回收的形态。
/// \return 子进程 pid(失败 -1); 成功时 *out_rd 是就绪通报的读端。
pid_t spawn_holding_receiver(char const *name, int *out_rd) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    pid_t const pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(fds[0]);
        /* 连接即拿到位并声明 owner 槽(见 elem_array::connect_receiver)。 */
        ipc::route rx{name, ipc::receiver};
        char const one = '1';
        ssize_t const n = ::write(fds[1], &one, 1);
        (void)n;
        ::close(fds[1]);
        /* 持有到进程被杀: 绝不析构, 也绝不优雅断连。 */
        auto *held = new std::vector<ipc::buff_t>();
        for (;;) {
            auto b = rx.recv(200);
            if (!b.empty()) held->push_back(std::move(b));
        }
    }
    ::close(fds[1]);
    *out_rd = fds[0];
    return pid;
}

/// 等到子进程报信(默认 5s); 超时返回 false。
bool await_ready(int rd, int timeout_ms = 5000) {
    if (!wait_readable(rd, timeout_ms)) return false;
    char b = 0;
    return ::read(rd, &b, 1) == 1;
}

void reap(pid_t pid) {
    int status = 0;
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
}

/// 池卫生前置: 借样直到失败, 每块立刻归还, 返回本档位**可用块数**。
/// == kPoolCap ⇒ 池干净(判据前提成立);
/// <  kPoolCap ⇒ 本档位被占着 —— 别路由的残留(按设计不可回收)或上一轮遗留的
/// 活持有者。两种情况判据前提都不成立, 调用方应显式 skip 并说明原因。
/// 顺带作用: 若残留块是**本路由**的且持有者已死, 这轮探测本身就会触发清扫把它
/// 收回来(表明确有其事), 于是返回 kPoolCap —— 这正是"自家残留可自愈"的性质。
std::size_t probe_free_chunks(ipc::route &tx) {
    std::size_t n = 0;
    while (n < kPoolCap) {
        auto lo = tx.loan(kBigSize);
        if (!lo.valid()) break;
        tx.discard_loan(lo);
        ++n;
    }
    return n;
}

/// 一条大消息的落点(载荷模式无关紧要, 只借尺寸档)。
std::vector<unsigned char> big_payload() {
    return std::vector<unsigned char>(kBigSize, 0x5A);
}

} // namespace

/* ① + ②: 单持有者。活着时 loan 必须失败; kill -9 后清扫让池恢复, 且恢复是
 * 可持续的(连续多轮借出/发布都成功) —— 悬挂块真的回了池, 不是一次侥幸。 */
TEST(Uf003CrashReclaim, DeadHolderIsReclaimedAtExhaustion)
{
    std::string const name = "uf003_dead_holder";
    ipc::route::clear_storage(name.c_str()); // 前置: 无同路由活路由时清残留段

    int rd = -1;
    pid_t const child = spawn_holding_receiver(name.c_str(), &rd);
    ASSERT_GT(child, 0);
    ASSERT_GE(rd, 0);
    ASSERT_TRUE(await_ready(rd)) << "收方子进程未在时限内就绪";
    ::close(rd);

    {
        ipc::route tx{name.c_str(), ipc::sender};

        std::size_t const free_chunks = probe_free_chunks(tx);
        if (free_chunks != kPoolCap) {
            reap(child);
            GTEST_SKIP() << "本档位(21504)chunk 池被非本路由的块占用: 可用 "
                         << free_chunks << "/" << kPoolCap
                         << " —— 清扫按设计不碰别路由的块, 判据前提不成立。"
                            "检查是否有别的测试/进程在用这一档或遗留了活持有者。";
        }

        std::vector<unsigned char> const buf = big_payload();
        for (std::size_t i = 0; i < kPoolCap; ++i) {
            ASSERT_TRUE(tx.send(buf.data(), buf.size()))
                << "第 " << i << " 条发送失败";
        }

        /* 持有者还活着: 池被它吃满, 但清扫必须一块都不夺。 */
        {
            auto lo = tx.loan(kBigSize);
            EXPECT_FALSE(lo.valid())
                << "活持有者的块被回收了 —— 保守判据失守";
            if (lo.valid()) tx.discard_loan(lo);
        }

        /* 崩溃形态: kill -9, 不优雅断连(位与 owner 记录都悬挂)。 */
        reap(child);

        /* 池穷尽 ⇒ 清理死持有者的悬挂块 ⇒ loan 恢复; 连续多轮都成功才算真恢复。 */
        for (int i = 0; i < 3; ++i) {
            auto lo = tx.loan(kBigSize);
            ASSERT_TRUE(lo.valid()) << "第 " << (i + 1)
                                    << " 轮借样失败: 死持有者的悬挂块未被回收";
            std::memset(lo.data, 0x3C, lo.size);
            ASSERT_TRUE(tx.publish_loan(lo, 1000));
        }
    } // tx 先析构, 再清段(见文件头"构造纪律 3")

    ipc::route::clear_storage(name.c_str());
}

/* ③: 多持有者。一块 chunk 的位图里有活人时, 整块必须放弃 —— 死者的悬挂不能
 * 成为"活人正在读的块被复用"的跳板。只有全部持有者都确定已死才允许回收。 */
TEST(Uf003CrashReclaim, LiveCoHolderBlocksReclaim)
{
    std::string const name = "uf003_multi_holder";
    ipc::route::clear_storage(name.c_str());

    int rd1 = -1;
    int rd2 = -1;
    pid_t const c1 = spawn_holding_receiver(name.c_str(), &rd1);
    ASSERT_GT(c1, 0);
    ASSERT_GE(rd1, 0);
    pid_t const c2 = spawn_holding_receiver(name.c_str(), &rd2);
    ASSERT_GT(c2, 0);
    ASSERT_GE(rd2, 0);
    ASSERT_TRUE(await_ready(rd1)) << "收方 #1 未就绪";
    ASSERT_TRUE(await_ready(rd2)) << "收方 #2 未就绪";
    ::close(rd1);
    ::close(rd2);

    {
        ipc::route tx{name.c_str(), ipc::sender};

        std::size_t const free_chunks = probe_free_chunks(tx);
        if (free_chunks != kPoolCap) {
            reap(c1);
            reap(c2);
            GTEST_SKIP() << "本档位(21504)chunk 池被非本路由的块占用: 可用 "
                         << free_chunks << "/" << kPoolCap
                         << " —— 判据前提不成立(清扫按设计不碰别路由的块)。";
        }

        std::vector<unsigned char> const buf = big_payload();
        for (std::size_t i = 0; i < kPoolCap; ++i) {
            ASSERT_TRUE(tx.send(buf.data(), buf.size()))
                << "第 " << i << " 条发送失败";
        }

        /* 只死一个: 位图里那位活着的收方仍持有 ⇒ 一块都不得回收。 */
        reap(c1);
        {
            auto lo = tx.loan(kBigSize);
            EXPECT_FALSE(lo.valid())
                << "还有活持有者共持这块 chunk, 却被回收了 —— 保守判据失守";
            if (lo.valid()) tx.discard_loan(lo);
        }

        /* 全部死透: 才允许清扫回收。 */
        reap(c2);
        {
            auto lo = tx.loan(kBigSize);
            EXPECT_TRUE(lo.valid()) << "全部持有者都已死亡, 清扫仍不回收";
            if (lo.valid()) tx.discard_loan(lo);
        }
    }

    ipc::route::clear_storage(name.c_str());
}
