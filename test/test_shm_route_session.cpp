/* 阶段 2 `RouteSession` 单元层验收：lease 配对 + 重建顺序 + stop/wake 叫醒。
 *
 * 落点与分工：docs/消息接收架构改造/阶段2_RouteSession实现说明.md §7 分工表 A 项
 * （「不链接完整握手」）。本用例只驱动 RouteSession 本体，不构造 shm_sub_ipc、
 * 不接控制面调度器 —— 那些是 B/C/D 的事。
 *
 * ── 为什么这些判据是承重的（不是"跑绿即过"）──────────────────────────────────
 * RouteSession 的失效方式全是**静默**的，不崩、不报错：
 *   ① 重建时不等待在途 recv 就 release() ⇒ recv 与 release 并发 ⇒ 已释放句柄被踩。
 *      症状是偶发 SIGSEGV，功能测「最终又能收到消息」永远抓不到（说明 §8.2 明确
 *      禁止只测功能）。
 *   ② 重建中不拒绝新 lease ⇒ 新 recv 进入已被 release 的对象。
 *   ③ release_receive 漏配对 ⇒ wait_quiescent 永不返回（挂死）。
 *   ④ release_receive 多配对 ⇒ size_t 下溢 ⇒ 计数成天文数字 ⇒ 同样永不返回。
 *   ⑤ 重建失败后留下 rebuilding_ == true ⇒ 该话题收包永久停摆（acquire 永远为空）。
 * 故每条都以**确定性事件探针 + 数值边界**断言，不靠"睡够就行"。
 *
 * ── 判据为什么能确定性成立（不依赖时序余量）────────────────────────────────
 * 「release() 必须发生在 recv 返回且 release_receive 之后」这条用**探针顺序**判定：
 *   · create 回调是说明 §4 第 6 步，唯一入口 ⇒ 它被调用就是"第 5 步（release）已过"；
 *   · create 回调内读到的旧 route 必须 `valid() == false`（已 release）；
 *   · create 回调内读到的 `released` 标志必须为 true（release_receive 已执行）。
 * 三条都是**因果序**，与机器快慢无关。若实现改成"不等待 inflight 直接 release"，
 * create 会在 released 置位前被调用 ⇒ 红灯，方向直接指向被破坏的那一步。
 *
 * ── 为什么用真实 ipc::route 而不是测试替身 ────────────────────────────────
 * `ipc::route` 是 `chan_wrapper` 具体类，`release()`/`disconnect()` **非虚**，
 * 无法替身。替代的观察手段是它自己的可观测副作用：
 *   · release() 后 `valid()` 变 false（h_ 置空）⇒ 可判"已 release"；
 *   · shared_ptr 仍持有 ⇒ 可判"对象未被析构"（说明 §4："shared_ptr 只能保证
 *     C++ 对象还在，保证不了 release() 与 recv() 不并发"）。
 *
 * ── 叫醒伪影：disconnect() 叫醒的 recv 返回的是**零填充**而非空 buffer ──────
 * 实测（/tmp 隔离探针，未改产品码）：
 *   · 超时路径  → `buff_t{}`：size=0、empty()==true；
 *   · 叫醒路径  → size=64(=ipc::data_length)、**全零**、empty()==false。
 * 机理：`ipc.cpp:1044` 的 `wait_for` 在 `quit_waiting()` 后直接 `break` 返回 true，
 * 而 `pop()` 并未填充 msg ⇒ `msg{}` 值初始化为全零 ⇒ `r_size = 64 + 0 > 0` ⇒
 * 返回 64 字节零缓冲。所以"叫醒 ⇒ 返回空"这个前提**不成立**，本文件不得按它断言
 * （曾按它写，导致 StopAndWakeUnblocksBlockedRecv 首次运行即假红）。
 *
 * ⚠️ 由此暴露一个**既有缺陷（本阶段未修，已上报）**：收包循环只判
 * `raw_data.empty()`（`shm_pub_sub_ipc.cc:836`），零填充缓冲过不了这一关，会继续
 * 走分流；当话题 `msg_id == 0`（`TopicDataPtrMake<T>()` 的默认值）时，
 * `AcceptWire` 的 `check_id`（比对缓冲**尾部 4 字节**，全零 ⇒ 0）恰好通过、
 * `deserialize_ok` 也为真 ⇒ 一条全零假消息被 push 进用户队列。本文件不断言该
 * 缺陷的行为，只用 `empty || 零填充伪影` 守住"不得返回非零负载"这条更弱的界。
 *
 * ── 段名纪律 ────────────────────────────────────────────────────────────
 * 名字**不得含 `/`**（POSIX shm 名只允许一个前导斜杠；实测 `/a/b` 会让
 * shm_open 返回 EINVAL，ipc::route 半初始化后 recv() 段错误）。用 `rs_<tag>_<n>`。
 * 每个用例用 RAII 在**所有 route 对象析构之后** clear_storage，避免段残留污染
 * 其它用例（test_sercli_auto_path 的历史教训）。
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/shm_route_session.h"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::shm;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* 有界等待：所有等待都必须有界，超时即按调用点语义判定。 */
bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/* 唯一段名 + 析构时清段。声明在 RouteSession / route 对象**之前**，于是析构逆序
 * 保证：route 先释放、段名 guard 最后清 —— 不会清掉仍在使用的段。 */
