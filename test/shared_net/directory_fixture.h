#pragma once
#include "dzIPC/net/local_directory.h"
#include "dzIPC/common/channel_scope.h"
#include "runtime_fixture.h"
#include <algorithm>

namespace shared_net_test {
inline Identity identity(unsigned value) { Identity id{}; for (unsigned i = 0; i < 4; ++i) id[i] = value >> (i * 8); return id; }
inline RouteDescriptor catalog_route(const std::string& topic, std::uint16_t role = 1, std::uint64_t epoch = 0) {
    RouteDescriptor d; d.topic = topic; d.key.scope = dzIPC::common::channel_scope_token(topic, 0, dzIPC::common::ScopeKind::PubSub);
    d.key.msg_id = 71; d.role_flags = role; d.receiver_route_epoch = epoch; return d;
}
struct DirectoryFixture {
    Identity local = identity(1), remote = identity(2);
    std::shared_ptr<DirectoryBudget> budget;
    PeerDirectory peers;
    DiscoveryHello hello;
    Ipv4Address source = Ipv4Address::parse("127.0.0.2", 24004);
    explicit DirectoryFixture(Limits limits = {}) : budget(std::make_shared<DirectoryBudget>(limits)), peers(local, 10, budget) {
        hello.gateway_id = remote; hello.gateway_epoch = 20; hello.snapshot_version = 1;
        peers.hello(hello, source, 1);
    }
    std::vector<ReceivedDatagram> pages(std::vector<RouteDescriptor> routes, std::uint64_t version = 1) const {
        std::sort(routes.begin(), routes.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        DirectorySnapshot s; s.version = version;
        if (!encode_directory(routes, s.body)) throw std::runtime_error("测试目录无效");
        s.body_crc = crc32c(ByteView(s.body));
        CatalogHeader h; h.source_id = remote; h.source_epoch = hello.gateway_epoch; h.target_id = local; h.target_epoch = 10;
        std::vector<ReceivedDatagram> result;
        for (unsigned i = 0; i < (s.body.size() + 1023) / 1024; ++i) {
            auto packet = catalog_page(s, h, i); ReceivedDatagram d; d.status = IoStatus::Data; d.source = source; d.size = packet.size();
            std::copy(packet.begin(), packet.end(), d.bytes.begin()); result.push_back(d);
        }
        return result;
    }
    DirectoryCode install(std::vector<RouteDescriptor> routes, std::uint64_t version = 1, std::uint64_t now = 2) {
        DirectoryCode result = DirectoryCode::Incomplete;
        for (const auto& page : pages(std::move(routes), version)) result = peers.page(page, now);
        return result;
    }
};
} // namespace shared_net_test
