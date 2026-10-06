#include "dzIPC/net/message_trace.h"
#include "dzIPC/net/local_protocol.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace dzIPC::net {
namespace {
constexpr const char* kPointNames[] = {
    "publish_begin", "credit_wait_begin", "credit_wait_end", "outbox_submit_begin",
    "outbox_submit_end", "publish_api_return", "drain_pulled", "drain_event_queued",
    "control_dequeued", "control_accepted", "data_queue_submitted", "worker_dequeued",
    "first_send", "last_send", "receive_first", "receive_commit", "ack_queued",
    "ack_sent", "ack_received", "reliable_result_queued", "reliable_completed"};

template <std::size_t N>
std::string hex(const std::array<std::uint8_t, N>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value[i * 2] = digits[bytes[i] >> 4];
        value[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return value;
}
}

MessageTraceRecorder::MessageTraceRecorder(const char* node) : node_(node ? node : "unknown") {
    const char* setting = std::getenv("DZIPC_NET_TRACE");
    if (setting && setting[0] == '1' && setting[1] == '\0')
        slots_.reset(new Slot[kMessageTraceCapacity]);
}

bool MessageTraceRecorder::equal(const MessageTraceKey& a, const MessageTraceKey& b) noexcept {
    return a.gateway_epoch == b.gateway_epoch && a.session_id == b.session_id &&
        a.session_known == b.session_known && a.publisher_id == b.publisher_id &&
        a.sequence == b.sequence && a.route == b.route && a.reliable == b.reliable;
}

std::size_t MessageTraceRecorder::index(const MessageTraceKey& key) noexcept {
    std::uint64_t value = route_hash(key.route) ^ key.gateway_epoch ^ (key.sequence * 0x9e3779b97f4a7c15ull);
    value ^= key.session_id + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    for (const auto byte : key.publisher_id)
        value = (value ^ byte) * 1099511628211ull;
    value ^= static_cast<std::uint64_t>(key.session_known) << 61;
    value ^= static_cast<std::uint64_t>(key.reliable) << 62;
    return static_cast<std::size_t>(value % kMessageTraceCapacity);
}

bool MessageTraceRecorder::begins(MessageTracePoint point) noexcept {
    return point == MessageTracePoint::PublishBegin || point == MessageTracePoint::DrainPulled ||
        point == MessageTracePoint::ReceiveFirst;
}

bool MessageTraceRecorder::terminal(const Slot& slot) noexcept {
    const auto at = [&](MessageTracePoint point) { return slot.stamps[static_cast<unsigned>(point)] != 0; };
    return at(MessageTracePoint::PublishApiReturn) || at(MessageTracePoint::ReliableCompleted) ||
        at(MessageTracePoint::ReliableResultQueued) ||
        (!slot.key.reliable && (at(MessageTracePoint::LastSend) || at(MessageTracePoint::ReceiveCommit)));
}

std::string MessageTraceRecorder::record_json(const Slot& slot) {
    std::ostringstream out;
    out << "{\"gateway_epoch\":\"" << slot.key.gateway_epoch << "\",\"session_id\":";
    if (slot.key.session_known) out << '"' << slot.key.session_id << '"'; else out << "null";
    out << ",\"session_known\":" << (slot.key.session_known ? "true" : "false")
        << ",\"publisher_id\":\"" << hex(slot.key.publisher_id)
        << "\",\"sequence\":\"" << slot.key.sequence
        << "\",\"route_scope\":\"" << hex(slot.key.route.scope)
        << "\",\"msg_id\":" << slot.key.route.msg_id
        << ",\"reliable\":" << (slot.key.reliable ? "true" : "false") << ",\"stamps_ns\":{";
    for (unsigned i = 0; i < static_cast<unsigned>(MessageTracePoint::Count); ++i) {
        if (i) out << ',';
        out << '"' << kPointNames[i] << "\":" << slot.stamps[i];
    }
    out << "}}";
    return out.str();
}

void MessageTraceRecorder::record(const MessageTraceKey& key, MessageTracePoint point,
                                  std::uint64_t stamp_ns) noexcept {
    if (!slots_ || point >= MessageTracePoint::Count) return;
    if (!stamp_ns) {
        stamp_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    auto& slot = slots_[index(key)];
    if (slot.lock.test_and_set(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!slot.occupied || !equal(slot.key, key)) {
        const bool stale = slot.occupied && stamp_ns >= slot.updated_ns && stamp_ns - slot.updated_ns >= kStaleNs;
        if (!begins(point) || (slot.occupied && !stale && !terminal(slot))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            slot.lock.clear(std::memory_order_release);
            return;
        }
        slot.key = key;
        slot.stamps.fill(0);
        slot.occupied = true;
    }
    auto& value = slot.stamps[static_cast<unsigned>(point)];
    if (!value || point == MessageTracePoint::LastSend ||
        point == MessageTracePoint::CreditWaitEnd || point == MessageTracePoint::OutboxSubmitEnd ||
        point == MessageTracePoint::AckSent)
        value = stamp_ns;
    slot.updated_ns = stamp_ns;
    slot.lock.clear(std::memory_order_release);
}

MessageTracePage MessageTraceRecorder::page(std::uint32_t start_offset,
                                           std::size_t max_records) const {
    if (start_offset > kMessageTraceCapacity || !max_records || max_records > 16)
        throw std::invalid_argument("message trace page 范围无效");
    MessageTracePage result;
    std::ostringstream out;
    out << "{\"enabled\":" << (slots_ ? "true" : "false") << ",\"node\":\"" << node_
        << "\",\"capacity\":" << (slots_ ? kMessageTraceCapacity : 0)
        << ",\"dropped\":" << dropped_.load(std::memory_order_relaxed)
        << ",\"start_offset\":" << start_offset << ",\"records\":[";
    bool first = true;
    std::size_t written = 0;
    std::uint32_t offset = start_offset;
    if (slots_) {
        while (offset < kMessageTraceCapacity) {
            auto& slot = slots_[offset];
            while (slot.lock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
            if (slot.occupied) {
                if (!first) out << ',';
                first = false;
                out << record_json(slot);
                ++written;
            }
            slot.lock.clear(std::memory_order_release);
            ++offset;
            if (written >= max_records) break;
        }
    } else {
        offset = 0;
    }
    result.complete = offset >= kMessageTraceCapacity;
    result.next_offset = result.complete ? static_cast<std::uint32_t>(kMessageTraceCapacity) : offset;
    out << "],\"next_offset\":" << result.next_offset << ",\"complete\":"
        << (result.complete ? "true" : "false") << '}';
    result.json = out.str();
    return result;
}

MessageTraceKey trace_key(const OutboxHeader& header) {
    MessageTraceKey key;
    key.gateway_epoch = header.gateway_epoch;
    key.session_id = header.session_id;
    key.publisher_id = header.publisher_id;
    key.sequence = header.sequence;
    key.route = header.route;
    key.reliable = header.delivery == Delivery::Reliable;
    return key;
}

MessageTraceKey trace_key(const WireHeader& header, std::uint64_t session_id,
                          bool session_known) {
    MessageTraceKey key;
    key.gateway_epoch = header.source_epoch;
    key.session_id = session_id;
    key.session_known = session_known;
    key.publisher_id = header.publisher_id;
    key.sequence = header.sequence;
    key.route = header.route;
    key.reliable = header.delivery == Delivery::Reliable;
    return key;
}

} // namespace dzIPC::net