struct RouteName
{
    std::string name;

    explicit RouteName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("rs_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~RouteName() { ipc::route::clear_storage(name.c_str()); }

    const char* c_str() const { return name.c_str(); }
};

/* 建一条真实 route。verbose=false：段刚清过时 open 失败会打一串 fail shm_open
 * 日志，与本用例判据无关，静音。 */
std::shared_ptr<ipc::route> make_route(const RouteName& rn)
{
    return std::make_shared<ipc::route>(rn.c_str(), ipc::receiver, /*verbose=*/false);
}

/* create 工厂：包住 RouteName，供 begin_rebuild 反复调用。 */
std::function<std::shared_ptr<ipc::route>()> factory(const RouteName& rn)
{
    return [&rn] { return make_route(rn); };
}

}   // namespace

/* ------------------------------------------------------------------ 空状态 */
/* 未建立任何 route 时全部接口都必须可用且不崩：调用方（订阅循环、析构路径）会在
 * 首次握手完成之前就调用它们。 */
TEST(RouteSession, EmptyStateIsWellDefined)
{
    RouteName rn{"empty"};
    RouteSession s;

    EXPECT_EQ(s.generation(), 0u) << "未建立 route 时 generation 初值必须是 0";
    EXPECT_FALSE(s.current_route()) << "未建立 route 时 current_route 必须为空";
    EXPECT_FALSE(s.acquire_receive().has_value()) << "无 route 时 acquire_receive 必须返回空（I3）";

    /* stop_and_wake / wait_quiescent 在空状态必须立即返回：析构路径无条件调用它们。 */
    s.stop_and_wake();
    s.wait_quiescent();

    /* 空状态下的 stop 之后仍然拒绝新 lease。 */
    EXPECT_FALSE(s.acquire_receive().has_value());

    EXPECT_EQ(rn.name.find('/'), std::string::npos) << "段名不得含 '/'（POSIX shm 名限制）";
}

/* ------------------------------------------------------- acquire / release */
TEST(RouteSession, AcquireLeaseCarriesRouteAndGeneration)
{
    RouteName rn{"lease"};
    RouteSession s;
    s.begin_rebuild(7, factory(rn));

    ASSERT_EQ(s.generation(), 7u);
    ASSERT_TRUE(s.current_route()) << "成功重建后必须有 route";

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value()) << "已建立 route 且未 stopping/rebuilding ⇒ acquire 必须成功";
    EXPECT_EQ(lease->generation, 7u) << "lease 必须携带当前 generation";
    EXPECT_EQ(lease->route.get(), s.current_route().get()) << "lease 必须指向当前 route";
    EXPECT_TRUE(lease->route->valid()) << "lease 里的 route 必须是已连接的";

    /* 配对释放后，计数归零 ⇒ wait_quiescent 立即返回（有界判据，见下一个用例）。 */
    s.release_receive();
    s.wait_quiescent();
    EXPECT_TRUE(s.acquire_receive().has_value()) << "release 之后应能再次 acquire";
    s.release_receive();
}

/* wait_quiescent 的语义就是"等到 inflight == 0"：inflight > 0 时**不得**返回。
 * 若它无条件返回，说明 §8.2 要求的"等待 recv 结束"是假的。 */
