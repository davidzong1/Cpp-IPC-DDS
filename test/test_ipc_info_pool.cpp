#if defined(__linux__) || defined(__QNX__)
#    include <signal.h>
#    include <sys/wait.h>
#    include <unistd.h>
#elif defined(_WIN32)
#    include <process.h>
#    include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "dzIPC/ipc_info_pool.h"
#include "test.h"

namespace {
using namespace dzIPC::info_pool;

bool has_slot(const std::vector<EntrySnapshot>& es, int32_t slot)
{
    return std::any_of(es.begin(), es.end(), [&](const EntrySnapshot& e) { return e.slot == slot; });
}

const EntrySnapshot* find_slot(const std::vector<EntrySnapshot>& es, int32_t slot)
{
    for (const auto& e : es)
        if (e.slot == slot)
            return &e;
    return nullptr;
}

#if defined(__linux__) || defined(__QNX__)
/* ---- UF-006(表满路径回收)用例的脚手架 ---- */

/* 占满整表的那些槽, 析构时逐个注销 ⇒ 断言提前 return 也不会污染后续用例 */
struct SlotReleaser
{
    std::vector<int32_t> slots;
    ~SlotReleaser()
    {
        for (std::size_t i = slots.size(); i-- > 0;)
            IpcInfoPool::instance().unregister_entry(slots[i]);
    }
};

/* 填满整表; 返回**终止那次失败调用**的返回码(表满时应为 -1) —— 也就是第 kMaxEntries+1 次
 * register_entry 的返回码, 判据①就断言在它身上。
 * ⚠️ 返回 -1 同时**建立了一个前提**: 表满那次尝试里的整表回收没找到死条目
 * (找到了就会腾出槽、循环继续) ⇒ 返回 -1 之后, 表里**没有**死条目。
 * 兜底上限 kMaxEntries+8 只是防御: 池容量就是 kMaxEntries。 */
int32_t fill_table(SlotReleaser& guard, const char* topic)
{
    int32_t s = -1;
    while (guard.slots.size() < kMaxEntries + 8)
    {
        s = IpcInfoPool::instance().register_entry({EntryKind::SocketPub, topic, "", "", 0, ""});
        if (s < 0)
            return s;
        guard.slots.push_back(s);
    }
    return s;
}

/* 子进程收尸: SIGKILL + waitpid; 断言提前 return 时也要保证不遗留孤儿/僵尸 */
struct ChildReaper
{
    ::pid_t pid{-1};
    ~ChildReaper() { kill_and_wait(SIGKILL); }

    void kill_and_wait(int sig) noexcept
    {
        if (pid <= 0)
            return;
        ::kill(pid, sig);
        int st = 0;
        ::waitpid(pid, &st, 0);
        pid = -1;
    }
};

/* 表里是否还有 pid == p 的条目(快照口径, 不回收) */
bool entry_present(::pid_t p)
{
    for (const auto& e : IpcInfoPool::instance().snapshot(false))
        if (e.pid == p)
            return true;
    return false;
}

/* 逐字段比对两次 snapshot —— 这是“不改 gc_dead=false 语义 / snapshot(false) 判定路径”
 * 的**可执行定义**。比 size() 严格: 条目有任何增删改(含 heartbeat 抖动)都会被抓到。 */
bool same_snapshot(const std::vector<EntrySnapshot>& a, const std::vector<EntrySnapshot>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const EntrySnapshot& x = a[i];
        const EntrySnapshot& y = b[i];
        if (x.slot != y.slot || x.kind != y.kind || x.pid != y.pid || x.domain_id != y.domain_id ||
            x.in_use != y.in_use || x.alive != y.alive || x.register_ts_ns != y.register_ts_ns ||
            x.heartbeat_ns != y.heartbeat_ns || x.topic_name != y.topic_name ||
            x.type_name != y.type_name || x.extra != y.extra)
            return false;
    }
    return true;
}

/* 把 stderr(fd 2) 临时改道到临时文件, 用来断言“表满失败不再静默”(判据②)。
 * dup2 是进程级的 ⇒ 只在**单线程**断言段内使用, 取文本时立刻恢复。 */
struct StderrCapture
{
    int saved{-1};
    int fd{-1};
    char path[96]{};

