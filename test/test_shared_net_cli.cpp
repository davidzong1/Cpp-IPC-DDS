#include "shared_net/public_fixture.h"
#include "shared_net/peer_stub.h"
#include "dzIPC/ipc_info_pool.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetCli, SummaryAndLocalPeerQueriesReflectInstalledState) {
    PublicFixture f; auto runtime = ClientRuntime::acquire(f.config.control_path);
    const auto missing = runtime->route_status(f.topic.descriptor.key);
    EXPECT_NE(missing.find("\"ready\":false"), std::string::npos);
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    ASSERT_TRUE(pub.publish(f.message()));
    EXPECT_NE(runtime->diagnostics_json().find("\"local_committed\":1"), std::string::npos);
    const auto route = runtime->route_status(f.topic.descriptor.key);
    EXPECT_NE(route.find("\"roles\":3"), std::string::npos);
    EXPECT_NE(route.find("\"topic\":\"" + f.topic.descriptor.topic + "\""), std::string::npos);
    const auto status = runtime->status(); EXPECT_LT(status.size(), 8000u);
    EXPECT_NE(status.find("\"logical_publishers\":1"), std::string::npos);
    EXPECT_NE(status.find("\"ready_subscribers\":1"), std::string::npos);
    EXPECT_NE(status.find("\"socket_buffers\":["), std::string::npos);
    EXPECT_NE(status.find("\"limits\":{"), std::string::npos);
    const auto unknown = runtime->route_status(f.topic.descriptor.key, identity(99));
    EXPECT_NE(unknown.find("\"source_verified\":false"), std::string::npos);
    EXPECT_NE(unknown.find("\"ready\":false"), std::string::npos);
}
TEST(SharedNetCli, LogicalRegistryContainsNoBridgeOrDuplicateLeg) {
    PublicFixture f;
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    unsigned count = 0;
    for (const auto& entry : dzIPC::info_pool::IpcInfoPool::instance().snapshot()) {
        if (entry.topic_name != f.topic.descriptor.topic) continue;
        ++count; EXPECT_NE(entry.extra.find("mode=shared_v1"), std::string::npos);
        EXPECT_TRUE(entry.kind == dzIPC::info_pool::EntryKind::SocketPub || entry.kind == dzIPC::info_pool::EntryKind::SocketSub);
    }
    EXPECT_EQ(count, 2u);
}
TEST(SharedNetCli, UnsupportedObjectSchedulingIsExplicit) {
    PublicFixture f;
    EXPECT_THROW((dzIPC::shared_net::Publisher(f.model(), f.topic.descriptor.topic, 0, false, true)), std::invalid_argument);
    EXPECT_THROW((dzIPC::shared_net::Subscriber(f.model(), f.topic.descriptor.topic, 0, 8, false, false, 0)), std::invalid_argument);
}