TEST(RouteSession, WaitQuiescentBlocksWhileInflightNonZero)
{
    RouteName rn{"quiesce"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());

    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        s.wait_quiescent();
        returned.store(true, std::memory_order_release);
    });

    EXPECT_FALSE(wait_for([&] { return returned.load(std::memory_order_acquire); }, 200))
        << "inflight == 1 时 wait_quiescent 必须阻塞（否则等待是假的）";

    s.release_receive();
    EXPECT_TRUE(wait_for([&] { return returned.load(std::memory_order_acquire); }, 1000))
        << "release_receive 之后 wait_quiescent 必须在有界时间内返回";
    waiter.join();
}

/* I4 反向：多余的 release_receive 不得让 size_t 下溢。下溢会把计数变成天文数字，
 * 症状是 wait_quiescent 永久挂死 —— 与"漏配对"是同一个静默失效。 */
TEST(RouteSession, ExtraReleaseDoesNotUnderflowQuiescence)
{
    RouteName rn{"extra_release"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

#ifndef NDEBUG
    /* 本用例的触发手段是「故意违约：不配对 acquire 就 release」。产品用
     * `assert(false)` 挡这一违约（src/dzIPC/shm_route_session.cc:36）。
     *   · Release（默认构建；CMake 给 Release 加 -DNDEBUG）下 assert 被编译掉、
     *     只剩那句 return ⇒ 本用例验证的正是「计数不下溢 ⇒ wait_quiescent 仍返回」。
     *   · Debug（-DOPEN_DEBUG_MODE=ON）下 assert 生效 ⇒ 这句违约调用直接
     *     SIGABRT + 核心转储，会**中止整个测试二进制**、掩盖同批其它用例的结果；
     *     而本用例想观测的「下溢保护」在 Debug 下没有 return 路径可观测
     *     （产品选择用 abort 表达调用方违约）。
     * 故 Debug 下显式跳过并说明，不静默变绿。GTEST_SKIP 在本仓已有先例：
     * test_udp_port_boundary.cpp:390、test_alloc_fault_inject.cpp:96。
     * 实测（/tmp 隔离编译，未改产品码）：Release 下本用例 rc=0 PASSED；
     * Debug 下 rc=134（SIGABRT），加本分支后 Debug 为 SKIPPED。 */
    GTEST_SKIP() << "Debug 构建下产品以 assert(false) 中止进程, 无 return 路径可观测; "
                    "该判据由 Release(默认)构建覆盖";
#endif

    /* 没有配对 acquire，直接多释放一次。 */
    s.release_receive();

    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        s.wait_quiescent();
        returned.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(wait_for([&] { return returned.load(std::memory_order_acquire); }, 500))
        << "多余的 release_receive 让计数下溢 ⇒ wait_quiescent 永不返回（I4 反向被破坏）";
    waiter.join();

    /* 且后续正常的 acquire/release 仍然可用（计数没被污染）。 */
    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value()) << "多余的 release 不得破坏后续 acquire";
    s.release_receive();
    s.wait_quiescent();
}

