#pragma once
#include "dzIPC/net/wire_protocol.h"

namespace dzIPC::net
{
inline constexpr std::size_t kLocalHeaderSize = 40, kOutboxHeaderSize = 112, kLocalMaxSize = 8192;
enum class LocalKind : std::uint16_t
{
    Hello = 1,
    Welcome,
    AttachTx,
    TxReady,
    RegisterPub,
    PubRegistered,
    RegisterSub,
    SubRegistered,
    SubReady,
    SubReadyAck,
    Unregister,
    Unregistered,
    QueryState,
    State,
    SendResult = 17,
    Ping,
    Pong,
    Error,
    TxProgress,
    RouteState,
    CreditRequest,
    CreditGrant
};
enum class SendResultCode : std::uint32_t
{
    Completed,
    NoSubscribers,
    Busy,
    TimedOut,
    PeerGone,
    PeerRestarted,
    Rejected,
    GatewayLost,
    Cancelled,
    Indeterminate,
    UnsupportedTimeout
};
struct LocalHeader
{
    LocalKind kind = LocalKind::Hello;
    std::uint64_t request_id = 0, session_id = 0, gateway_epoch = 0;
};
ProtocolStatus encode_local(const LocalHeader &, ByteView body, Bytes &out);
ProtocolStatus decode_local(ByteView packet, LocalHeader &out, ByteView &body);

struct WelcomeBody
{
    Identity locality{};
    std::uint32_t max_message_bytes = kMaxMessageBytes, outbox_limit_bytes = 32 * 1024 * 1024,
                  outbox_record_limit = 256;
    std::uint64_t granted_bytes = 0, granted_records = 0;
    std::string tx_name;
};
std::string outbox_name(const Identity &locality, std::uint64_t gateway_epoch,
                        std::uint64_t session_id);
ProtocolStatus encode_welcome(const WelcomeBody &, Bytes &out);
ProtocolStatus decode_welcome(ByteView body, WelcomeBody &out);
struct CreditCounters
{
    std::uint64_t bytes = 0, records = 0;
};
Bytes encode_credit_grant(CreditCounters counters);
Bytes encode_tx_progress(CreditCounters counters);
ProtocolStatus decode_credit_grant(ByteView, CreditCounters &);
ProtocolStatus decode_tx_progress(ByteView, CreditCounters &);
struct RouteStateBody
{
    Identity publisher_id{};
    std::uint64_t state_version = 0;
    std::uint32_t remote_ready_count = 0;
    bool synchronized = false;
};
ProtocolStatus encode_route_state(const RouteStateBody &, Bytes &out);
ProtocolStatus decode_route_state(ByteView, RouteStateBody &);
struct SendResultBody
{
    Identity publisher_id{};
    std::uint64_t sequence = 0;
    SendResultCode result = SendResultCode::Completed;
    bool possible_remote_delivery = false;
    std::uint32_t target_count = 0, acked_count = 0;
};
ProtocolStatus encode_send_result(const SendResultBody &, Bytes &out);
ProtocolStatus decode_send_result(ByteView, SendResultBody &);

struct OutboxHeader
{
    std::uint64_t gateway_epoch = 0, session_id = 0;
    Identity publisher_id{};
    std::uint64_t sequence = 0;
    RouteKey route;
    std::uint32_t schema_hash = 0;
    Encoding encoding = Encoding::Tlv;
    Delivery delivery = Delivery::BestEffort;
    std::uint64_t request_id = 0, deadline_monotonic_ns = 0;
};
ProtocolStatus encode_outbox(const OutboxHeader &, ByteView blob, Bytes &out);
ProtocolStatus encode_outbox_into(const OutboxHeader &, ByteView blob, void *loan,
                                  std::size_t capacity);
// 输入为 loan 容量；只返回 payload_size 范围，填充字节不属于 blob。
ProtocolStatus decode_outbox(ByteView loan, OutboxHeader &out, ByteView &blob,
                             std::uint32_t max_message = kMaxMessageBytes);
} // namespace dzIPC::net
