/* dzipc_topic_cat 的传输选型单测。
 *
 * 背景: 工具原来取"IpcInfoPool 里第一条 topic 匹配的条目"就收工, 于是 Auto ser-cli
 * 切到 SHM 之后(socket/shm 四条并存, socket 腿 stop_data_plane() 不注销池登记且注册
 * 在先)确定性地选到那条**已经不发数据**的 socket 腿 —— 空转且零报错。
 *
 * 这里只测纯选型函数(不建 SHM/UDP/进程), 外加一条走**真实 IpcInfoPool** 的集成断言
 * (池的 slot 分配与快照顺序是缺陷的另一半, 不能只测人造数组)。
 */
#include "gtest/gtest.h"

#include <string>
#include <vector>

#include "dzIPC/ipc_info_pool.h"
#include "exec/dzipc_topic_cat/include/transport_select.h"

namespace {

using dzIPC::info_pool::EntryKind;
using dzIPC::info_pool::EntrySnapshot;
using dzipc_topic_cat::Selection;
using dzipc_topic_cat::TransportPreference;

using Pref = dzipc_topic_cat::TransportPreference;

EntrySnapshot mk(int32_t slot, EntryKind kind, const std::string& topic, int64_t ts, int32_t domain = 0,
                 bool alive = true, bool in_use = true)
{
    EntrySnapshot e;
    e.slot = slot;
    e.kind = kind;
    e.pid = 1000 + slot;
    e.register_ts_ns = ts;
    e.heartbeat_ns = ts;
    e.topic_name = topic;
    e.type_name = "Srv::RequestResponseTestRequest";
    e.domain_id = domain;
    e.in_use = in_use;
    e.alive = alive;
    return e;
}

/* Auto ser-cli 切到 SHM 之后的**实测**池形态(2026-09-15, dzipc_list 轮询
 * SerCliAutoPath.SameHostSwitchesToShm 期间): socket 腿在 slot 0/1(注册在先),
 * shm 腿在 slot 2/3。 */
std::vector<EntrySnapshot> auto_after_switch(const std::string& topic)
{
    return {
        mk(0, EntryKind::SocketServer, topic, 100),
        mk(1, EntryKind::SocketClient, topic, 200),
        mk(2, EntryKind::ShmServer, topic, 300),
        mk(3, EntryKind::ShmClient, topic, 400),
    };
}

}   // namespace

/* ------------------------------------------------------------------------- *
 * 1. 回归本体: 四条并存时必须选 SHM(而不是 slot 最小的 SocketServer)。
 * ------------------------------------------------------------------------- */
TEST(TopicCatTransportSelect, AutoPrefersShmOverEarlierSocketEntry)
{
    const std::string topic = "ut_select_auto";
    auto entries = auto_after_switch(topic);
    /* 缺陷的另一半: "取首条"必然命中 socket —— 先把这个前提钉住, 免得将来池顺序变了
     * 让本用例变成空转。 */
    ASSERT_EQ(entries.front().kind, EntryKind::SocketServer);

    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, /*ser_or_topic=*/true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_TRUE(sel.shm) << "四条并存时 auto 必须选 SHM 腿";
    EXPECT_EQ(sel.kind, EntryKind::ShmClient) << "同族取 register_ts_ns 最新(shm_client 最后注册)";
    EXPECT_EQ(sel.slot, 3);
    EXPECT_EQ(sel.domain_id, 0);
}

/* 2. 只有 socket 腿(切换前 / 纯 socket 会话): auto 仍走 socket, 旧用法不变。 */
TEST(TopicCatTransportSelect, AutoFallsBackToSocketWhenNoShmLeg)
{
    const std::string topic = "ut_select_socket_only";
    std::vector<EntrySnapshot> entries{
        mk(0, EntryKind::SocketServer, topic, 100),
        mk(1, EntryKind::SocketClient, topic, 200),
    };
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_FALSE(sel.shm);
    EXPECT_EQ(sel.kind, EntryKind::SocketClient);
}

/* 3. 显式 --transport socket: 即使 SHM 腿在, 也按用户说的挂 socket。 */
TEST(TopicCatTransportSelect, ExplicitSocketIgnoresShmLeg)
{
    const std::string topic = "ut_select_force_socket";
    auto entries = auto_after_switch(topic);
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Socket);
    ASSERT_TRUE(sel.found);
    EXPECT_FALSE(sel.shm);
    EXPECT_EQ(sel.slot, 1);   // 同族取最新 -> socket_client
}

/* 4. 显式 --transport shm: 有 SHM 腿就挂 SHM。 */
TEST(TopicCatTransportSelect, ExplicitShmPicksShmLeg)
{
    const std::string topic = "ut_select_force_shm";
    auto entries = auto_after_switch(topic);
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Shm);
    ASSERT_TRUE(sel.found);
    EXPECT_TRUE(sel.shm);
}