/* ------------------------------------------------------------- 重建：拒绝新 lease */
TEST(RouteSession, RebuildRejectsNewAcquire)
{
    RouteName rn{"reject"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    std::atomic<int> acquire_in_create{-1};
    std::atomic<int> acquire_while_waiting{-1};

    /* 先在重建"等待 inflight 归零"期间探测（rebuilder 会卡在第 4 步）。 */
    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());

    std::atomic<bool> create_called{false};
    std::thread rebuilder([&] {
        s.begin_rebuild(2,
                        [&] {
                            create_called.store(true, std::memory_order_release);
                            /* create 在 §4 第 6 步、锁外调用 ⇒ 这里 acquire 不会自死锁，
                             * 且此刻 rebuilding_ 仍为 true ⇒ 必须被拒。
                             * 若被放行（I3 破坏）则**立即配对释放**：否则残留计数让
                             * rebuilder 卡在第 4 步等归零 ⇒ join 挂死、断言不可达。 */
                            auto inner = s.acquire_receive();
                            acquire_in_create.store(inner.has_value() ? 1 : 0,
                                                    std::memory_order_release);
                            if (inner.has_value())
                            {
                                s.release_receive();
                            }
                            return make_route(rn);
                        });
    });

    /* 前置：rebuilder 确实卡在"等 inflight 归零"，还没进 create。 */
    ASSERT_FALSE(wait_for([&] { return create_called.load(std::memory_order_acquire); }, 200))
        << "rebuilder 未等待在途 recv —— 前提不成立";

    /* 主判据 A：等待期间新 acquire 必须为空（I3）。
     * 同样先**配对释放**再记录判据：若实现放行了新 lease，被放行的计数会让
     * rebuilder 停在第 4 步等归零 ⇒ join 挂死，下面两条断言永远执行不到 ——
     * 破坏 I3 时应当得到明确红灯，而不是退化成"超时"这种无方向的症状。 */
    auto probe = s.acquire_receive();
    acquire_while_waiting.store(probe.has_value() ? 1 : 0, std::memory_order_release);
    if (probe.has_value())
    {
        s.release_receive();
    }

    s.release_receive();
    rebuilder.join();

    EXPECT_EQ(acquire_while_waiting.load(), 0)
        << "重建等待 inflight 归零期间不得放行新 lease（I3 被破坏）";
    EXPECT_EQ(acquire_in_create.load(), 0) << "create 调用期间（rebuilding_ == true）acquire 必须为空";
    EXPECT_EQ(s.generation(), 2u) << "重建成功后 generation 必须切换";
    EXPECT_TRUE(s.acquire_receive().has_value()) << "重建结束后必须重新放行 lease";
    s.release_receive();
}

/* ------------------------------------------- 重建：inflight 未归零不得 release */
/* 说明 §8.2 的核心判据：release() 必须发生在 recv 返回且 release_receive 之后。
 * 用因果序探针判定（见文件头），不靠 sleep 余量。 */
TEST(RouteSession, RebuildWaitsForInflightBeforeReleasingOldRoute)
{
    RouteName rn{"order"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());
    std::shared_ptr<ipc::route> old = lease->route;

    std::atomic<bool> released{false};
    std::atomic<bool> create_called{false};
    std::atomic<bool> old_valid_in_create{true};
    std::atomic<bool> released_before_create{false};
    std::atomic<bool> create_old_matches_lease{false};

    std::thread rebuilder([&] {
        s.begin_rebuild(2,
                        [&] {
                            create_called.store(true, std::memory_order_release);
                            /* 第 6 步 ⇒ 第 5 步（release）已过：旧 route 必须已 invalid。 */
                            old_valid_in_create.store(old->valid(), std::memory_order_release);
                            /* release_receive 必须先于 create 发生（§8.2）。 */
                            released_before_create.store(released.load(std::memory_order_acquire),
                                                         std::memory_order_release);
                            create_old_matches_lease.store(true, std::memory_order_release);
                            return make_route(rn);
                        });
    });

    /* 前置：inflight == 1 期间不得走到 create。 */
    ASSERT_FALSE(wait_for([&] { return create_called.load(std::memory_order_acquire); }, 200))
        << "inflight 未归零就进入 create —— 重建没有等待在途 recv（I2 破坏）";

    /* 主判据 A：此刻重建尚未走到第 5 步 ⇒ 旧 route 仍被 session 持有、generation 未切换。
     * 这两条都经 RouteSession 自己的锁读，无数据竞争。 */
    EXPECT_EQ(s.current_route().get(), old.get())
        << "inflight 未归零时不得 release/reset 旧 route（§4 第 5 步必须在第 4 步之后）";
    EXPECT_EQ(s.generation(), 1u) << "inflight 未归零时 generation 不得提前切换";

    /* recv 结束 + 配对释放 ⇒ 重建才可继续。 */
    s.release_receive();
    released.store(true, std::memory_order_release);
    rebuilder.join();

    /* 主判据 B：因果序三条。 */
    EXPECT_TRUE(create_called.load()) << "release_receive 之后重建必须完成";
    EXPECT_TRUE(create_old_matches_lease.load()) << "create 回调未被调用（用例构造异常）";
    EXPECT_TRUE(released_before_create.load())
        << "release() 发生在 release_receive 之前 ⇒ 与 recv 并发（说明 §8.2 被破坏）";
    EXPECT_FALSE(old_valid_in_create.load())
        << "create 被调用时旧 route 必须已 release（§4 第 5 步必须先于第 6 步）";
    EXPECT_EQ(s.generation(), 2u);

    /* 说明 §4 的边界：shared_ptr 只保证对象还在，不保证 release/recv 不并发。
     * 旧对象此刻已 release（句柄空）但**未被析构**（lease 的 shared_ptr 曾持有它）。 */
    EXPECT_FALSE(old->valid()) << "旧 route 必须已被 release";
    EXPECT_GE(old.use_count(), 1) << "旧 route 对象必须仍存活（release 只放句柄，不析构对象）";
}