    StderrCapture()
    {
        std::snprintf(path, sizeof(path), "/tmp/uf006_stderr_%d_XXXXXX", static_cast<int>(::getpid()));
        fd = ::mkstemp(path);
        if (fd < 0)
        {
            path[0] = '\0';
            return;
        }
        saved = ::dup(2);
        if (saved < 0)
        {
            ::close(fd);
            fd = -1;
            return;
        }
        if (::dup2(fd, 2) < 0)
        {
            ::close(saved);
            ::close(fd);
            saved = -1;
            fd = -1;
        }
    }

    ~StderrCapture()
    {
        stop();
        if (fd >= 0)
            ::close(fd);
        if (path[0] != '\0')
            ::unlink(path);
    }

    void stop() noexcept
    {
        if (saved < 0)
            return;
        std::fflush(nullptr);   /* 先把 stdio 缓冲里的 stderr 冲进文件 */
        ::dup2(saved, 2);
        ::close(saved);
        saved = -1;
    }

    /* 取回捕获到的文本(内部先 stop, 保证缓冲已落盘) */
    std::string text()
    {
        stop();
        std::string out;
        if (fd < 0)
            return out;
        char buf[512];
        ssize_t n = 0;
        while ((n = ::pread(fd, buf, sizeof(buf), static_cast<off_t>(out.size()))) > 0)
            out.append(buf, static_cast<std::size_t>(n));
        return out;
    }
};
#endif

TEST(IpcInfoPool, RegisterThenSnapshotReturnsEntry)
{
    auto& pool = IpcInfoPool::instance();
    RegisterInfo info{EntryKind::ShmPub, "ut_topic_register", "TestMsgType", "", 0, "extra=foo"};
    int32_t slot = pool.register_entry(info);
    ASSERT_GE(slot, 0);

    auto es = pool.snapshot(false);
    const EntrySnapshot* e = find_slot(es, slot);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->kind, EntryKind::ShmPub);
    EXPECT_EQ(e->topic_name, "ut_topic_register");
    EXPECT_EQ(e->type_name, "TestMsgType");
    EXPECT_EQ(e->extra, "extra=foo");
#if defined(_WIN32)
    const int32_t self_pid = static_cast<int32_t>(::_getpid());
#else
    const int32_t self_pid = static_cast<int32_t>(::getpid());
#endif
    EXPECT_EQ(e->pid, self_pid);

    pool.unregister_entry(slot);
    auto es2 = pool.snapshot(false);
    EXPECT_FALSE(has_slot(es2, slot));
}

TEST(IpcInfoPool, ScopedRegistrationAutoReleases)
{
    int32_t slot = -1;
    {
        ScopedRegistration reg({EntryKind::SocketSub, "ut_scoped", "T", "", 0, ""});
        ASSERT_TRUE(reg.valid());
        slot = reg.slot();
        auto es = IpcInfoPool::instance().snapshot(false);
        EXPECT_TRUE(has_slot(es, slot));
    }
    auto es2 = IpcInfoPool::instance().snapshot(false);
    EXPECT_FALSE(has_slot(es2, slot));
}

TEST(IpcInfoPool, RebindReleasesOldSlot)
{
    ScopedRegistration reg({EntryKind::ShmPub, "ut_rebind_a", "", "", 0, "first"});
    int32_t slot = reg.slot();
    ASSERT_GE(slot, 0);

    reg.rebind({EntryKind::SocketPub, "ut_rebind_b", "", "", 0, "second"});
    int32_t slot_after = reg.slot();
    ASSERT_GE(slot_after, 0);

    auto es = IpcInfoPool::instance().snapshot(false);
    const EntrySnapshot* e = find_slot(es, slot_after);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->kind, EntryKind::SocketPub);
    EXPECT_EQ(e->topic_name, "ut_rebind_b");
    EXPECT_EQ(e->extra, "second");
}

TEST(IpcInfoPool, ScopedRegistrationMoveTransfersOwnership)
{
    ScopedRegistration a({EntryKind::ShmServer, "ut_move_src", "", "", 0, ""});
    ASSERT_TRUE(a.valid());
    int32_t slot = a.slot();

    ScopedRegistration b(std::move(a));
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.slot(), slot);

    auto es = IpcInfoPool::instance().snapshot(false);
    EXPECT_TRUE(has_slot(es, slot));
    b.reset();
    auto es2 = IpcInfoPool::instance().snapshot(false);
    EXPECT_FALSE(has_slot(es2, slot));
}

