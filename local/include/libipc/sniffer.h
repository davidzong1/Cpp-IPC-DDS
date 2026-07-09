#pragma once

/**
 * \file libipc/sniffer.h
 *
 * Passive read-only hook into an ipc channel.
 *
 * Use case: external tools that want to tail messages flowing through a
 * shared-memory channel without disturbing the publisher (analogous to
 * `ros2 topic echo`). The sniffer attaches to the same shared-memory
 * regions a regular receiver would (QU_CONN__... and CHUNK_INFO__...),
 * but it does NOT register itself as a receiver:
 *
 *   - The publisher never waits for the sniffer.
 *   - Real subscribers are unaffected.
 *   - In exchange, a fast publisher can wrap the 256-slot ring before the
 *     sniffer copies a slot, in which case messages are reported as
 *     dropped (see ipc::sniffer::dropped()).
 *
 * Topology: the on-disk layout differs by topology, so the caller must
 * pass the same flag the publisher used (server / route). channel topology
 * is not yet supported and will be rejected at open() time.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "libipc/export.h"
#include "libipc/def.h"
#include "libipc/buffer.h"

namespace ipc {

// `buff_t` is also aliased in ipc.h, but sniffer.h is included BY ipc.h —
// so we declare the alias locally too. Identical `using` declarations are
// allowed at namespace scope.
using buff_t = buffer;

class IPC_EXPORT sniffer {
public:
    /// Channel topology. Must match what the publisher used to create the
    /// channel (e.g. ipc::server -> server, ipc::route -> route).
    enum class topology : unsigned {
        server  = 0, // single producer, single consumer, unicast
        route   = 1, // single producer, multi  consumer, broadcast
        channel = 2, // multi  producer, multi  consumer, broadcast (NOT supported yet)
    };

    /// Per-message metadata returned alongside the payload.
    struct meta {
        std::uint32_t cc_id   = 0; // sender's connection id (0 if unknown)
        std::uint32_t msg_id  = 0; // monotonic message id assigned by the producer
        std::uint64_t dropped = 0; // total dropped messages observed so far
    };

    sniffer() noexcept;
    sniffer(sniffer&& rhs) noexcept;
    ~sniffer();
    sniffer& operator=(sniffer rhs) noexcept;
    void swap(sniffer& rhs) noexcept;

    /// Attach to a channel by name. The default mode opens-or-creates the
    /// underlying SHM, exactly like a normal ipc::chan would, so calling
    /// open() before the publisher is fine.
    /// Returns false if topology is unsupported or the SHM cannot be mapped.
    bool open(char const* name, topology t = topology::route);
    bool open(prefix pref, char const* name, topology t = topology::route);

    void close() noexcept;
    bool valid() const noexcept;

    char const* name() const noexcept;        // channel name, or "" if not open
    char const* prefix_str() const noexcept;  // prefix, or "" if not open
    topology    get_topology() const noexcept;

    /// Bit-set of currently-registered receivers (lower 32 bits).
    /// Useful for "is anybody listening" diagnostics.
    std::size_t receiver_connections() const noexcept;

    /// Total dropped messages since open(). A drop is counted when the
    /// publisher has lapped our cursor by more than the ring length (256).
    std::uint64_t dropped() const noexcept;

    /// Discard any backlog and resume from the publisher's current write
    /// index. Equivalent to a fresh open() but cheaper.
    void skip_to_latest() noexcept;

    /// Try to read one message without blocking. Returns an empty buffer
    /// if there is no new message. Reassembles fragmented messages and
    /// resolves large-message storage chunks.
    buff_t try_recv(meta* out_meta = nullptr) noexcept;

    /// Block up to timeout_ms (default: forever) for the next message.
    /// Returns an empty buffer on timeout.
    buff_t recv(std::uint64_t timeout_ms = invalid_value,
                meta* out_meta = nullptr) noexcept;

private:
    class impl_t;
    impl_t* p_ = nullptr;
};

} // namespace ipc