/* 重建自己的第 3 步必须叫醒真实阻塞的 recv。
 * 与 StopAndWakeUnblocksBlockedRecv 分开守门：begin_rebuild 有独立的 disconnect 调用点，
 * 删掉它会让重建线程等到 recv 超时后才继续。 */
TEST(RouteSession, RebuildDisconnectUnblocksBlockedRecv)
{
    RouteName rn{"rebuild_wake"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());

    constexpr std::uint64_t kRecvTimeoutMs = 2000;
    std::atomic<bool> recv_entered{false};
    std::atomic<bool> recv_done{false};
    std::atomic<long> recv_elapsed_ms{-1};
    std::thread receiver([&] {
        recv_entered.store(true, std::memory_order_release);
        const auto started = Clock::now();
        (void)lease->route->recv(kRecvTimeoutMs);
        recv_elapsed_ms.store(
            static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()),
            std::memory_order_release);
        recv_done.store(true, std::memory_order_release);
        s.release_receive();
    });

    const bool entered = wait_for([&] { return recv_entered.load(std::memory_order_acquire); }, 500);
    /* 给 recv 进入 wait_for 的机会，并确认它没有靠超时提前返回。 */
    std::this_thread::sleep_for(200ms);
    const bool blocked = !recv_done.load(std::memory_order_acquire);

    const auto started = Clock::now();
    s.begin_rebuild(2, factory(rn));
    const auto rebuild_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    receiver.join();

    EXPECT_TRUE(entered) << "recv 线程未启动";
    EXPECT_TRUE(blocked) << "recv 未进入阻塞，不能验证 rebuild 的唤醒";
    EXPECT_TRUE(recv_done.load(std::memory_order_acquire));
    EXPECT_LT(recv_elapsed_ms.load(std::memory_order_acquire), 1000)
        << "begin_rebuild 未通过 disconnect 叫醒 recv，可能等满 2000ms 超时";
    EXPECT_LT(rebuild_ms, 1000) << "begin_rebuild 等待超过唤醒边界";
    EXPECT_EQ(s.generation(), 2u);
}

/* ------------------------------------------------ 重建：新旧对象与 generation */
TEST(RouteSession, RebuildSwapsRouteAndPublishesNewGeneration)
{
    RouteName rn{"swap"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));
    auto first = s.current_route();
    ASSERT_TRUE(first);

    s.begin_rebuild(2, factory(rn));
    auto second = s.current_route();
    ASSERT_TRUE(second);

    EXPECT_NE(first.get(), second.get()) << "重建必须换掉 route 对象，不能复用旧句柄";
    EXPECT_FALSE(first->valid()) << "旧 route 必须在重建中 release（§4 第 5 步）";
    EXPECT_TRUE(second->valid()) << "新 route 必须是已连接的";
    EXPECT_EQ(s.generation(), 2u);

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(lease->generation, 2u) << "重建后新 lease 必须携带新 generation";
    EXPECT_EQ(lease->route.get(), second.get());
    s.release_receive();
}

/* ------------------------------------------------------ 重建失败：可重试、不卡死 */
TEST(RouteSession, RebuildFailureLeavesEmptyAndRetryable)
{
    RouteName rn{"fail"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));
    ASSERT_EQ(s.generation(), 1u);

    /* create 返回空。 */
    s.begin_rebuild(2, [] { return std::shared_ptr<ipc::route>{}; });
    EXPECT_FALSE(s.current_route()) << "create 失败后 route_ 必须为空（§4）";
    EXPECT_EQ(s.generation(), 1u) << "create 失败不得让 generation 假装生效";
    EXPECT_FALSE(s.acquire_receive().has_value()) << "无 route 时 acquire 必须为空";

    /* create 抛异常：必须与返回空等价，且不得逃出 begin_rebuild（调用方是收包线程）。 */
    EXPECT_NO_THROW(s.begin_rebuild(3, []() -> std::shared_ptr<ipc::route> {
        throw std::runtime_error("injected create failure");
    })) << "create 抛出不得逃出 begin_rebuild";
    EXPECT_FALSE(s.current_route()) << "create 抛出后 route_ 必须为空";
    EXPECT_EQ(s.generation(), 1u) << "create 抛出不得让 generation 假装生效";

    /* 关键：rebuilding_ 必须已复位，否则后续 acquire 永久为空（话题收包静默停摆）。 */
    s.begin_rebuild(4, factory(rn));
    EXPECT_EQ(s.generation(), 4u) << "失败后重建必须可重试（rebuilding_ 未复位会永久卡死）";
    EXPECT_TRUE(s.acquire_receive().has_value()) << "失败重试成功后必须重新放行 lease";
    s.release_receive();
}

