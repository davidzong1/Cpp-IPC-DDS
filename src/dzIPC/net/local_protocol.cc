#include "dzIPC/net/local_protocol.h"
#include "byte_codec.h"

namespace dzIPC::net
{
using namespace codec;
namespace
{
ProtocolStatus fields(bool okay)
{
    return okay ? ProtocolStatus{} : error(ProtocolCode::BadField);
}
bool handle(ByteView b)
{
    return b.size >= 16 && !zeros(b.data, 16);
}
bool valid_tx_name(const std::string &s)
{
    constexpr const char *prefix = "dzgw_tx_v2_";
    constexpr std::size_t prefix_size = sizeof("dzgw_tx_v2_") - 1;
    if (s.size() != prefix_size + 66 || s.compare(0, prefix_size, prefix))
        return false;
    for (std::size_t i = prefix_size; i < s.size(); ++i)
    {
        if (i == prefix_size + 32 || i == prefix_size + 49)
        {
            if (s[i] != '_')
                return false;
        }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    }
    return true;
}
ProtocolStatus validate_body(const LocalHeader &h, ByteView b)
{
    if (!valid(b) || b.size > kLocalMaxSize - kLocalHeaderSize)
        return error(ProtocolCode::BadLength);
    if (h.kind == LocalKind::Hello)
    {
        if (h.session_id || h.gateway_epoch || !h.request_id)
            return error(ProtocolCode::BadField);
    }
    else if (!h.session_id || !h.gateway_epoch)
        return error(ProtocolCode::BadField);
    const bool notification = h.kind == LocalKind::TxProgress || h.kind == LocalKind::RouteState;
    const bool flexible = h.kind == LocalKind::CreditGrant || h.kind == LocalKind::Error;
    if (notification ? h.request_id != 0 : (!flexible && h.request_id == 0))
        return error(ProtocolCode::BadField);
    switch (h.kind)
    {
    case LocalKind::Hello:
        if (b.size != 36)
            return error(ProtocolCode::BadLength);
        return fields(handle(b) && get(b.data + 16, 8) && get(b.data + 24, 8) &&
                      get(b.data + 32, 4) == 1);
    case LocalKind::Welcome: {
        WelcomeBody w;
        auto s = decode_welcome(b, w);
        if (!s)
            return s;
        return fields(w.tx_name == outbox_name(w.locality, h.gateway_epoch, h.session_id));
    }
    case LocalKind::AttachTx:
    case LocalKind::TxReady:
        return fields(b.size == 0);
    case LocalKind::RegisterPub:
    case LocalKind::RegisterSub: {
        if (b.size < 16 || !handle(b))
            return error(ProtocolCode::BadLength);
        RouteDescriptor d;
        return decode_descriptor(ByteView(b.data + 16, b.size - 16), true, d);
    }
    case LocalKind::PubRegistered:
    case LocalKind::Unregistered:
        return fields(b.size == 16 && handle(b));
    case LocalKind::SubRegistered:
        return fields(b.size == 28 && handle(b) && get(b.data + 16, 4));
    case LocalKind::SubReady:
        return fields(b.size == 20 && handle(b) && get(b.data + 16, 4));
    case LocalKind::SubReadyAck:
        return fields(b.size == 24 && handle(b) && get(b.data + 16, 8));
    case LocalKind::Unregister:
        return fields(b.size == 17 && handle(b) && (b.data[16] == 1 || b.data[16] == 2));
    case LocalKind::QueryState: {
        if (b.size == 2 && b.data[0] == 3) return fields(b.data[1] <= 3);
        if (b.size < 1 || b.data[0] > 2)
            return error(ProtocolCode::BadLength);
        const auto required = b.data[0] == 0 ? 1u : b.data[0] == 1 ? 37u : 53u;
        if (b.size != required)
            return error(ProtocolCode::BadLength);
        if (b.data[0])
        {
            Scope scope;
            copy(scope, b.data + 1);
            if (!valid_scope(scope))
                return error(ProtocolCode::BadField);
        }
        return fields(b.data[0] != 2 || !zeros(b.data + 37, 16));
    }
    case LocalKind::State:
        if (b.size < 4 || get(b.data, 4) != b.size - 4)
            return error(ProtocolCode::BadLength);
        // JSON 的业务字段由 status 层解释，此层只校验长度与 UTF-8。
        return fields(b.size > 4 && utf8(ByteView(b.data + 4, b.size - 4)));
    case LocalKind::SendResult: {
        SendResultBody r;
        return decode_send_result(b, r);
    }
    case LocalKind::Ping:
    case LocalKind::Pong:
        return fields(b.size == 8);
    case LocalKind::Error:
        if (b.size < 6 || get(b.data + 4, 2) != b.size - 6)
            return error(ProtocolCode::BadLength);
        return fields(utf8(ByteView(b.data + 6, b.size - 6)));
    case LocalKind::TxProgress:
    case LocalKind::CreditGrant:
        return fields(b.size == 16);
    case LocalKind::RouteState: {
        RouteStateBody r;
        return decode_route_state(b, r);
    }
    case LocalKind::CreditRequest:
        if (b.size != 8)
            return error(ProtocolCode::BadLength);
        return fields((get(b.data, 4) || get(b.data + 4, 4)) &&
                      get(b.data, 4) <= 32 * 1024 * 1024 && get(b.data + 4, 4) <= 512);
    default:
        return error(ProtocolCode::BadField); // 明确拒绝保留编号 15/16
    }
}
ProtocolStatus validate_outbox(const OutboxHeader &h, ByteView b, std::uint32_t max_message)
{
    if (!valid(b) || !b.size || b.size > max_message || b.size > kMaxMessageBytes)
        return error(ProtocolCode::BadLength);
    if (!h.gateway_epoch || !h.session_id || !nonzero(h.publisher_id) || !h.sequence ||
        !valid_scope(h.route.scope) ||
        (h.encoding != Encoding::Tlv && h.encoding != Encoding::DzFlat) ||
        (h.encoding == Encoding::Tlv && h.schema_hash) ||
        (h.encoding == Encoding::DzFlat && !h.schema_hash))
        return error(ProtocolCode::BadField);
    if (h.delivery == Delivery::Reliable)
        return fields(h.request_id && h.deadline_monotonic_ns);
    return fields(h.delivery == Delivery::BestEffort && !h.request_id && !h.deadline_monotonic_ns);
}
} // namespace
std::string outbox_name(const Identity &locality, std::uint64_t epoch, std::uint64_t session)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string s = "dzgw_tx_v2_";
    for (auto b : locality)
    {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    for (auto value : {epoch, session})
    {
        s += '_';
        for (int shift = 60; shift >= 0; shift -= 4)
            s += hex[(value >> shift) & 15];
    }
    return s;
}
ProtocolStatus encode_local(const LocalHeader &h, ByteView body, Bytes &out)
{
    auto s = validate_body(h, body);
    if (!s)
        return s;
    Bytes b(40 + body.size);
    auto *d = b.data();
    std::memcpy(d, "DZLC", 4);
    put(d + 4, 2, 2);
    put(d + 6, static_cast<std::uint16_t>(h.kind), 2);
    put(d + 8, b.size(), 4);
    put(d + 16, h.request_id, 8);
    put(d + 24, h.session_id, 8);
    put(d + 32, h.gateway_epoch, 8);
    if (body.size)
        std::memcpy(d + 40, body.data, body.size);
    out.swap(b);
    return {};
}
ProtocolStatus decode_local(ByteView b, LocalHeader &out, ByteView &body)
{
    if (!valid(b) || b.size < 40 || b.size > 8192)
        return error(ProtocolCode::BadLength);
    const auto *d = b.data;
    if (std::memcmp(d, "DZLC", 4))
        return error(ProtocolCode::BadMagic);
    if (get(d + 4, 2) != 2)
        return error(ProtocolCode::BadVersion);
    if (get(d + 8, 4) != b.size)
        return error(ProtocolCode::BadLength);
    if (get(d + 12, 4))
        return error(ProtocolCode::BadField);
    LocalHeader h;
    h.kind = static_cast<LocalKind>(get(d + 6, 2));
    h.request_id = get(d + 16, 8);
    h.session_id = get(d + 24, 8);
    h.gateway_epoch = get(d + 32, 8);
    ByteView p(d + 40, b.size - 40);
    auto s = validate_body(h, p);
    if (!s)
        return s;
    out = h;
    body = p;
    return {};
}
ProtocolStatus encode_welcome(const WelcomeBody &w, Bytes &out)
{
    if (!valid_tx_name(w.tx_name))
        return error(ProtocolCode::BadField);
    Bytes b(46 + w.tx_name.size());
    auto *d = b.data();
    std::copy(w.locality.begin(), w.locality.end(), d);
    put(d + 16, w.max_message_bytes, 4);
    put(d + 20, w.outbox_limit_bytes, 4);
    put(d + 24, w.outbox_record_limit, 4);
    put(d + 28, w.granted_bytes, 8);
    put(d + 36, w.granted_records, 8);
    put(d + 44, w.tx_name.size(), 2);
    std::copy(w.tx_name.begin(), w.tx_name.end(), d + 46);
    WelcomeBody checked;
    auto s = decode_welcome(ByteView(b), checked);
    if (s)
        out.swap(b);
    return s;
}
ProtocolStatus decode_welcome(ByteView b, WelcomeBody &out)
{
    if (!valid(b) || b.size < 46 || b.size != 46 + get(b.data + 44, 2))
        return error(ProtocolCode::BadLength);
    WelcomeBody w;
    copy(w.locality, b.data);
    w.max_message_bytes = get(b.data + 16, 4);
    w.outbox_limit_bytes = get(b.data + 20, 4);
    w.outbox_record_limit = get(b.data + 24, 4);
    w.granted_bytes = get(b.data + 28, 8);
    w.granted_records = get(b.data + 36, 8);
    w.tx_name.assign(reinterpret_cast<const char *>(b.data + 46), b.size - 46);
    if (!nonzero(w.locality) || !valid_tx_name(w.tx_name) || !w.max_message_bytes ||
        w.max_message_bytes > kMaxMessageBytes || !w.outbox_limit_bytes ||
        w.outbox_limit_bytes > 32 * 1024 * 1024 || !w.outbox_record_limit ||
        w.outbox_record_limit > 256 || w.granted_bytes > 32 * 1024 * 1024 ||
        w.granted_records > 512)
        return error(ProtocolCode::BadField);
    std::uint64_t capacity = 131072, required = std::uint64_t(w.max_message_bytes) + 112;
    if (required <= 65536)
        capacity = ((required + 1023) / 1024) * 1024;
    else
        while (capacity < required)
            capacity *= 2;
    if (w.outbox_limit_bytes < capacity)
        return error(ProtocolCode::BadField);
    out = std::move(w);
    return {};
}
Bytes encode_credit_grant(CreditCounters c)
{
    Bytes b(16);
    put(b.data(), c.bytes, 8);
    put(b.data() + 8, c.records, 8);
    return b;
}
Bytes encode_tx_progress(CreditCounters c)
{
    Bytes b(16);
    put(b.data(), c.records, 8);
    put(b.data() + 8, c.bytes, 8);
    return b;
}
ProtocolStatus decode_credit_grant(ByteView b, CreditCounters &c)
{
    if (!valid(b) || b.size != 16)
        return error(ProtocolCode::BadLength);
    c = {get(b.data, 8), get(b.data + 8, 8)};
    return {};
}
ProtocolStatus decode_tx_progress(ByteView b, CreditCounters &c)
{
    if (!valid(b) || b.size != 16)
        return error(ProtocolCode::BadLength);
    c = {get(b.data + 8, 8), get(b.data, 8)};
    return {};
}
ProtocolStatus encode_route_state(const RouteStateBody &r, Bytes &out)
{
    Bytes b(32);
    std::copy(r.publisher_id.begin(), r.publisher_id.end(), b.begin());
    put(b.data() + 16, r.state_version, 8);
    put(b.data() + 24, r.remote_ready_count, 4);
    put(b.data() + 28, r.synchronized ? 1 : 0, 4);
    RouteStateBody checked;
    auto s = decode_route_state(ByteView(b), checked);
    if (s)
        out.swap(b);
    return s;
}
ProtocolStatus decode_route_state(ByteView b, RouteStateBody &out)
{
    if (!valid(b) || b.size != 32)
        return error(ProtocolCode::BadLength);
    if (!handle(b) || !get(b.data + 16, 8) || get(b.data + 28, 4) > 1 || get(b.data + 24, 4) > 128)
        return error(ProtocolCode::BadField);
    RouteStateBody r;
    copy(r.publisher_id, b.data);
    r.state_version = get(b.data + 16, 8);
    r.remote_ready_count = get(b.data + 24, 4);
    r.synchronized = get(b.data + 28, 4) != 0;
    out = r;
    return {};
}
ProtocolStatus encode_send_result(const SendResultBody &r, Bytes &out)
{
    Bytes b(40);
    std::copy(r.publisher_id.begin(), r.publisher_id.end(), b.begin());
    put(b.data() + 16, r.sequence, 8);
    put(b.data() + 24, static_cast<std::uint32_t>(r.result), 4);
    put(b.data() + 28, r.possible_remote_delivery ? 2 : 0, 4);
    put(b.data() + 32, r.target_count, 4);
    put(b.data() + 36, r.acked_count, 4);
    SendResultBody checked;
    auto s = decode_send_result(ByteView(b), checked);
    if (s)
        out.swap(b);
    return s;
}
ProtocolStatus decode_send_result(ByteView b, SendResultBody &out)
{
    if (!valid(b) || b.size != 40)
        return error(ProtocolCode::BadLength);
    if (!handle(b) || !get(b.data + 16, 8) || get(b.data + 24, 4) > 10 ||
        (get(b.data + 28, 4) & ~2ull) || get(b.data + 32, 4) > 128 ||
        get(b.data + 36, 4) > get(b.data + 32, 4))
        return error(ProtocolCode::BadField);
    SendResultBody r;
    copy(r.publisher_id, b.data);
    r.sequence = get(b.data + 16, 8);
    r.result = static_cast<SendResultCode>(get(b.data + 24, 4));
    r.possible_remote_delivery = get(b.data + 28, 4) != 0;
    r.target_count = get(b.data + 32, 4);
    r.acked_count = get(b.data + 36, 4);
    out = r;
    return {};
}
ProtocolStatus encode_outbox(const OutboxHeader &h, ByteView payload, Bytes &out)
{
    auto s = validate_outbox(h, payload, kMaxMessageBytes);
    if (!s)
        return s;
    Bytes b(112 + payload.size);
    s = encode_outbox_into(h, payload, b.data(), b.size());
    if (s)
        out.swap(b);
    return s;
}
ProtocolStatus encode_outbox_into(const OutboxHeader &h, ByteView payload, void *loan,
                                  std::size_t capacity)
{
    const auto s = validate_outbox(h, payload, kMaxMessageBytes);
    if (!s)
        return s;
    if (!loan || capacity < 112 + payload.size)
        return error(ProtocolCode::BadLength);
    auto *d = static_cast<std::uint8_t *>(loan);
    std::memset(d, 0, 112);
    std::memcpy(d, "DZTX", 4);
    put(d + 4, 2, 2);
    put(d + 6, 112, 2);
    put(d + 8, h.gateway_epoch, 8);
    put(d + 16, h.session_id, 8);
    std::copy(h.publisher_id.begin(), h.publisher_id.end(), d + 24);
    put(d + 40, h.sequence, 8);
    std::copy(h.route.scope.begin(), h.route.scope.end(), d + 48);
    put(d + 80, h.route.msg_id, 4);
    put(d + 84, payload.size, 4);
    put(d + 88, h.schema_hash, 4);
    d[92] = static_cast<std::uint8_t>(h.encoding);
    d[93] = static_cast<std::uint8_t>(h.delivery);
    put(d + 96, h.request_id, 8);
    put(d + 104, h.deadline_monotonic_ns, 8);
    std::memcpy(d + 112, payload.data, payload.size);
    return {};
}
ProtocolStatus decode_outbox(ByteView b, OutboxHeader &out, ByteView &payload,
                             std::uint32_t max_message)
{
    if (!valid(b) || b.size < 112)
        return error(ProtocolCode::BadLength);
    const auto *d = b.data;
    if (std::memcmp(d, "DZTX", 4))
        return error(ProtocolCode::BadMagic);
    if (get(d + 4, 2) != 2)
        return error(ProtocolCode::BadVersion);
    if (get(d + 6, 2) != 112 || get(d + 84, 4) > b.size - 112)
        return error(ProtocolCode::BadLength);
    if (get(d + 94, 2))
        return error(ProtocolCode::BadField);
    OutboxHeader h;
    h.gateway_epoch = get(d + 8, 8);
    h.session_id = get(d + 16, 8);
    copy(h.publisher_id, d + 24);
    h.sequence = get(d + 40, 8);
    copy(h.route.scope, d + 48);
    h.route.msg_id = get(d + 80, 4);
    h.schema_hash = get(d + 88, 4);
    h.encoding = static_cast<Encoding>(d[92]);
    h.delivery = static_cast<Delivery>(d[93]);
    h.request_id = get(d + 96, 8);
    h.deadline_monotonic_ns = get(d + 104, 8);
    const ByteView p(d + 112, get(d + 84, 4));
    auto s = validate_outbox(h, p, max_message);
    if (!s)
        return s;
    out = h;
    payload = p;
    return {};
}
} // namespace dzIPC::net