/* 5. 显式 --transport shm 但池里只有 socket: **不静默回退**, 返回未找到(调用方继续等)。
 *    静默回退正是"显示 SOCKET 却以为在看 SHM"的来源。 */
TEST(TopicCatTransportSelect, ExplicitShmNeverFallsBackToSocket)
{
    const std::string topic = "ut_select_shm_absent";
    std::vector<EntrySnapshot> entries{mk(0, EntryKind::SocketServer, topic, 100)};
    EXPECT_FALSE(dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Shm).found);
    EXPECT_TRUE(dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Socket).found);
}

/* 6. 死条目/未启用条目跳过: 首条死、次条活时**不该**整体断开(旧实现会), 也不该选中死的。 */
TEST(TopicCatTransportSelect, DeadAndUnusedEntriesAreSkipped)
{
    const std::string topic = "ut_select_dead";
    std::vector<EntrySnapshot> entries{
        mk(0, EntryKind::SocketServer, topic, 100, 0, /*alive=*/false),
        mk(1, EntryKind::SocketClient, topic, 200, 0, true, /*in_use=*/false),
        mk(2, EntryKind::ShmServer, topic, 300),
    };
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_TRUE(sel.shm);
    EXPECT_EQ(sel.slot, 2);

    /* 全是死条目 => 未找到(调用方据此断开并回到等待态)。 */
    std::vector<EntrySnapshot> dead{mk(0, EntryKind::ShmServer, topic, 100, 0, false)};
    EXPECT_FALSE(dzipc_topic_cat::select_sniffer_entry(dead, topic, true, Pref::Auto).found);
}

/* 7. kind 家族要与嗅探模式匹配: service 模式不吃 pub/sub 条目, 反之亦然。 */
TEST(TopicCatTransportSelect, KindFamilyMustMatchSniffMode)
{
    const std::string topic = "ut_select_kind";
    std::vector<EntrySnapshot> pubsub{
        mk(0, EntryKind::SocketPub, topic, 100),
        mk(1, EntryKind::ShmPub, topic, 200),
    };
    EXPECT_FALSE(dzipc_topic_cat::select_sniffer_entry(pubsub, topic, /*ser_or_topic=*/true, Pref::Auto).found);
    const Selection as_topic = dzipc_topic_cat::select_sniffer_entry(pubsub, topic, false, Pref::Auto);
    ASSERT_TRUE(as_topic.found);
    EXPECT_TRUE(as_topic.shm);

    std::vector<EntrySnapshot> sercli{mk(0, EntryKind::ShmServer, topic, 100)};
    EXPECT_FALSE(dzipc_topic_cat::select_sniffer_entry(sercli, topic, false, Pref::Auto).found);
    EXPECT_TRUE(dzipc_topic_cat::select_sniffer_entry(sercli, topic, true, Pref::Auto).found);
}

/* 8. 别的 topic 的条目不能参与选型(名字相同前缀也不行)。 */
TEST(TopicCatTransportSelect, OtherTopicsAreIgnored)
{
    std::vector<EntrySnapshot> entries{
        mk(0, EntryKind::ShmServer, "ut_select_other_1", 100),
        mk(1, EntryKind::ShmServer, "ut_select_other_1_suffix", 200),
    };
    EXPECT_FALSE(dzipc_topic_cat::select_sniffer_entry(entries, "ut_select_other_1", true, Pref::Socket).found);
    EXPECT_TRUE(dzipc_topic_cat::select_sniffer_entry(entries, "ut_select_other_1", true, Pref::Shm).found);
}

/* 9. 同传输内的并列规则: 取 register_ts_ns 最新; 时间相同取 slot 小者(确定性)。 */
TEST(TopicCatTransportSelect, NewestRegistrationWinsWithinSameTransport)
{
    const std::string topic = "ut_select_tiebreak";
    std::vector<EntrySnapshot> entries{
        mk(5, EntryKind::SocketServer, topic, 700),
        mk(2, EntryKind::SocketClient, topic, 900),
        mk(9, EntryKind::SocketServer, topic, 900),
    };
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_EQ(sel.slot, 2) << "同为 900ns 时取 slot 小者";
}

/* 10. domain_id 跟着被选中的条目走(sniffer 用它算段名/端口)。 */
TEST(TopicCatTransportSelect, SelectedDomainIdComesFromChosenEntry)
{
    const std::string topic = "ut_select_domain";
    std::vector<EntrySnapshot> entries{
        mk(0, EntryKind::SocketServer, topic, 100, /*domain=*/7),
        mk(1, EntryKind::ShmServer, topic, 200, /*domain=*/9),
    };
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_EQ(sel.domain_id, 9);
}