/* ------------------------------------------------------------ stop / wake */
/* 空状态 stop 不崩、且拒绝新 lease（已由 EmptyStateIsWellDefined 覆盖）；
 * 这里覆盖"stop 之后成功重建重新放行 lease"这一接口语义裁定（见头文件）。 */
TEST(RouteSession, StopThenSuccessfulRebuildReopensLeases)
{
    RouteName rn{"reopen"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    s.stop_and_wake();
    EXPECT_FALSE(s.acquire_receive().has_value()) << "stop 之后必须拒绝新 lease（I3）";

    /* 说明 §4 表格的「add_peer 失败」路径 = stop_and_wake + wait_quiescent + release，
     * 之后调用方走"稍后重试"；重试会再次 Ready 并 begin_rebuild。若成功重建不复位
     * stopping_，该话题收包将永久停摆。 */
    s.begin_rebuild(2, factory(rn));
    EXPECT_EQ(s.generation(), 2u);
    EXPECT_TRUE(s.acquire_receive().has_value())
        << "成功重建后必须重新放行 lease（否则重试路径永久停摆）";
    s.release_receive();
}

/* stop_and_wake 必须叫醒**正在阻塞的 recv**，而不是靠 50ms/2s 超时。
 * 说明 §5 第 4 步："卡在 recv(50) 里时必须靠 disconnect/quit_waiting 把它叫醒，
 * 不能只靠超时。" */
TEST(RouteSession, StopAndWakeUnblocksBlockedRecv)
{
    RouteName rn{"wake"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());

    constexpr std::uint64_t kRecvTimeoutMs = 2000;   /* recv 的超时，远大于叫醒延迟 */
    std::atomic<bool> recv_done{false};
    std::atomic<long> recv_elapsed_ms{-1};
    std::atomic<long> recv_size{-1};
    std::atomic<int> recv_empty{-1};
    std::atomic<int> recv_all_zero{-1};

    std::thread recv_thread([&] {
        const auto t0 = Clock::now();
        auto buf = lease->route->recv(kRecvTimeoutMs);
        recv_elapsed_ms.store(
            static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
            std::memory_order_release);
        recv_size.store(static_cast<long>(buf.size()), std::memory_order_release);
        recv_empty.store(buf.empty() ? 1 : 0, std::memory_order_release);
        {
            const auto* p = static_cast<const unsigned char*>(buf.data());
            bool all_zero = (p != nullptr);
            for (std::size_t i = 0; all_zero && i < buf.size(); ++i)
            {
                if (p[i] != 0)
                {
                    all_zero = false;
                }
            }
            recv_all_zero.store(all_zero ? 1 : 0, std::memory_order_release);
        }
        recv_done.store(true, std::memory_order_release);
        s.release_receive();
    });

    /* 前置：recv 确实进入了阻塞（2s 超时下不该在 200ms 内返回）。没有这条，
     * 后面"叫醒成功"可能只是"recv 本来就没阻塞"的伪绿。 */
    std::this_thread::sleep_for(200ms);
    ASSERT_FALSE(recv_done.load(std::memory_order_acquire))
        << "recv 未进入阻塞 —— 本用例前提不成立（无发送方时 2s 超时不该提前返回）";

    const auto t_wake = Clock::now();
    s.stop_and_wake();
    recv_thread.join();
    const auto wake_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_wake).count();

    EXPECT_TRUE(recv_done.load()) << "stop_and_wake 之后 recv 必须已返回";
    /* ⛔ 不得断言「叫醒路径必须返回空 buffer」—— 该前提被 libipc 实测证伪:
     * recv() 被 disconnect()/quit_waiting() 叫醒时返回的是 ipc::data_length 字节的
     * **零填充** buffer(empty() == false); 只有**超时**路径才返回空 buffer。
     * 把「必须空」写进断言, 就把 libipc 的既有实现细节误当成 RouteSession 的契约,
     * 会把一个正确的实现判红(本用例首次运行即如此, 见本轮根因定位)。
     *
     * 承重判据改为「返回值里没有真实消息」: 本用例没有发送方 ⇒ 真消息不可能出现,
     * 故返回值只允许是「空」或「零填充伪影」。若将来有人把叫醒路径改成返回
     * 未初始化/陈旧字节, 这条会红 —— 那才是真正会把垃圾投递出去的回归。 */
    const bool empty = (recv_empty.load() == 1);
    const bool zero_filler =
        (recv_all_zero.load() == 1) &&
        (static_cast<std::size_t>(recv_size.load()) == ipc::data_length);
    EXPECT_TRUE(empty || zero_filler)
        << "叫醒路径返回了非零填充的负载(size=" << recv_size.load()
        << "), 疑似把叫醒伪影当成真消息(本用例无发送方, 不该有任何真实负载)";
    /* 数值边界：必须显著早于 recv 自己的 2000ms 超时 —— 否则说明是超时兜底而非叫醒。 */
    EXPECT_LT(wake_ms, 1000)
        << "stop_and_wake 后 recv 未及时返回（" << wake_ms << "ms，说明靠 2000ms 超时兜底而非被叫醒）";
    EXPECT_LT(recv_elapsed_ms.load(), 1500)
        << "recv 总耗时 " << recv_elapsed_ms.load() << "ms 接近 " << kRecvTimeoutMs
        << "ms 超时 ⇒ 未被 disconnect 叫醒（说明 §5 第 4 步被破坏）";
}

