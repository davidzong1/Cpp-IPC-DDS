#include "shared_net/directory_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetDiscovery, SelfAddressAndEndpointConflicts) {
    DirectoryFixture f; auto h = f.hello; h.gateway_id = f.local; h.gateway_epoch = 10;
    EXPECT_EQ(f.peers.hello(h, f.source, 2), DirectoryCode::Ignored);
    h.gateway_epoch = 11; EXPECT_EQ(f.peers.hello(h, f.source, 2), DirectoryCode::IdentityConflict);
    EXPECT_EQ(f.peers.hello(f.hello, Ipv4Address::parse("127.0.0.3", 24004), 2), DirectoryCode::IdentityConflict);
    h = f.hello; ++h.control_port; EXPECT_EQ(f.peers.hello(h, f.source, 2), DirectoryCode::IdentityConflict);
    EXPECT_EQ(f.budget->usage().histories, 1u);
}
TEST(SharedNetDiscovery, LeaseRecoveryRequiresFullSnapshotAndKeepsAdmission) {
    DirectoryFixture f; const auto route = catalog_route("lease", 3, 7);
    ASSERT_EQ(f.install({route}), DirectoryCode::Ok); auto old = f.peers.peer(f.remote);
    EXPECT_EQ(f.peers.targets(route).size(), 1u); f.peers.tick(3000000001ull);
    EXPECT_FALSE(old.admission->active.load()); EXPECT_TRUE(f.peers.targets(route).empty());
    EXPECT_TRUE(old.snapshot->routes.at(route.key)->active.load());
    f.peers.hello(f.hello, f.source, 3000000002ull);
    EXPECT_FALSE(old.admission->active.load()); ASSERT_FALSE(f.peers.tick(3000000002ull).empty());
    ASSERT_EQ(f.install({route}, 1, 3000000003ull), DirectoryCode::Ok);
    EXPECT_EQ(f.peers.peer(f.remote).admission, old.admission);
    EXPECT_EQ(f.peers.peer(f.remote).snapshot, old.snapshot); EXPECT_TRUE(old.admission->active.load());
}
TEST(SharedNetDiscovery, RetiredRandomEpochNeverReturnsAfterNewEpoch) {
    DirectoryFixture f; auto route = catalog_route("retired", 3, 5); ASSERT_EQ(f.install({route}), DirectoryCode::Ok);
    auto old = f.peers.peer(f.remote); const auto delayed = f.pages({route}).front();
    auto h = f.hello; h.gateway_epoch = 3;
    ASSERT_EQ(f.peers.hello(h, f.source, 3), DirectoryCode::Ok); EXPECT_FALSE(old.admission->active.load());
    EXPECT_FALSE(old.snapshot->routes.at(route.key)->active.load()); EXPECT_FALSE(f.peers.peer(f.remote).snapshot);
    EXPECT_EQ(f.peers.hello(f.hello, f.source, 4), DirectoryCode::RetiredEpoch);
    EXPECT_EQ(f.peers.page(delayed, 4), DirectoryCode::Ignored); EXPECT_EQ(f.peers.peer(f.remote).admission->hello.gateway_epoch, 3u);
}
TEST(SharedNetDiscovery, HistoryLimitRejectsNewEpochWithoutRetiringCurrent) {
    Limits limits; limits.gateway_history = 2; limits.peer_history = 3;
    DirectoryFixture f(limits); auto h = f.hello; h.gateway_epoch = 21;
    ASSERT_EQ(f.peers.hello(h, f.source, 2), DirectoryCode::Ok); auto current = f.peers.peer(f.remote).admission;
    h.gateway_epoch = 22; EXPECT_EQ(f.peers.hello(h, f.source, 3), DirectoryCode::HistoryFull);
    EXPECT_EQ(f.peers.peer(f.remote).admission, current);
    h.gateway_id = identity(3); EXPECT_EQ(f.peers.hello(h, f.source, 3), DirectoryCode::Ok);
    h.gateway_id = identity(4); EXPECT_EQ(f.peers.hello(h, f.source, 3), DirectoryCode::HistoryFull);
    EXPECT_EQ(f.budget->usage().histories, 3u);
}
TEST(SharedNetDiscovery, PeerCapacityAndRequestBackoffAreBounded) {
    DirectoryFixture f;
    for (unsigned i = 3; i <= 129; ++i) { auto h = f.hello; h.gateway_id = identity(i); ASSERT_EQ(f.peers.hello(h, f.source, 1), DirectoryCode::Ok); }
    auto h = f.hello; h.gateway_id = identity(130); EXPECT_EQ(f.peers.hello(h, f.source, 1), DirectoryCode::HistoryFull);
    EXPECT_EQ(f.peers.tick(2, 16).size(), 16u); EXPECT_EQ(f.peers.peers().size(), 128u);
    DirectoryFixture one; EXPECT_EQ(one.peers.tick(2).size(), 1u); EXPECT_TRUE(one.peers.tick(100).empty());
    EXPECT_EQ(one.peers.tick(100000002).size(), 1u); EXPECT_TRUE(one.peers.tick(100000003).empty());
}
TEST(SharedNetDiscovery, RealGatewayExchangesHelloAndCatalogOnControlSocket) {
    Directory dir; auto config = configuration(dir);
    DatagramEndpoint observer(Ipv4Address::parse("0.0.0.0", config.discovery_port), true);
    observer.join_discovery(config.discovery_group, config.interface, config.listen_ip);
    GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); DatagramEndpoint remote(Ipv4Address::parse("127.0.0.1", 0));
    ReceivedDatagram announcement; DiscoveryHello own;
    ASSERT_TRUE(until([&] { return observer.receive(announcement).status == IoStatus::Data && decode_hello(announcement.view(), own) && own.gateway_epoch == client->gateway_epoch(); }));
    EXPECT_EQ(announcement.source.host, Ipv4Address::parse(config.listen_ip, 1).host); EXPECT_EQ(announcement.source.port, config.discovery_port);
    DiscoveryHello hello; hello.gateway_id = identity(987); hello.gateway_epoch = 19; hello.control_port = remote.local_address().port;
    Bytes h; ASSERT_TRUE(encode_hello(hello, h));
    ASSERT_EQ(remote.send(ByteView(h), Ipv4Address::parse("127.0.0.1", config.discovery_port)).status, IoStatus::Data);
    ReceivedDatagram request;
    ASSERT_TRUE(until([&] { return remote.receive(request).status == IoStatus::Data; }));
    CatalogHeader header; ByteView payload; ASSERT_TRUE(decode_catalog(request.view(), header, payload)); EXPECT_EQ(header.kind, CatalogKind::Request);
    DirectorySnapshot empty; empty.body = {0, 0, 0, 0}; empty.body_crc = crc32c(ByteView(empty.body));
    std::swap(header.source_id, header.target_id); std::swap(header.source_epoch, header.target_epoch);
    auto page = catalog_page(empty, header, 0); ASSERT_EQ(remote.send(ByteView(page), request.source).status, IoStatus::Data);
    EXPECT_TRUE(until([&] { return client->status().find("\"active_peers\":1") != std::string::npos; }));
}