TEST(IpcInfoPool, GcReapsEntriesOfDeadChildProcess)
{
    /* 子进程注册后直接退出，父进程 gc_dead 应将其回收 */
    auto& pool = IpcInfoPool::instance();
#if defined(__linux__) || defined(__QNX__)
    pid_t child = ::fork();
    if (child == 0)
    {
        auto& child_pool = IpcInfoPool::instance();
        child_pool.register_entry({EntryKind::ShmPub, "ut_gc_deadchild", "", "", 0, "from-child"});
        ::_exit(0);
    }
#elif defined(_WIN32)
    intptr_t child_handle_raw = _spawnl(_P_NOWAIT, "child_process.exe", "child_process.exe", nullptr);
    ASSERT_NE(child_handle_raw, static_cast<intptr_t>(-1));
    HANDLE child_handle = reinterpret_cast<HANDLE>(child_handle_raw);
    DWORD child_pid = ::GetProcessId(child_handle);
    ASSERT_NE(child_pid, 0u);
#endif
#if defined(__linux__) || defined(__QNX__)
    ASSERT_GE(child, 0);
#endif
    /* 等子进程退出 */
    int status = 0;
#if defined(__linux__) || defined(__QNX__)
    ::waitpid(child, &status, 0);
#elif defined(_WIN32)
    WaitForSingleObject(child_handle, INFINITE);
    ::CloseHandle(child_handle);
#endif
    /* 在 gc 前应能看到子进程写入的那条 */
    bool present_before = false;
    for (const auto& e : pool.snapshot(false))
    {
#if defined(_WIN32)
        if (e.topic_name == "ut_gc_deadchild" && e.pid == static_cast<int32_t>(child_pid))
#else
        if (e.topic_name == "ut_gc_deadchild" && e.pid == child)
#endif
        {
            present_before = true;
            break;
        }
    }
    EXPECT_TRUE(present_before);

    std::size_t reaped = pool.gc_dead();
    EXPECT_GE(reaped, static_cast<std::size_t>(1));

    bool present_after = false;
    for (const auto& e : pool.snapshot(false))
    {
#if defined(_WIN32)
        if (e.topic_name == "ut_gc_deadchild" && e.pid == static_cast<int32_t>(child_pid))
#else
        if (e.topic_name == "ut_gc_deadchild" && e.pid == child)
#endif
        {
            present_after = true;
            break;
        }
    }
    EXPECT_FALSE(present_after);
}