/* stop 之后 wait_quiescent 必须能在 inflight 归零时返回（析构路径的收尾动作）。 */
TEST(RouteSession, StopThenQuiesceReturns)
{
    RouteName rn{"stop_quiesce"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    auto lease = s.acquire_receive();
    ASSERT_TRUE(lease.has_value());

    s.stop_and_wake();

    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        s.wait_quiescent();
        returned.store(true, std::memory_order_release);
    });
    EXPECT_FALSE(wait_for([&] { return returned.load(std::memory_order_acquire); }, 200))
        << "inflight 未归零时 wait_quiescent 不得返回";

    s.release_receive();
    EXPECT_TRUE(wait_for([&] { return returned.load(std::memory_order_acquire); }, 1000))
        << "stop 之后 inflight 归零 ⇒ wait_quiescent 必须返回";
    waiter.join();
}

/* ---------------------------------------- stop 的幂等性与非终态（产品代码依赖） */
/* 头文件「接口语义裁定」声明 stop_and_wake **幂等**且**非终态**。这不是纸面约定：
 * src/dzIPC/shm_pub_sub_ipc.cc 里同一个 RouteSession 会在三处被重复 stop ——
 * 析构路径、add_peer 失败路径、控制面离开 Ready 路径；且 add_peer 失败路径是
 * `stop_and_wake() + wait_quiescent() + release(current_route())` 三步收尾，
 * **依赖 stop 之后 current_route() 仍返回旧 route**。若把 stop 实现成「清空 route_」
 * 的终态，那步收尾会拿到空指针 ⇒ 旧 route 的 release() 被静默跳过（句柄泄漏 +
 * 段可能被 clear_storage 后仍被 recv），而所有既有用例都不报警。 */
