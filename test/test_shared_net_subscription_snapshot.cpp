#include "shared_net/directory_fixture.h"
#include "shared_net/shm_wire_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetSnapshot, OutOfOrderDuplicateMissingAndAtomicReplacement) {
    DirectoryFixture f; auto old = catalog_route("old", 2, 5); ASSERT_EQ(f.install({old}), DirectoryCode::Ok);
    auto before = f.peers.peer(f.remote).snapshot;
    std::vector<RouteDescriptor> routes; for (unsigned i = 0; i < 40; ++i) routes.push_back(catalog_route(std::string(60, 'x') + std::to_string(i), 3, i + 1));
    auto pages = f.pages(routes, 2); ASSERT_GT(pages.size(), 2u);
    for (std::size_t i = pages.size(); i-- > 1;) { EXPECT_EQ(f.peers.page(pages[i], 3), DirectoryCode::Incomplete); EXPECT_EQ(f.peers.page(pages[i], 3), DirectoryCode::Incomplete); }
    EXPECT_EQ(f.peers.peer(f.remote).snapshot, before);
    ASSERT_EQ(f.peers.page(pages[0], 3), DirectoryCode::Ok); auto after = f.peers.peer(f.remote).snapshot;
    EXPECT_EQ(after->routes.size(), routes.size()); EXPECT_FALSE(before->routes.at(old.key)->active.load());
    for (const auto& route : routes) { ASSERT_TRUE(after->routes.count(route.key)); EXPECT_EQ(after->routes.at(route.key)->descriptor.topic, route.topic); }
    EXPECT_EQ(f.peers.page(f.pages({old})[0], 4), DirectoryCode::Stale);
}
TEST(SharedNetSnapshot, ConflictingDuplicateCrcAndVersionDoNotReplaceInstalled) {
    DirectoryFixture f; ASSERT_EQ(f.install({catalog_route("base")}), DirectoryCode::Ok); auto before = f.peers.peer(f.remote).snapshot;
    auto d = catalog_route(std::string(1024, 't')); auto pages = f.pages({d}, 2); ASSERT_EQ(pages.size(), 2u);
    ASSERT_EQ(f.peers.page(pages[0], 3), DirectoryCode::Incomplete);
    CatalogHeader h; ByteView p; ASSERT_TRUE(decode_catalog(pages[0].view(), h, p)); Bytes changed(p.data, p.data + p.size); changed.back() ^= 1;
    Bytes encoded; ASSERT_TRUE(encode_catalog(h, ByteView(changed), encoded)); std::copy(encoded.begin(), encoded.end(), pages[0].bytes.begin());
    EXPECT_EQ(f.peers.page(pages[0], 3), DirectoryCode::Conflict); EXPECT_EQ(f.budget->usage().candidates, 0u);
    EXPECT_EQ(f.peers.page(pages[0], 3), DirectoryCode::Incomplete); EXPECT_EQ(f.peers.page(pages[1], 3), DirectoryCode::Invalid);
    EXPECT_EQ(f.peers.peer(f.remote).snapshot, before);
    auto newer = f.pages({d}, 4); EXPECT_EQ(f.peers.page(newer[0], 4), DirectoryCode::Incomplete);
    EXPECT_EQ(f.peers.page(f.pages({d}, 3)[1], 4), DirectoryCode::Stale);
}
TEST(SharedNetSnapshot, CandidateLimitsExpiryAndWrongSourceNeverInstall) {
    Limits limits; limits.peer_candidate_bytes = 1024; DirectoryFixture f(limits);
    auto large = f.pages({catalog_route(std::string(1024, 'a'))}); EXPECT_EQ(f.peers.page(large[0], 2), DirectoryCode::QuotaExceeded);
    EXPECT_EQ(f.budget->usage().candidates, 0u);
    DirectoryFixture normal; auto pages = normal.pages({catalog_route(std::string(1024, 'a'))});
    auto wrong = pages[0]; ++wrong.source.port; EXPECT_EQ(normal.peers.page(wrong, 2), DirectoryCode::Ignored); EXPECT_EQ(normal.budget->usage().candidates, 0u);
    EXPECT_EQ(normal.peers.page(pages[0], 2), DirectoryCode::Incomplete); EXPECT_GT(normal.budget->usage().candidates, 0u);
    normal.peers.tick(2000000002ull); EXPECT_EQ(normal.budget->usage().candidates, 0u); EXPECT_FALSE(normal.peers.peer(normal.remote).snapshot);
}
TEST(SharedNetSnapshot, InstalledAndReferencedOldDirectoryQuotasReleaseOnLastReference) {
    auto budget = std::make_shared<DirectoryBudget>(); auto first = budget->replace({catalog_route("a")}, 1);
    ASSERT_TRUE(first); const auto charge = budget->usage().installed;
    auto second = budget->replace({catalog_route("b")}, 2, first); ASSERT_TRUE(second);
    EXPECT_EQ(budget->usage().old, charge); first.reset(); EXPECT_EQ(budget->usage().old, 0u);
    second.reset(); EXPECT_EQ(budget->usage().installed, 0u);
    Limits limits; limits.old_directory_bytes = 1; auto limited = std::make_shared<DirectoryBudget>(limits);
    first = limited->replace({catalog_route("a")}, 1); ASSERT_TRUE(first); EXPECT_FALSE(limited->replace({catalog_route("b")}, 2, first));
    EXPECT_TRUE(first->routes.begin()->second->active.load()); EXPECT_EQ(limited->usage().old, 0u);
    limits.directory_bytes = 1; EXPECT_FALSE(std::make_shared<DirectoryBudget>(limits)->replace({}, 1));
}
TEST(SharedNetSnapshot, PubOnlyAndTypeConflictsAndMaximumTopicCount) {
    DirectoryFixture f; const auto pub = catalog_route("pub-only"); ASSERT_EQ(f.install({pub}), DirectoryCode::Ok);
    EXPECT_TRUE(f.peers.targets(pub).empty()); EXPECT_TRUE(f.peers.peer(f.remote).snapshot->routes.count(pub.key));
    auto conflict = pub; ++conflict.key.msg_id;
    EXPECT_EQ(f.install({pub, conflict}, 2), DirectoryCode::QuotaExceeded);
    std::vector<RouteDescriptor> routes; for (unsigned i = 0; i < 4096; ++i) routes.push_back(catalog_route("topic/" + std::to_string(i), 2, i + 1));
    ASSERT_EQ(f.install(routes, 3), DirectoryCode::Ok); EXPECT_EQ(f.peers.peer(f.remote).snapshot->routes.size(), 4096u);
    routes.push_back(catalog_route("excess")); Bytes oversized; EXPECT_FALSE(encode_directory(routes, oversized));
    DirectorySnapshot malformed; malformed.version = 4; malformed.body = {0, 0, 16, 1}; malformed.body_crc = crc32c(ByteView(malformed.body));
    CatalogHeader h; h.source_id = f.remote; h.source_epoch = f.hello.gateway_epoch; h.target_id = f.local; h.target_epoch = 10;
    auto bytes = catalog_page(malformed, h, 0); ReceivedDatagram packet; packet.status = IoStatus::Data; packet.source = f.source; packet.size = bytes.size(); std::copy(bytes.begin(), bytes.end(), packet.bytes.begin());
    EXPECT_EQ(f.peers.page(packet, 4), DirectoryCode::QuotaExceeded);
}
TEST(SharedNetSnapshot, TwoStageSubscriberAndLastReadyEpochAndTypeReservation) {
    BusinessTopic topic; auto budget = std::make_shared<DirectoryBudget>(); LocalDirectory local(budget);
    auto pub = local.add(1, identity(1), topic.descriptor, true); auto sub = local.add(2, identity(2), topic.descriptor, false);
    EXPECT_EQ(local.snapshot()->routes.at(topic.descriptor.key)->descriptor.role_flags, 1u);
    EXPECT_THROW(local.ready(2, identity(2), 1), std::runtime_error);
    auto bridge = std::make_shared<ShmWireBridge>(topic.descriptor); ASSERT_TRUE(local.set_bridge(sub->binding, bridge));
    const auto generation = bridge->generation(); const auto epoch = local.ready(2, identity(2), generation); EXPECT_GT(epoch, 0u);
    auto admission = local.snapshot()->routes.at(topic.descriptor.key);
    auto sub2 = local.add(3, identity(3), topic.descriptor, false); EXPECT_EQ(local.ready(3, identity(3), generation), epoch);
    EXPECT_TRUE(local.remove(2, identity(2), false)); EXPECT_TRUE(admission->active.load());
    EXPECT_TRUE(local.remove(3, identity(3), false)); EXPECT_FALSE(admission->active.load()); EXPECT_EQ(local.snapshot()->routes.at(topic.descriptor.key)->descriptor.role_flags, 1u);
    auto sub3 = local.add(4, identity(4), topic.descriptor, false); ASSERT_TRUE(local.set_bridge(sub3->binding, bridge));
    EXPECT_GT(local.ready(4, identity(4), generation), epoch);
    auto wrong = topic.descriptor; ++wrong.key.msg_id; EXPECT_THROW(local.add(5, identity(5), wrong, true), std::runtime_error);
    wrong = topic.descriptor; wrong.schema_hash = 123; local.add(5, identity(5), wrong, true);
    wrong.schema_hash = 456; EXPECT_THROW(local.add(5, identity(6), wrong, false), std::runtime_error);
    local.close_session(4); local.close_session(5); local.close_session(1); EXPECT_EQ(local.handle_count(), 0u); EXPECT_TRUE(local.snapshot()->routes.empty());
}
TEST(SharedNetSnapshot, RealControlRegistrationWaitsForBridgeAndReady) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config); auto client = ClientRuntime::acquire(dir.control());
    const auto id = identity(88); Bytes descriptor; ASSERT_TRUE(encode_descriptor(topic.descriptor, true, descriptor));
    Bytes body(id.begin(), id.end()); body.insert(body.end(), descriptor.begin(), descriptor.end());
    auto registered = client->request(LocalKind::RegisterSub, body); ASSERT_EQ(registered.header.kind, LocalKind::SubRegistered); ASSERT_EQ(registered.body.size(), 28u);
    const auto generation = codec::get(registered.body.data() + 16, 4); ASSERT_NE(generation, 0u); EXPECT_EQ(codec::get(registered.body.data() + 20, 8), 0u);
    body.assign(id.begin(), id.end()); codec::append(body, generation, 4); auto ready = client->request(LocalKind::SubReady, body);
    ASSERT_EQ(ready.header.kind, LocalKind::SubReadyAck); EXPECT_NE(codec::get(ready.body.data() + 16, 8), 0u);
    body.assign(id.begin(), id.end()); body.push_back(2); EXPECT_EQ(client->request(LocalKind::Unregister, body).header.kind, LocalKind::Unregistered);
}
TEST(SharedNetSnapshot, CandidateGlobalLimitAndWithdrawalFailureRemainBounded) {
    Limits limits; limits.candidate_bytes = 1024;
    DirectoryFixture f(limits); const auto pages = f.pages({catalog_route(std::string(1024, 'z'))});
    EXPECT_EQ(f.peers.page(pages[0], 2), DirectoryCode::QuotaExceeded); EXPECT_EQ(f.budget->usage().candidates, 0u);
    limits = {}; limits.old_directory_bytes = 300;
    auto budget = std::make_shared<DirectoryBudget>(limits); LocalDirectory local(budget, limits);
    auto route = catalog_route("withdraw", 0); auto pub = local.add(1, identity(1), route, true);
    auto admission = local.snapshot()->routes.at(route.key);
    EXPECT_TRUE(local.remove(1, identity(1), true)); EXPECT_FALSE(local.healthy());
    EXPECT_FALSE(admission->active.load()); EXPECT_EQ(local.handle_count(), 0u);
    EXPECT_THROW(local.add(1, identity(2), route, true), std::runtime_error);
}
TEST(SharedNetSnapshot, LargeCatalogProgressExtendsIdleTimeoutButDuplicatesCannot) {
    DirectoryFixture f;
    std::vector<RouteDescriptor> routes;
    for (unsigned i = 0; i < 40; ++i) routes.push_back(catalog_route(std::string(1000, 'a') + std::to_string(i)));
    const auto pages = f.pages(routes); ASSERT_GT(pages.size(), 30u);
    for (unsigned i = 0; i < 29; ++i) {
        const auto now = std::uint64_t(i) * 1000000000ull + 2;
        f.peers.hello(f.hello, f.source, now);
        EXPECT_EQ(f.peers.page(pages[i], now), DirectoryCode::Incomplete);
    }
    EXPECT_GT(f.budget->usage().candidates, 0u);
    f.peers.hello(f.hello, f.source, 29999999999ull);
    EXPECT_EQ(f.peers.page(pages[0], 29999999999ull), DirectoryCode::Incomplete);
    f.peers.tick(30000000002ull); EXPECT_EQ(f.budget->usage().candidates, 0u); EXPECT_FALSE(f.peers.peer(f.remote).snapshot);
}
