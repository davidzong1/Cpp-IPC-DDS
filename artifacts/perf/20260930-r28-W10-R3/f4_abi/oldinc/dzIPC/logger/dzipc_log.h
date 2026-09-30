#pragma once
/// \file dzipc_log.h
/// \brief dzIPC logging module that records IPC events to ROS bag v2 files.
///
/// Quick start:
///   dzIPC::logger::StartDzipcLog("/tmp/events", 256, 100000);
///   // ... run IPC operations ...
///   dzIPC::logger::StopDzipcLog();   // flushes + joins writer thread
///
/// Rotation (bag file splitting):
///   dzIPC::logger::RotationOptions opts;
///   opts.max_file_size_mb  = 1024;   // split after 1 GiB
///   opts.max_duration_sec  = 3600;   // split every hour
///   dzIPC::logger::StartDzipcLog("/tmp/events", 256, 100000, opts);
///
/// Any of the four thresholds - max_memory_mb, max_queue_size, max_file_size_mb,
/// or max_duration_sec - triggers a bag rotation (finalize current file + open new
/// one).  Rotation is handled by the writer thread; file close/open runs without
/// the queue mutex so RecordEvent remains non-blocking.
///
/// Events are buffered in memory and written to timestamped .bag files.
/// Each bag is independently valid and readable by standard ROS tools.

#include <cstddef>
#include <cstdint>
#include <limits>
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
// Rotation configuration
// ---------------------------------------------------------------------------

/// Controls automatic bag-file rotation driven by file size or elapsed time.
/// Any threshold > 0 enables that trigger.  When a trigger fires the current
/// bag is finalized (header/index/connection records written), closed, and a
/// new timestamped bag is opened.  Multiple triggers firing simultaneously
/// produce a single rotation.
///
/// Default (all-zero) disables size and duration rotation; memory and queue
/// triggers from StartDzipcLog still apply independently.
struct RotationOptions
{
    /// Rotate when the current bag file exceeds this many MiB (0 = disabled).
    size_t max_file_size_mb = 0;

    /// Rotate when the current bag has been open longer than this many
    /// seconds (0 = disabled).
    uint64_t max_duration_sec = 0;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Start the background logging thread (original three-parameter form).
/// @param path           Output .bag file path or directory prefix.
/// @param max_memory_mb  Memory budget before rotating (default 512 MiB; explicit 0 falls back to 256 MiB).
/// @param max_queue_size Max pending events before triggering rotation + augmented capacity (default unlimited).
/// @return true on success, false if already running or file cannot be opened.
IPC_EXPORT bool StartDzipcLog(const std::string& path, size_t max_memory_mb = 512,
                              size_t max_queue_size = std::numeric_limits<size_t>::max());

/// Start the background logging thread with rotation options.
/// @param path           Output .bag file path or directory prefix.
/// @param max_memory_mb  Memory budget before rotating (explicit 0 falls back to 256 MiB).
/// @param max_queue_size Max pending events before triggering rotation + augmented capacity (0 = default 100k).
/// @param rotation       File-size and duration rotation controls.
/// @return true on success, false if already running or file cannot be opened.
IPC_EXPORT bool StartDzipcLog(const std::string& path, size_t max_memory_mb, size_t max_queue_size,
                              const RotationOptions& rotation);

/// Stop the logger, flush remaining events, join the writer thread, and
/// finalize the current .bag file.  Idempotent and safe to call multiple times.
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
