#pragma once
#include "reassembly_fixture.h"
#include "dzIPC/net/reliable_session.h"

namespace shared_net_test {
struct ReliableFixture {
    ReassemblyFixture receiver;
    Bytes blob;
    WireHeader header;
    ReliableTarget target;
    explicit ReliableFixture(std::size_t size = 4096) : blob(flat_blob(size)) {
        ByteView body; auto packet = receiver.packets(blob).front(); decode_packet(packet.view(), header, body);
        target.peer.admission = std::make_shared<PeerAdmission>(); target.peer.admission->hello.gateway_id = receiver.local;
        target.peer.admission->hello.gateway_epoch = 3; target.peer.admission->hello.control_port = 25004;
        target.peer.admission->ipv4 = Ipv4Address::parse("127.0.0.1", 0).host;
        target.route = receiver.admission.subscriber;
        auto snapshot = std::make_shared<DirectorySnapshot>(); snapshot->routes.emplace(header.route, target.route); target.peer.snapshot = snapshot;
    }
    ReceivedDatagram data(const WireHeader& header) const {
        const auto offset = std::size_t(header.fragment_index) * 1024; Bytes encoded;
        encode_packet(header, {blob.data() + offset, std::min<std::size_t>(1024, blob.size() - offset)}, encoded);
        ReceivedDatagram packet; packet.status = IoStatus::Data; packet.size = encoded.size(); std::copy(encoded.begin(), encoded.end(), packet.bytes.begin());
        packet.source = {receiver.admission.peer->ipv4, data_port(header.route, receiver.admission.peer->hello.data_base_port, receiver.admission.peer->hello.data_shards)}; return packet;
    }
    ReceivedDatagram control(const Bytes& bytes) const {
        ReceivedDatagram packet; packet.status = IoStatus::Data; packet.size = bytes.size(); std::copy(bytes.begin(), bytes.end(), packet.bytes.begin());
        packet.source = {target.peer.admission->ipv4, target.peer.admission->hello.control_port}; return packet;
    }
    ReceivedDatagram ack() const {
        auto h = header; h.kind = PacketKind::Ack; std::swap(h.source_id, h.target_id); std::swap(h.source_epoch, h.target_epoch);
        Bytes encoded; encode_packet(h, {}, encoded); return control(encoded);
    }
};
}
