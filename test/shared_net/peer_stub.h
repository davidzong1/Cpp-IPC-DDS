#pragma once
#include "directory_fixture.h"
#include "gtest/gtest.h"
namespace shared_net_test {
struct PeerStub {
    DatagramEndpoint control{Ipv4Address::parse("127.0.0.1", 0)}, data{Ipv4Address::parse("127.0.0.1", 0)};
    DiscoveryHello hello;
    void announce(const GatewayConfig& config, const RouteDescriptor& descriptor, const std::shared_ptr<ClientRuntime>& client, Identity publisher) {
        hello.gateway_id = identity(782); hello.gateway_epoch = 88; hello.snapshot_version = 1;
        hello.control_port = control.local_address().port; hello.data_base_port = data.local_address().port; hello.data_shards = 1;
        Bytes packet; ASSERT_TRUE(encode_hello(hello, packet));
        ASSERT_EQ(control.send(ByteView(packet), Ipv4Address::parse(config.listen_ip, config.discovery_port)).status, IoStatus::Data);
        ReceivedDatagram request; ASSERT_TRUE(until([&] { return control.receive(request).status == IoStatus::Data; }));
        CatalogHeader header; ByteView bytes; ASSERT_TRUE(decode_catalog(request.view(), header, bytes));
        std::swap(header.source_id, header.target_id); std::swap(header.source_epoch, header.target_epoch);
        auto route = descriptor; route.role_flags = 2; route.receiver_route_epoch = 77;
        DirectorySnapshot snapshot; snapshot.version = 1; ASSERT_TRUE(encode_directory({route}, snapshot.body)); snapshot.body_crc = crc32c(ByteView(snapshot.body));
        auto page = catalog_page(snapshot, header, 0); ASSERT_EQ(control.send(ByteView(page), request.source).status, IoStatus::Data);
        ASSERT_TRUE(until([&] { return client->route_state(publisher).remote_ready_count == 1; }));
    }
    void reply(const GatewayConfig& config, WireHeader h, PacketKind kind, Bytes payload = {}) {
        std::swap(h.source_id, h.target_id); std::swap(h.source_epoch, h.target_epoch); h.kind = kind; h.fragment_index = 0;
        Bytes encoded; ASSERT_TRUE(encode_packet(h, ByteView(payload), encoded));
        ASSERT_EQ(control.send(ByteView(encoded), Ipv4Address::parse(config.listen_ip, config.control_port)).status, IoStatus::Data);
    }
};
}
