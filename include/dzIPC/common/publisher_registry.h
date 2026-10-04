#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dzIPC {
namespace control_plane_shm {

/* Publisher registry is kept in a separate, versioned SHM object.  This lets
 * an old single-publisher process keep using TopicControl without interpreting
 * the publisher fields with a different layout. */
inline constexpr std::uint32_t kPublisherRegistryMagic = 0x445A5052U;
inline constexpr std::uint32_t kPublisherRegistryInitializing = 0x445A5049U;
inline constexpr std::uint32_t kPublisherRegistryLayout = 3U;
inline constexpr std::uint32_t kMaxPublisherSlots = 32U;

struct PublisherSlot {
    /* 0=free, 1=active, 2=initializing/reclaiming.  State 2 prevents a
     * concurrent join from observing half-written identity fields. */
    std::atomic<std::uint32_t> in_use{0};
    std::atomic<std::uint64_t> publisher_id{0};
    std::atomic<std::int32_t> pid{0};
    std::atomic<std::uint64_t> start_token{0};
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::int64_t> heartbeat_ns{0};
};

struct PublisherControl {
    std::atomic<std::uint32_t> magic{0};
    std::atomic<std::uint32_t> layout{0};
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint32_t> publisher_count{0};
    std::atomic<std::int32_t> coordinator_slot{-1};
    std::atomic<std::int64_t> coordinator_lease_ns{0};
    /* Short registry transactions are serialized with a recoverable
     * cross-process spin gate.  `closing` stays set after the final publisher
     * leaves until its owner has finished stopping the topic and deciding
     * whether the data segment can be cleared. */
    std::atomic<std::uint64_t> mutation_owner{0};
    std::atomic<std::int64_t> mutation_since_ns{0};
    std::atomic<std::int32_t> mutation_pid{0};
    std::atomic<std::uint64_t> mutation_start_token{0};
    std::atomic<std::uint32_t> closing{0};
    std::atomic<std::int64_t> closing_since_ns{0};
    std::atomic<std::int32_t> closing_pid{0};
    std::atomic<std::uint64_t> closing_start_token{0};
    PublisherSlot publishers[kMaxPublisherSlots];
};

class PublisherRegistry {
public:
    PublisherRegistry();
    ~PublisherRegistry();

    PublisherRegistry(const PublisherRegistry&) = delete;
    PublisherRegistry& operator=(const PublisherRegistry&) = delete;

    /* Open or create a registry segment.  The caller supplies a versioned
     * name, normally derived from the topic control name. */
    bool open(const std::string& name);
    bool valid() const noexcept { return control_ != nullptr; }

    /* Join is idempotent for the same (publisher_id, start_token) pair.  A
     * generation mismatch or a duplicate publisher_id with another token is
     * rejected instead of silently taking over an active slot. */
    int join(std::uint32_t generation, std::uint64_t publisher_id,
             std::int32_t pid, std::uint64_t start_token);
    bool heartbeat(int slot, std::uint64_t publisher_id,
                   std::uint64_t start_token);
    bool leave(int slot, std::uint64_t publisher_id,
               std::uint64_t start_token, bool* became_empty = nullptr);

    /* Complete the final-publisher close transaction.  A new publisher may
     * join only after this call, or after stale recovery proves the closer
     * died. */
    bool finish_close() noexcept;

    /* Return the number of slots removed because their heartbeat is older
     * than timeout_ns.  A stale coordinator lease is released with its slot. */
    std::uint32_t reap_stale(std::int64_t timeout_ns);

    /* Acquire or renew the single coordinator lease.  A live, unexpired lease
     * held by another publisher is never stolen. */
    bool acquire_coordinator(int slot, std::uint64_t publisher_id,
                             std::uint64_t start_token,
                             std::int64_t lease_ns);
    bool is_coordinator(int slot, std::uint64_t publisher_id,
                        std::uint64_t start_token) const;

    std::uint32_t generation() const noexcept;
    std::uint32_t publisher_count() const noexcept;
    std::int32_t coordinator_slot() const noexcept;
    std::uint64_t coordinator_publisher_id() const noexcept;

    /* Atomically install a generation.  The expected value is normally zero
     * for the first creator, or the last observed value when a coordinator
     * advances an existing topic. */
    bool compare_exchange_generation(std::uint32_t expected,
                                     std::uint32_t desired) noexcept;
    bool advance_generation(std::uint32_t expected,
                            std::uint32_t desired) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PublisherControl* control_{nullptr};

    bool identity_matches(int slot, std::uint64_t publisher_id,
                          std::uint64_t start_token) const noexcept;
    void clear_slot(PublisherSlot& slot) noexcept;
    static std::int64_t now_ns() noexcept;
    bool initialize_if_needed();
    bool lock_mutation() noexcept;
    void unlock_mutation() noexcept;
    bool recover_stale_close(std::int64_t now_ns) noexcept;
    std::uint64_t mutation_token() const noexcept;
    mutable std::atomic<std::uint64_t> mutation_identity_{0};
    mutable std::atomic<std::uint64_t> mutation_process_id_{0};
    bool closing_owner_{false};
    std::uint64_t closing_owner_process_id_{0};
};

} // namespace control_plane_shm
} // namespace dzIPC