TEST(IpcInfoPool, HeartbeatUpdatesTimestamp)
{
    ScopedRegistration reg({EntryKind::ShmSub, "ut_hb", "", "", 0, ""});
    ASSERT_TRUE(reg.valid());

    int64_t ts_before = 0;
    for (const auto& e : IpcInfoPool::instance().snapshot(false))
    {
        if (e.slot == reg.slot())
        {
            ts_before = e.heartbeat_ns;
            break;
        }
    }
    ASSERT_GT(ts_before, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reg.heartbeat();

    int64_t ts_after = 0;
    for (const auto& e : IpcInfoPool::instance().snapshot(false))
    {
        if (e.slot == reg.slot())
        {
            ts_after = e.heartbeat_ns;
            break;
        }
    }
    EXPECT_GT(ts_after, ts_before);
}

TEST(IpcInfoPool, ToStringCoversAllKinds)
{
    EXPECT_STREQ(to_string(EntryKind::ShmPub), "shm_pub");
    EXPECT_STREQ(to_string(EntryKind::ShmSub), "shm_sub");
    EXPECT_STREQ(to_string(EntryKind::SocketPub), "socket_pub");
    EXPECT_STREQ(to_string(EntryKind::SocketSub), "socket_sub");
    EXPECT_STREQ(to_string(EntryKind::ShmServer), "shm_server");
    EXPECT_STREQ(to_string(EntryKind::ShmClient), "shm_client");
    EXPECT_STREQ(to_string(EntryKind::SocketServer), "socket_server");
    EXPECT_STREQ(to_string(EntryKind::SocketClient), "socket_client");
    EXPECT_STREQ(to_string(EntryKind::Unknown), "unknown");
}

#if defined(__linux__) || defined(__QNX__)
/* UF-006 核心判据: 表满时 **下一次注册** 必须自己回收死进程留下的条目,
 * 而不是只能靠外部显式调 gc_dead()/snapshot(true) —— 后者在产品码里零调用点。
 * 回滚变异(去掉满载路径的回收) ⇒ 本用例必红: 注册恒返 -1。 */
TEST(IpcInfoPool, FullTableRegisterReapsDeadChildEntry)
{
    auto& pool = IpcInfoPool::instance();

    /* 0) 基线前提: 池段名固定 ⇒ 它是**全机共享**的, 历史 run / 其它进程可能留下死条目甚至把表占满。
       gc_dead() 只清 pid 已死的条目(不碰任何活着的进程), 且处在“被测路径”之外 ⇒ 不构成承重,
       只让本用例起点干净、各组实验前提一致。 */
    pool.gc_dead();

    int sync_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(sync_pipe), 0);

    ChildReaper reaper;
    ::pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(sync_pipe[0]);
        int32_t s = IpcInfoPool::instance().register_entry(
            {EntryKind::ShmPub, "ut_uf006_deadchild", "", "", 0, ""});
        const char ok = (s >= 0) ? char(1) : char(0);
        ssize_t w = ::write(sync_pipe[1], &ok, 1);
        (void)w;
        /* 等父进程先填满表、再把我 SIGKILL —— 这样留下的正是“in_use=1 + 死 pid”
           的形态(与缺陷成因一致)。兜底 15s 自杀, 父进程异常时不留永久孤儿。 */
        for (int i = 0; i < 150; ++i)
            ::usleep(100 * 1000);
        ::_exit(0);
    }
    reaper.pid = child;
    ::close(sync_pipe[1]);
    char ok = 0;
    const ssize_t got = ::read(sync_pipe[0], &ok, 1);
    ::close(sync_pipe[0]);
    ASSERT_EQ(got, static_cast<ssize_t>(1));
    ASSERT_EQ(ok, char(1)) << "子进程未能注册(表可能已被本机其它进程占满)";

    /* 1) 先占满整表。fill_table 返回的正是**第 kMaxEntries+1 次**注册的返回码 ⇒
       这里一并断言“表满 + 无死条目 ⇒ 仍返回 -1”(返回码未改), 并建立“表里没有死条目”的前提。 */
    SlotReleaser guard;
    ASSERT_EQ(fill_table(guard, "ut_uf006_filler"), -1) << "未能填到表满: 槽数=" << guard.slots.size();
    ASSERT_FALSE(guard.slots.empty());

    /* 2) 杀子进程 ⇒ 表里留下一条 in_use=1 且 pid 已死的条目 */
    reaper.kill_and_wait(SIGKILL);
    bool dead_present = false;
    for (const auto& e : pool.snapshot(false))
    {
        if (e.pid == child)
        {
            dead_present = true;
            EXPECT_FALSE(e.alive) << "子进程已退出, 该条目必须被判定为不存活";
            break;
        }
    }
    ASSERT_TRUE(dead_present) << "前提不成立: 死子进程的条目不在表里";

    /* 3) 满载路径必须回收它并让注册成功(改动前恒为 -1)。
       空扫节流窗 250ms 由第 1 步填表的终止尝试上过闸 ⇒ 先等过窗口, 让首次尝试就落在
       “可以扫”的一侧; 有界重试只是给节流与调度留余量, 不是“撞运气”。
       ⛔ 共享池归因哨兵: 每次尝试**之前**表必须仍是满的(kMaxEntries 条)。否则说明有外部进程
          释放了槽或回收了死条目 —— 那样即使注册成功也不是我这版代码的功劳(实测踩过: 同机
          另一个 test_ipc_info_pool 实例并发跑, 它退出时释放自己的槽 ⇒ 恒返 -1 的旧码也“成功”)。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int32_t slot = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (slot < 0 && std::chrono::steady_clock::now() < deadline)
    {
        ASSERT_EQ(pool.snapshot(false).size(), kMaxEntries)
            << "共享池被外部进程改动(表不再满) ⇒ 成功无法归因于被测代码, 本用例不成立, 请重跑";
        slot = pool.register_entry({EntryKind::SocketSub, "ut_uf006_after_reap", "", "", 0, ""});
        if (slot < 0)
            ::usleep(60 * 1000);
    }
    if (slot >= 0)
        guard.slots.push_back(slot);
    EXPECT_GE(slot, 0) << "表满 + 有死条目时, 注册必须回收死条目并成功(改动前恒 -1)";

    /* 4) 死条目必须真的没了。按 pid 判, 不按槽号 —— 空出来的槽很可能已被第 3 步复用 */
    EXPECT_FALSE(entry_present(child)) << "死子进程的条目仍在表中 ⇒ 满载路径未回收它";
}

