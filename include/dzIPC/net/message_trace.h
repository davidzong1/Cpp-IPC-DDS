#pragma once

#include "dzIPC/net/wire_protocol.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dzIPC::net {

struct OutboxHeader;

struct MessageTraceKey {
    std::uint64_t gateway_epoch = 0;
    std::uint64_t session_id = 0;
    bool session_known = true;
    Identity publisher_id{};
    std::uint64_t sequence = 0;
    RouteKey route;
    bool reliable = false;
};

enum class MessageTracePoint : unsigned {
    PublishBegin,
    CreditWaitBegin,
    CreditWaitEnd,
    OutboxSubmitBegin,
    OutboxSubmitEnd,
    PublishApiReturn,
    DrainPulled,
    DrainEventQueued,
    ControlDequeued,
    ControlAccepted,
    DataQueueSubmitted,
    WorkerDequeued,
    FirstSend,
    LastSend,
    ReceiveFirst,
    ReceiveCommit,
    AckQueued,
    AckSent,
    AckReceived,
    ReliableResultQueued,
    ReliableCompleted,
    Count
};

struct MessageTracePage {
    std::string json;
    std::uint32_t next_offset = 0;
    bool complete = true;
};

inline constexpr std::size_t kMessageTraceCapacity = 16384;
inline constexpr std::size_t kMessageTracePageRecords = 6;

class MessageTraceRecorder {
    struct Slot {
        mutable std::atomic_flag lock = ATOMIC_FLAG_INIT;
        bool occupied = false;
        MessageTraceKey key;
        std::array<std::uint64_t, static_cast<unsigned>(MessageTracePoint::Count)> stamps{};
        std::uint64_t updated_ns = 0;
    };
    static constexpr std::uint64_t kStaleNs = 30000000000ull;
    std::unique_ptr<Slot[]> slots_;
    std::string node_;
    std::atomic<std::uint64_t> dropped_{0};

    static bool equal(const MessageTraceKey&, const MessageTraceKey&) noexcept;
    static std::size_t index(const MessageTraceKey&) noexcept;
    static bool begins(MessageTracePoint) noexcept;
    static bool terminal(const Slot&) noexcept;
    static std::string record_json(const Slot&);

  public:
    explicit MessageTraceRecorder(const char* node = "unknown");
    bool enabled() const noexcept { return static_cast<bool>(slots_); }
    void record(const MessageTraceKey&, MessageTracePoint,
                std::uint64_t stamp_ns = 0) noexcept;
    MessageTracePage page(std::uint32_t start_offset = 0,
                          std::size_t max_records = kMessageTracePageRecords) const;
};

MessageTraceKey trace_key(const OutboxHeader& header);
MessageTraceKey trace_key(const WireHeader& header, std::uint64_t session_id = 0,
                          bool session_known = false);

} // namespace dzIPC::net