TEST(RouteSession, StopIsIdempotentAndNonTerminal)
{
    RouteName rn{"stop_idem"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));
    auto cur = s.current_route();
    ASSERT_TRUE(cur) << "用例构造异常：重建后必须有 route";

    s.stop_and_wake();
    EXPECT_FALSE(s.acquire_receive().has_value()) << "stop 之后必须拒绝新 lease（I3）";

    /* 非终态：stop 只 disconnect，不清 route_。 */
    EXPECT_EQ(s.current_route().get(), cur.get())
        << "stop_and_wake 不得清空 route_（产品代码在 stop 之后仍要 release 当前 route）";
    EXPECT_GE(cur.use_count(), 1) << "stop 不得让 route 对象被析构（析构只放自己那份）";

    /* 幂等：重复调用不抛、不改变可观测状态。 */
    EXPECT_NO_THROW(s.stop_and_wake()) << "stop_and_wake 必须 noexcept 且可重复调用";
    EXPECT_NO_THROW(s.stop_and_wake());
    EXPECT_EQ(s.current_route().get(), cur.get()) << "重复 stop 不得改变 route_";
    EXPECT_FALSE(s.acquire_receive().has_value()) << "重复 stop 之后仍必须拒绝新 lease";

    /* 非终态的另一面：stop 不是「锁死」，成功重建必须重新放行 lease。 */
    s.begin_rebuild(2, factory(rn));
    EXPECT_EQ(s.generation(), 2u);
    EXPECT_TRUE(s.acquire_receive().has_value()) << "stop 之后成功重建必须重新放行 lease";
    s.release_receive();
}

/* ------------------------------------------- 析构只放自己那份，不替调用方 release */
/* I1：lease 持有的 shared_ptr 让 route 在 recv 期间（乃至 RouteSession 析构之后）
 * 保活。析构 RouteSession 不得替调用方 release 对象。 */
TEST(RouteSession, DtorReleasesOnlyItsOwnShare)
{
    RouteName rn{"dtor"};
    std::shared_ptr<ipc::route> held;

    {
        RouteSession s;
        s.begin_rebuild(1, factory(rn));
        auto lease = s.acquire_receive();
        ASSERT_TRUE(lease.has_value());
        held = lease->route;
        s.release_receive();   /* 归零：析构前不留下 inflight */
        s.wait_quiescent();
    }

    EXPECT_TRUE(held) << "lease 的 shared_ptr 必须仍持有对象";
    EXPECT_GE(held.use_count(), 1) << "RouteSession 析构后 lease 的 shared_ptr 必须仍让对象存活（I1）";

    held->disconnect();
    held->release();
}

/* ---------------------------------------------------- 并发 churn：无数据竞争 */
/* 多线程反复 acquire/release 与 begin_rebuild/stop 交错。判据不是"没崩"，而是
 * 收尾时计数必须精确归零（漏配对/多配对都会让 wait_quiescent 挂死或有界等待失败）。 */
TEST(RouteSession, ConcurrentAcquireReleaseWithRebuildIsRaceFree)
{
    RouteName rn{"churn"};
    RouteSession s;
    s.begin_rebuild(1, factory(rn));

    std::atomic<bool> stop{false};
    std::atomic<long> acquired{0};
    std::atomic<long> rejected{0};

    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i)
    {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                auto lease = s.acquire_receive();
                if (!lease.has_value())
                {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    continue;
                }
                acquired.fetch_add(1, std::memory_order_relaxed);
                /* 模拟 recv 的最小临界段：lease 的 shared_ptr 在整段内保活。 */
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                s.release_receive();
            }
        });
    }

    std::thread rebuilder([&] {
        for (uint32_t g = 2; g <= 8 && !stop.load(std::memory_order_acquire); ++g)
        {
            s.begin_rebuild(g, factory(rn));
            std::this_thread::sleep_for(2ms);
        }
    });

    std::this_thread::sleep_for(300ms);
    stop.store(true, std::memory_order_release);
    rebuilder.join();
    for (auto& t : readers)
    {
        t.join();
    }

    /* 收尾：stop 后所有 reader 都已退出 ⇒ inflight 必须精确归零。若某个 reader 的
     * release_receive 漏了或多了，这里会永久挂死（由用例超时暴露）。 */
    s.stop_and_wake();
    std::atomic<bool> quiesced{false};
    std::thread waiter([&] {
        s.wait_quiescent();
        quiesced.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(wait_for([&] { return quiesced.load(std::memory_order_acquire); }, 2000))
        << "并发 churn 之后 wait_quiescent 未返回 ⇒ acquire/release 计数不配对";
    waiter.join();

    EXPECT_GT(acquired.load(), 0) << "并发用例必须真的拿到过 lease（否则是空跑伪绿）";
    EXPECT_GT(rejected.load(), 0) << "重建期间必须有被拒的 acquire（否则未覆盖 I3）";
    std::printf("[route-session] acquired=%ld rejected=%ld\n", acquired.load(), rejected.load());
}
