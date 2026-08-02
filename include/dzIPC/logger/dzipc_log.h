#pragma once
/// \file dzipc_log.h
/// \brief dzIPC logging module that records IPC events to ROS bag v2 files.
///
/// Usage:
///   dzIPC::logger::StartDzipcLog("/tmp/events.bag", 256, 100000);
///   // ... run IPC operations ...
///   dzIPC::logger::StopDzipcLog();   // flushes + joins writer thread
///
/// The module captures publishes, requests, responses, and IpcInfoPool
/// endpoint metadata.  Events are buffered in memory and written to a
/// single .bag file on Stop().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libipc/export.h"

namespace dzIPC
{
namespace logger
{

// ---------------------------------------------------------------------------
// Event types captured by the logger
// ---------------------------------------------------------------------------

enum class EventKind : uint8_t
{
    kPublish = 0,
    kRequest = 1,
    kResponse = 2,
    kEndpointMeta = 3,
};

enum class RoleKind : uint8_t
{
    kPublisher = 0,
    kSubscriber = 1,
    kClient = 2,
    kServer = 3,
};

enum class TransportKind : uint8_t
{
    kShm = 0,
    kSocket = 1,
};

/// All data needed to record one event.  The logger takes ownership of
/// the payload buffer.  A zero timestamp_ns is replaced with NowNs().
struct LogEvent
{
    uint64_t timestamp_ns{0};
    std::string topic;
    std::string type_name;
    uint32_t domain_id{0};
    uint32_t msg_id{0};
    TransportKind transport{TransportKind::kShm};
    RoleKind role{RoleKind::kPublisher};
    EventKind event_kind{EventKind::kPublish};
    /// Serialized message payload; may be empty for metadata events.
    std::shared_ptr<std::vector<uint8_t>> payload;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Start the background logging thread.
/// @param path           Output .bag file path (overwritten if exists).
/// @param max_memory_mb  Soft memory budget before flushing a chunk (default 256 MiB).
/// @param max_queue_size Max pending events before dropping (default 100k).
/// @return true on success, false if already running or file cannot be opened.
IPC_EXPORT bool StartDzipcLog(const std::string& path, size_t max_memory_mb = 256, size_t max_queue_size = 100000);

/// Stop the logger, flush remaining events, join the writer thread, and
/// finalize the .bag file.  Idempotent and safe to call multiple times.
IPC_EXPORT void StopDzipcLog();

/// Query whether the logger is currently running.
IPC_EXPORT bool IsDzipcLogRunning();

/// Push a single event into the logger queue.  Thread-safe.
/// Non-blocking; if the queue is full the event is silently dropped.
IPC_EXPORT void RecordEvent(LogEvent event);

/// Convenience: build and record a publish event.
IPC_EXPORT void RecordPublish(const std::string& topic, const std::string& type_name, uint32_t domain_id,
                              uint32_t msg_id, TransportKind transport, const uint8_t* payload_raw,
                              size_t payload_size);

/// Convenience: build and record a service request event.
IPC_EXPORT void RecordRequest(const std::string& topic, const std::string& type_name, uint32_t domain_id,
                              uint32_t msg_id, TransportKind transport, const uint8_t* payload_raw,
                              size_t payload_size);

/// Convenience: build and record a service response event.
IPC_EXPORT void RecordResponse(const std::string& topic, const std::string& type_name, uint32_t domain_id,
                               uint32_t msg_id, TransportKind transport, const uint8_t* payload_raw,
                               size_t payload_size);

/// Record endpoint metadata entries from the current IpcInfoPool snapshot.
/// Called automatically by StartDzipcLog().
IPC_EXPORT void RecordEndpointMeta();

/// Returns current time as nanoseconds since epoch (wall clock).
IPC_EXPORT uint64_t NowNs();

}  // namespace logger
}  // namespace dzIPC