/* 11. same_channel: 决定"要不要重建 sniffer"。
 *     socket->shm 必须重建; 同传输换进程(slot/pid 变)不必重建; 换 domain 要重建。 */
TEST(TopicCatTransportSelect, SameChannelDrivesRebuildDecision)
{
    const std::string topic = "ut_select_channel";
    auto entries = auto_after_switch(topic);
    const Selection socket_leg = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Socket);
    const Selection shm_leg = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Shm);
    ASSERT_TRUE(socket_leg.found);
    ASSERT_TRUE(shm_leg.found);
    EXPECT_FALSE(socket_leg.same_channel(shm_leg)) << "切换传输必须触发重建";
    EXPECT_TRUE(shm_leg.same_channel(shm_leg));

    EntrySnapshot other_pid = entries[2];
    other_pid.slot = 17;
    other_pid.pid = 4242;
    other_pid.register_ts_ns = 999;
    const Selection moved = dzipc_topic_cat::select_sniffer_entry({other_pid}, topic, true, Pref::Shm);
    ASSERT_TRUE(moved.found);
    EXPECT_TRUE(moved.same_channel(shm_leg)) << "同传输同 domain 换进程不需重建";

    EntrySnapshot other_domain = entries[2];
    other_domain.domain_id = 5;
    const Selection dom = dzipc_topic_cat::select_sniffer_entry({other_domain}, topic, true, Pref::Shm);
    ASSERT_TRUE(dom.found);
    EXPECT_FALSE(dom.same_channel(shm_leg)) << "domain 变了通道就变了";
}

/* 12. CLI 取值解析: 只认 auto/shm/socket, 非法值必须报错(不静默回退到 auto)。 */
TEST(TopicCatTransportSelect, ParseTransportPreferenceIsStrict)
{
    TransportPreference p{};
    EXPECT_TRUE(dzipc_topic_cat::parse_transport_preference("auto", p));
    EXPECT_EQ(p, Pref::Auto);
    EXPECT_TRUE(dzipc_topic_cat::parse_transport_preference("shm", p));
    EXPECT_EQ(p, Pref::Shm);
    EXPECT_TRUE(dzipc_topic_cat::parse_transport_preference("socket", p));
    EXPECT_EQ(p, Pref::Socket);
    EXPECT_FALSE(dzipc_topic_cat::parse_transport_preference("SHM", p));
    EXPECT_FALSE(dzipc_topic_cat::parse_transport_preference("udp", p));
    EXPECT_FALSE(dzipc_topic_cat::parse_transport_preference("", p));
    EXPECT_STREQ(dzipc_topic_cat::to_string(Pref::Auto), "auto");
    EXPECT_STREQ(dzipc_topic_cat::to_string(Pref::Shm), "shm");
    EXPECT_STREQ(dzipc_topic_cat::to_string(Pref::Socket), "socket");
}

/* ------------------------------------------------------------------------- *
 * 13. 走真实 IpcInfoPool 的集成断言:
 *     池按"首个空槽"分配 slot、快照按 slot 升序返回 —— 缺陷的物理成因。
 *     socket 先注册(占低 slot), shm 后注册(占高 slot); 选型必须选 shm。
 * ------------------------------------------------------------------------- */
TEST(TopicCatTransportSelect, RealPoolPrefersLaterRegisteredShmLeg)
{
    using dzIPC::info_pool::IpcInfoPool;
    using dzIPC::info_pool::RegisterInfo;

    const std::string topic = "ut_select_real_pool";
    auto& pool = IpcInfoPool::instance();
    const int32_t socket_slot =
        pool.register_entry(RegisterInfo{EntryKind::SocketServer, topic, "T", "socket", 0, "ut"});
    const int32_t shm_slot = pool.register_entry(RegisterInfo{EntryKind::ShmServer, topic, "T", "shm", 0, "ut"});
    ASSERT_GE(socket_slot, 0);
    ASSERT_GE(shm_slot, 0);
    ASSERT_LT(socket_slot, shm_slot) << "先注册的必须拿到更小的 slot(缺陷前提)";

    const auto entries = pool.snapshot(false);
    const Selection sel = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Auto);
    ASSERT_TRUE(sel.found);
    EXPECT_TRUE(sel.shm) << "真实池顺序下 auto 也必须选 SHM";
    EXPECT_EQ(sel.slot, shm_slot);
    EXPECT_EQ(sel.kind, EntryKind::ShmServer);

    /* 显式 socket 是逃生口: 同一份快照下能按用户要求挂回那条腿。 */
    const Selection forced = dzipc_topic_cat::select_sniffer_entry(entries, topic, true, Pref::Socket);
    ASSERT_TRUE(forced.found);
    EXPECT_EQ(forced.slot, socket_slot);

    pool.unregister_entry(socket_slot);
    pool.unregister_entry(shm_slot);
}
