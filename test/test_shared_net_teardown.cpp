#include "shared_net/public_fixture.h"
#include "shared_net/reassembly_fixture.h"
#include "../src/dzIPC/net/gateway_data.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
namespace {
std::size_t threads() { return std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}); }
}
TEST(SharedNetTeardown, OneHundredEndpointLifetimesReturnActiveResources) {
    PublicFixture f;
    auto cycle = [&] {
        dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
        dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
        EXPECT_TRUE(pub.publish(f.message())); dzIPC::Sample sample; EXPECT_TRUE(sub.get(sample, 1000));
    };
    auto runtime = ClientRuntime::acquire(f.config.control_path); cycle();
    ASSERT_TRUE(until([&] { return runtime->status().find("\"registered_handles\":0") != std::string::npos; }));
    const auto fds = fd_count(), tasks = threads();
    auto topic_shm = [&] {
        std::map<std::string, std::uintmax_t> files;
        for (const auto& file : std::filesystem::directory_iterator("/dev/shm"))
            if (file.path().filename().string().find(f.topic.descriptor.topic) != std::string::npos)
                files.emplace(file.path().filename().string(), file.file_size());
        return files;
    };
    const auto segments = topic_shm();
    for (unsigned i = 0; i < 100; ++i) {
        cycle();
        ASSERT_TRUE(until([&] { return runtime->status().find("\"registered_handles\":0") != std::string::npos; }));
        EXPECT_LE(fd_count(), fds); EXPECT_LE(threads(), tasks); EXPECT_EQ(topic_shm(), segments);
    }
    f.gateway->stop(); const auto status = f.gateway->status_json();
    EXPECT_NE(status.find("\"allocated_send_bytes\":\"0\""), std::string::npos);
    EXPECT_NE(status.find("\"reassembly_bytes\":0"), std::string::npos);
    EXPECT_NE(status.find("\"target_states\":0"), std::string::npos);
}
TEST(SharedNetTeardown, StoppingLoadedShardReleasesReceiveBudgetBeforeDestruction) {
    ReassemblyFixture f; Directory dir; auto config = configuration(dir); config.data_shards = 1; config.limits.reassembly_bytes = 1024 * 1024;
    auto endpoint = std::make_unique<DatagramEndpoint>(Ipv4Address::parse("127.0.0.1", 0));
    const auto destination = endpoint->local_address(); DatagramEndpoint sender{Ipv4Address::parse("127.0.0.1", 0)};
    f.admission.peer->hello.data_shards = 1; f.admission.peer->hello.data_base_port = sender.local_address().port;
    auto view = std::make_shared<GatewayDataView>(); auto local = std::make_shared<DirectorySnapshot>(), remote = std::make_shared<DirectorySnapshot>();
    local->routes.emplace(f.admission.subscriber->descriptor.key, f.admission.subscriber);
    remote->routes.emplace(f.admission.publisher->descriptor.key, f.admission.publisher);
    view->local = local; view->bridges = std::make_shared<GatewayDataView::Bridges>();
    PeerView peer; peer.admission = f.admission.peer; peer.snapshot = remote;
    view->peers.emplace(peer.admission->hello.gateway_id, peer);
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints; endpoints.push_back(std::move(endpoint)); auto wake = local::event();
    GatewayData data(config, f.local, 3, wake.get(), std::move(endpoints)); data.synchronize(view).get();
    const auto packet = f.packets(flat_blob(1024 * 1024)).front(); ASSERT_EQ(sender.send(packet.view(), destination).status, IoStatus::Data);
    ASSERT_TRUE(until([&] { return data.stats().receive_usage.bytes == 1024 * 1024; }));
    data.stop(); EXPECT_EQ(data.stats().receive_usage.bytes, 0u); EXPECT_EQ(data.stats().receive_usage.streams, 0u);
    EXPECT_FALSE(data.control(packet)); EXPECT_FALSE(data.healthy());
}

TEST(SharedNetTeardown, FailedTargetPreparationReturnsAllClaims) {
    Directory dir; auto config = configuration(dir); config.data_shards = 1; config.limits.publisher_reliable = 1;
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
    endpoints.push_back(std::make_unique<DatagramEndpoint>(Ipv4Address::parse("127.0.0.1", 0)));
    auto wake = local::event(); Identity id{}; id[0] = 1;
    GatewayData data(config, id, 1, wake.get(), std::move(endpoints));
    OutboxRecord record; record.header.delivery = Delivery::Reliable; record.header.publisher_id[0] = 1;
    PeerView peer; peer.snapshot = std::make_shared<DirectorySnapshot>();
    for (unsigned i = 0; i < 100; ++i) {
        EXPECT_THROW(data.submit(record, {peer}), std::out_of_range);
        EXPECT_EQ(data.stats().target_states, 0u);
    }
    auto stopping = std::async(std::launch::async, [&] { data.stop(); });
    for (unsigned i = 0; i < 100; ++i) data.submit(record, {});
    stopping.get(); EXPECT_EQ(data.stats().target_states, 0u);
}
