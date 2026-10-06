#include "shared_net/public_fixture.h"
#include "shared_net/reassembly_fixture.h"
#include "../src/dzIPC/net/gateway_data.h"
#include "dzIPC/threepools/socket_wait_set.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>
#include <time.h>
using namespace shared_net_test;
namespace {
std::size_t threads() { return std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}); }
std::vector<std::uint64_t> json_numbers(const std::string& json, const std::string& key) {
    std::vector<std::uint64_t> values; const auto marker = "\"" + key + "\":"; std::size_t at = 0;
    while ((at = json.find(marker, at)) != std::string::npos) {
        at += marker.size(); values.push_back(std::stoull(json.substr(at)));
    }
    return values;
}
std::uint64_t process_cpu_ns() {
    timespec value{}; if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(value.tv_nsec);
}
}
TEST(SharedNetTeardown, ManyIdleGatewayEndpointsBlockAndReturnOwnerSlots) {
    if (!dzIPC::threepools::SocketWaitSet::backend_available()) GTEST_SKIP() << "多 FD 等待后端不可用";
    Directory dir; auto config = configuration(dir); config.network_version = NetworkVersion::V2;
    config.data_mode = DataMode::PerTopic; config.data_shards = 0; config.data_workers = 4;
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints; auto wake = local::event();
    Identity id{}; id[0] = 3;
    GatewayData data(config, id, 1, wake.get(), std::move(endpoints), std::vector<unsigned>{});
    auto view = std::make_shared<GatewayDataView>(); view->local = std::make_shared<DirectorySnapshot>();
    view->bridges = std::make_shared<GatewayDataView::Bridges>(); data.synchronize(view).get();

    constexpr unsigned endpoint_count = 256;
    std::vector<std::uint16_t> ports; ports.reserve(endpoint_count);
    std::vector<std::unique_ptr<DatagramEndpoint>> pending; pending.reserve(endpoint_count);
    for (unsigned i = 0; i < endpoint_count; ++i) {
        auto endpoint = std::make_unique<DatagramEndpoint>(Ipv4Address::parse("127.0.0.1", 0));
        ports.push_back(endpoint->local_address().port);
        pending.push_back(std::move(endpoint));
    }
    std::atomic<bool> add_ok{true};
    std::vector<std::thread> adders;
    for (unsigned worker = 0; worker < config.data_workers; ++worker)
        adders.emplace_back([&, worker] {
            for (unsigned i = worker; i < endpoint_count; i += config.data_workers)
                if (!data.add_endpoint(std::move(pending[i]), ports[i], worker)) add_ok.store(false);
        });
    for (auto& adder : adders) adder.join();
    ASSERT_TRUE(add_ok.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto baseline = data.shard_metrics();
    const auto sockets = json_numbers(baseline, "socket_count");
    ASSERT_EQ(sockets.size(), config.data_workers);
    for (const auto count : sockets) EXPECT_EQ(count, endpoint_count / config.data_workers);
    const auto wakeups_before = json_numbers(baseline, "wakeups");
    const auto cpu_before = process_cpu_ns();
    const auto wall_before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto wall_elapsed = std::chrono::steady_clock::now() - wall_before;
    const auto cpu_after = process_cpu_ns();
    const auto idle = data.shard_metrics();
    EXPECT_EQ(json_numbers(idle, "wakeups"), wakeups_before);
    EXPECT_GE(wall_elapsed, std::chrono::milliseconds(180));
    ASSERT_GT(cpu_after, cpu_before);
    EXPECT_LT(cpu_after - cpu_before, 50000000ull) << "256 个空闲端点不应忙轮询";

    ASSERT_TRUE(data.remove_endpoint(ports.back()));
    auto replacement = std::make_unique<DatagramEndpoint>(Ipv4Address::parse("127.0.0.1", 0));
    const auto replacement_port = replacement->local_address().port;
    ASSERT_TRUE(data.add_endpoint(std::move(replacement), replacement_port, (endpoint_count - 1) % config.data_workers));
    for (const auto port : ports) if (port != ports.back()) ASSERT_TRUE(data.remove_endpoint(port));
    ASSERT_TRUE(data.remove_endpoint(replacement_port));
    for (const auto count : json_numbers(data.shard_metrics(), "socket_count")) EXPECT_EQ(count, 0u);
    data.stop();
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