/* UF-006 反向判据(tester §1a 口径, 恰好三条): 满载失败路径上必须
 *   ① 第 kMaxEntries+1 次 register_entry **仍返回 -1** —— 返回码契约不变;
 *   ② 该次失败**不再静默** —— 出现可观测诊断行(这条就是回滚变异的靶点);
 *   ③ 两次 snapshot(false) **逐字段完全相同** —— 不改 gc_dead=false 语义/判定路径。
 * 前提(由 fill_table 的终止条件建立): 表满且表里**没有**死条目。 */
TEST(IpcInfoPool, FullTableFailureStaysObservableAndLeavesSnapshotIntact)
{
    auto& pool = IpcInfoPool::instance();

    /* 先等过诊断限流窗(1 行/秒/原因): 让下面那一次满载失败的诊断**必然**输出 ⇒
       本用例不依赖“同二进制内其它用例碰巧没占用限流窗”, 结论可复现。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    SlotReleaser guard;
    StderrCapture cap;   /* 从填表就开始捕获: 终止那次(即第 513 次)注册的诊断也在其中 */
    ASSERT_EQ(fill_table(guard, "ut_uf006_filler2"), -1) << "未能填到表满: 槽数=" << guard.slots.size();
    ASSERT_FALSE(guard.slots.empty());

    const std::vector<EntrySnapshot> before = pool.snapshot(false);
    const int32_t slot = pool.register_entry({EntryKind::SocketSub, "ut_uf006_no_dead", "", "", 0, ""});
    const std::string log = cap.text();

    /* ① 返回码不变 */
    EXPECT_LT(slot, 0) << "表满且无死条目时, 返回码必须仍是 -1";
    /* ② 可观测: 满载失败不得静默(去掉诊断调用 ⇒ 本断言转红) */
    EXPECT_NE(log.find("info_pool"), std::string::npos)
        << "表满失败必须留下可观测诊断(不得静默); 实测 stderr=[" << log << "]";
    /* ③ 判定路径零副作用: 逐字段相同 */
    const std::vector<EntrySnapshot> after = pool.snapshot(false);
    EXPECT_TRUE(same_snapshot(before, after)) << "snapshot(false) 在满载失败路径上必须逐字段不变(before="
                                              << before.size() << " after=" << after.size() << ")";
    EXPECT_TRUE(has_slot(after, guard.slots.front())) << "自己刚占的槽不得被回收";
}

/* UF-006 差异面的两条边(白名单核对): snapshot(false) **不回收**死条目(读路径无写副作用 ——
 * 这正是 logger endpoint-meta 看到的口径, 见证据 §7), snapshot(true) **仍然回收**。 */
TEST(IpcInfoPool, SnapshotFalseKeepsDeadEntryWhileSnapshotTrueStillReaps)
{
    auto& pool = IpcInfoPool::instance();

    int sync_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(sync_pipe), 0);
    ChildReaper reaper;
    ::pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(sync_pipe[0]);
        const char ok = (IpcInfoPool::instance().register_entry(
                             {EntryKind::ShmSub, "ut_uf006_snapchild", "", "", 0, ""}) >= 0)
                            ? char(1)
                            : char(0);
        ssize_t w = ::write(sync_pipe[1], &ok, 1);
        (void)w;
        ::_exit(0);
    }
    reaper.pid = child;
    ::close(sync_pipe[1]);
    char ok = 0;
    const ssize_t got = ::read(sync_pipe[0], &ok, 1);
    ::close(sync_pipe[0]);
    ASSERT_EQ(got, static_cast<ssize_t>(1));
    ASSERT_EQ(ok, char(1)) << "子进程未能注册(表可能已被本机其它进程占满)";
    reaper.kill_and_wait(SIGKILL);

    bool present = false;
    for (const auto& e : pool.snapshot(false))   /* ⛔ 这一步不得回收 */
    {
        if (e.pid == child)
        {
            present = true;
            EXPECT_TRUE(e.in_use) << "死条目仍应带着 in_use=1 出现(logger 看到的就是这个口径)";
            EXPECT_FALSE(e.alive);
            break;
        }
    }
    EXPECT_TRUE(present) << "snapshot(false) 不得回收死条目(gc_dead=false 语义)";

    bool gone = true;
    for (const auto& e : pool.snapshot(true))    /* true 必须仍回收 */
    {
        if (e.pid == child)
        {
            gone = false;
            break;
        }
    }
    EXPECT_TRUE(gone) << "snapshot(true) 必须仍然回收死条目";
}
#endif

}   // namespace
