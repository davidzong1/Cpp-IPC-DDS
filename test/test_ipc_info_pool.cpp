#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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
    EXPECT_EQ(e->pid, static_cast<int32_t>(::getpid()));

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
    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        auto& child_pool = IpcInfoPool::instance();
        child_pool.register_entry({EntryKind::ShmPub, "ut_gc_deadchild", "", "", 0, "from-child"});
        ::_exit(0);
    }
    /* 等子进程退出 */
    int status = 0;
    ::waitpid(child, &status, 0);

    /* 在 gc 前应能看到子进程写入的那条 */
    bool present_before = false;
    for (const auto& e : pool.snapshot(false))
    {
        if (e.topic_name == "ut_gc_deadchild" && e.pid == child)
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
        if (e.topic_name == "ut_gc_deadchild" && e.pid == child)
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

}   // namespace
