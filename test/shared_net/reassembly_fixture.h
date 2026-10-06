#pragma once
#include "outbox_fixture.h"
#include "dzIPC/net/reassembly.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/channel_scope.h"

namespace shared_net_test {
struct ReassemblyFixture {
    Identity local{};
    std::shared_ptr<ReassemblyBudget> budget;
    ReceiveAdmission admission;
    std::unique_ptr<ReassemblyShard> shard;
    explicit ReassemblyFixture(Limits limits = {}) : budget(std::make_shared<ReassemblyBudget>(limits)) {
        local[0] = 9;
        admission.peer = std::make_shared<PeerAdmission>(); admission.peer->hello.gateway_id[0] = 8;
        admission.peer->hello.gateway_epoch = 2; admission.peer->hello.data_shards = 3;
        admission.peer->hello.data_base_port = 20000; admission.peer->ipv4 = Ipv4Address::parse("127.0.0.1", 0).host;
        admission.publisher = std::make_shared<RouteAdmission>(); admission.subscriber = std::make_shared<RouteAdmission>();
        route("reassembly/a"); shard = std::make_unique<ReassemblyShard>(local, 3, 0, 1, budget);
    }
    void route(const std::string& topic) {
        RouteDescriptor p; p.topic = topic; p.key.scope = dzIPC::common::channel_scope_token(topic, 0, dzIPC::common::ScopeKind::PubSub);
        p.key.msg_id = 71; p.schema_hash = 0xabcdef01; p.role_flags = 1;
        admission.publisher->descriptor = p; p.role_flags = 2; p.receiver_route_epoch = 11; admission.subscriber->descriptor = p;
    }
    std::vector<ReceivedDatagram> packets(const Bytes& bytes, std::uint64_t sequence = 1, unsigned publisher = 1, Delivery delivery = Delivery::Reliable) const {
        WireHeader h; h.route = admission.publisher->descriptor.key; h.source_id = admission.peer->hello.gateway_id; h.source_epoch = admission.peer->hello.gateway_epoch;
        h.target_id = local; h.target_epoch = 3; h.publisher_id[0] = publisher; h.sequence = sequence; h.message_size = bytes.size();
        h.fragment_count = (bytes.size() + 1023) / 1024; h.message_crc = crc32c(ByteView(bytes)); h.schema_hash = 0xabcdef01;
        h.receiver_route_epoch = admission.subscriber->descriptor.receiver_route_epoch; h.encoding = Encoding::DzFlat; h.delivery = delivery;
        if (admission.network_version == NetworkVersion::V2) {
            h.data_source_endpoint_epoch = admission.publisher->descriptor.endpoint_epoch;
            h.data_target_endpoint_epoch = admission.subscriber->descriptor.endpoint_epoch;
        }
        std::vector<ReceivedDatagram> result;
        for (h.fragment_index = 0; h.fragment_index < h.fragment_count; ++h.fragment_index) {
            const auto offset = std::size_t(h.fragment_index) * 1024;
            Bytes encoded;
            const auto payload = ByteView(bytes.data() + offset, std::min<std::size_t>(1024, bytes.size() - offset));
            const auto status = admission.network_version == NetworkVersion::V2
                ? encode_packet_v2(h, payload, encoded) : encode_packet(h, payload, encoded);
            if (!status) throw std::runtime_error("测试分片编码失败");
            ReceivedDatagram packet; std::copy(encoded.begin(), encoded.end(), packet.bytes.begin()); packet.size = encoded.size(); packet.status = IoStatus::Data;
            packet.source = {admission.peer->ipv4, admission.network_version == NetworkVersion::V2
                ? admission.publisher->descriptor.data_port
                : static_cast<std::uint16_t>(admission.peer->hello.data_base_port + route_hash(h.route) % admission.peer->hello.data_shards)};
            result.push_back(packet);
        }
        return result;
    }
    ReceiveFeedback feed(const Bytes& bytes, std::uint64_t sequence = 1, Delivery delivery = Delivery::Reliable, std::uint64_t now = 1) {
        ReceiveFeedback last;
        for (const auto& packet : packets(bytes, sequence, 1, delivery)) last = shard->ingest(packet, admission, now);
        return last;
    }
};
inline ReceivedDatagram changed_packet(ReceivedDatagram packet, const std::function<void(WireHeader&, Bytes&)>& change) {
    WireHeader h; ByteView p;
    const auto decoded = packet.size > 4 && packet.bytes[4] == 2
        ? decode_packet_v2(packet.view(), h, p) : decode_packet(packet.view(), h, p);
    if (!decoded) throw std::runtime_error("测试分片解码失败");
    Bytes body(p.data, p.data + p.size); change(h, body); Bytes encoded;
    const auto status = packet.size > 4 && packet.bytes[4] == 2
        ? encode_packet_v2(h, ByteView(body), encoded) : encode_packet(h, ByteView(body), encoded);
    if (!status) throw std::runtime_error("测试分片重编码失败");
    std::copy(encoded.begin(), encoded.end(), packet.bytes.begin()); packet.size = encoded.size(); return packet;
}
inline PacketKind reply_kind(const ReceiveFeedback& feedback) {
    WireHeader h; ByteView body;
    const auto decoded = feedback.control_packet.size() > 4 && feedback.control_packet[4] == 2
        ? decode_packet_v2(ByteView(feedback.control_packet), h, body)
        : decode_packet(ByteView(feedback.control_packet), h, body);
    if (!decoded) throw std::runtime_error("测试回执解码失败"); return h.kind;
}
} // namespace shared_net_test
