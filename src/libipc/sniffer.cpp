/**
 * libipc/sniffer.cpp - implementation of the passive read-only channel hook.
 *
 * Layout NOTE: the structs `msg_t` and `chunk_t` below MUST stay
 * byte-compatible with the same-named structs in ipc.cpp. static_asserts
 * below trip if alignment or field order ever drifts.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "libipc/sniffer.h"
#include "libipc/def.h"
#include "libipc/shm.h"
#include "libipc/buffer.h"
#include "libipc/pool_alloc.h"

#include "libipc/policy.h"
#include "libipc/queue.h"
#include "libipc/waiter.h"
#include "libipc/rw_lock.h"

#include "libipc/utility/id_pool.h"
#include "libipc/utility/log.h"
#include "libipc/utility/utility.h"

#include "libipc/memory/resource.h"
#include "libipc/circ/elem_array.h"

namespace {

using msg_id_t = std::uint32_t;

// -------- Layout mirrors of types defined in ipc.cpp --------
// Keep byte-identical to ipc.cpp; static_asserts below trip on drift.

template <std::size_t DataSize, std::size_t AlignSize>
struct msg_t;

template <std::size_t AlignSize>
struct msg_t<0, AlignSize> {
    msg_id_t     cc_id_;
    msg_id_t     id_;
    std::int32_t remain_;
    bool         storage_;
};

template <std::size_t DataSize, std::size_t AlignSize>
struct msg_t : msg_t<0, AlignSize> {
    std::aligned_storage_t<DataSize, AlignSize> data_{};
};

constexpr std::size_t kAlignSize =
    (ipc::detail::min<std::size_t>)(
        static_cast<std::size_t>(ipc::data_length),
        alignof(std::max_align_t));
using sniff_msg_t = msg_t<ipc::data_length, kAlignSize>;

// -------- Large-message chunk helpers (mirror ipc.cpp) --------

IPC_CONSTEXPR_ std::size_t align_chunk_size(std::size_t size) noexcept {
    return (((size - 1) / ipc::large_msg_align) + 1) * ipc::large_msg_align;
}

IPC_CONSTEXPR_ std::size_t calc_chunk_size(std::size_t size) noexcept {
    return ipc::make_align(
        alignof(std::max_align_t),
        align_chunk_size(ipc::make_align(alignof(std::max_align_t),
                                         sizeof(std::atomic<ipc::circ::cc_t>)) +
                         size));
}

struct chunk_t {
    void* data() noexcept {
        return reinterpret_cast<ipc::byte_t*>(this) +
               ipc::make_align(alignof(std::max_align_t),
                               sizeof(std::atomic<ipc::circ::cc_t>));
    }
};

struct chunk_info_t {
    ipc::id_pool<> pool_;
    ipc::spin_lock lock_;

    static std::size_t chunks_mem_size(std::size_t chunk_size) noexcept {
        return ipc::id_pool<>::max_count * chunk_size;
    }

    ipc::byte_t* chunks_mem() noexcept {
        return reinterpret_cast<ipc::byte_t*>(this + 1);
    }

    chunk_t* at(std::size_t chunk_size, ipc::storage_id_t id) noexcept {
        if (id < 0) return nullptr;
        return reinterpret_cast<chunk_t*>(chunks_mem() + (chunk_size * id));
    }
};

// -------- Sanity: check msg layout is what ipc.cpp would produce --------

static_assert(sizeof(msg_t<0, kAlignSize>::cc_id_)   == 4, "cc_id_ drift");
static_assert(sizeof(msg_t<0, kAlignSize>::id_)      == 4, "id_ drift");
static_assert(sizeof(msg_t<0, kAlignSize>::remain_)  == 4, "remain_ drift");
static_assert(std::is_standard_layout<msg_t<0, kAlignSize>>::value,
              "msg_t<0,...> must be standard-layout");
static_assert(sizeof(sniff_msg_t) >= sizeof(msg_t<0, kAlignSize>) + ipc::data_length,
              "sniff_msg_t total size too small");
static_assert(alignof(sniff_msg_t) == kAlignSize ||
              alignof(sniff_msg_t) == alignof(std::max_align_t),
              "sniff_msg_t alignment unexpected");

// -------- Per-topology elem_array reader (passive) --------

struct cache_entry {
    std::size_t              fill = 0;
    std::vector<ipc::byte_t> buf;
};

struct reader_iface {
    virtual ~reader_iface() = default;
    virtual bool          valid() const noexcept = 0;
    virtual std::size_t   receiver_connections() const noexcept = 0;
    virtual std::uint32_t write_index() const noexcept = 0;
    /// Copy slot data at the given absolute counter into `out`.
    virtual void          copy_slot(std::uint32_t index, sniff_msg_t& out) = 0;
};

template <typename Flag>
struct typed_reader final : reader_iface {
    using policy_t = ipc::policy::choose<ipc::circ::elem_array, Flag>;
    using elems_t  = typename policy_t::template elems_t<sizeof(sniff_msg_t),
                                                          alignof(sniff_msg_t)>;
    using elem_t   = typename elems_t::elem_t;

    ipc::shm::handle h_;
    elems_t*         elems_ = nullptr;

    ~typed_reader() {
        // Release the SHM handle without unlinking the name.
        // The sniffer is a passive observer — it must never delete the
        // publisher's SHM segment when it closes or reopens.  On POSIX,
        // release_no_unlink() only munmap/free/ref-decrement, skipping
        // shm_unlink entirely.
        h_.release_no_unlink();
    }

    bool open(char const* shm_name) {
        if (!h_.acquire(shm_name, sizeof(elems_t))) return false;
        elems_ = static_cast<elems_t*>(h_.get());
        if (elems_ == nullptr) return false;
        elems_->init();
        return true;
    }

    bool valid() const noexcept override { return elems_ != nullptr; }

    std::size_t receiver_connections() const noexcept override {
        if (elems_ == nullptr) return 0;
        return static_cast<std::size_t>(
            elems_->connections(std::memory_order_acquire));
    }

    std::uint32_t write_index() const noexcept override {
        return elems_ ? static_cast<std::uint32_t>(elems_->write_index()) : 0u;
    }

    void copy_slot(std::uint32_t index, sniff_msg_t& out) override {
        elem_t const* block = elems_->block();
        std::memcpy(&out,
                    &block[ipc::circ::index_of(index)].data_,
                    sizeof(out));
    }
};

} // namespace

namespace ipc {

class sniffer::impl_t {
public:
    ipc::string  prefix_;
    ipc::string  name_;
    sniffer::topology topo_ = sniffer::topology::route;

    std::unique_ptr<reader_iface> reader_;
    std::uint32_t                 cur_     = 0;
    bool                          primed_  = false;
    std::uint64_t                 dropped_ = 0;

    // Lazy-loaded per-chunk-size storage handles for large messages.
    ipc::map<std::size_t, ipc::shm::handle> chunk_handles_;

    // Fragment state must live across try_recv_one() calls. recv() may be
    // woken after any fragment, while the rest of the message is still being
    // published.
    ipc::map<msg_id_t, cache_entry> frags_;

    // Optional waiter for blocking recv(); opened lazily.
    ipc::detail::waiter rd_waiter_;

    bool open(ipc::string pref, ipc::string nm, sniffer::topology t) {
        close();
        prefix_ = std::move(pref);
        name_   = std::move(nm);
        topo_   = t;

        // The shm name uses the queue_generator's template parameters
        // (data_length, kAlignSize), NOT sizeof(msg_t)/alignof(msg_t).
        // See ipc.cpp::queue_generator<...,DataSize,AlignSize>::conn_info_t::init().
        ipc::string shm_name = ipc::make_prefix(
            prefix_, {"QU_CONN__", name_, "__",
                      ipc::to_string(static_cast<std::size_t>(ipc::data_length)),
                      "__",
                      ipc::to_string(kAlignSize)});

        switch (t) {
        case sniffer::topology::server: {
            using flag_t = ipc::wr<relat::single, relat::single, trans::unicast>;
            auto r = std::unique_ptr<typed_reader<flag_t>>(new typed_reader<flag_t>);
            if (!r->open(shm_name.c_str())) return false;
            reader_ = std::move(r);
            break;
        }
        case sniffer::topology::route: {
            using flag_t = ipc::wr<relat::single, relat::multi, trans::broadcast>;
            auto r = std::unique_ptr<typed_reader<flag_t>>(new typed_reader<flag_t>);
            if (!r->open(shm_name.c_str())) return false;
            reader_ = std::move(r);
            break;
        }
        case sniffer::topology::channel:
            ipc::error("sniffer: 'channel' topology is not supported yet\n");
            return false;
        default:
            ipc::error("sniffer: unknown topology %u\n",
                       static_cast<unsigned>(t));
            return false;
        }

        // Open the rd_waiter lazily — the first call to recv() will open it.
        // This ensures that try_recv()-only users (e.g. dzplot polling loop)
        // never open the waiter SHM at all, avoiding its close() path
        // entirely.

        primed_ = false;
        cur_ = 0;
        dropped_ = 0;
        frags_.clear();
        return true;
    }

    void close() noexcept {
        frags_.clear();
        // Release chunk handles without shm_unlink — the sniffer must never
        // delete the publisher's SHM segments.
        for (auto& kv : chunk_handles_) {
            kv.second.release_no_unlink();
        }
        chunk_handles_.clear();
        rd_waiter_.close();
        reader_.reset();
        prefix_.clear();
        name_.clear();
        primed_ = false;
        dropped_ = 0;
    }

    /// Resolve a large-message storage chunk and copy its payload out.
    /// Returns empty buff_t on failure.
    buff_t fetch_storage(ipc::storage_id_t id, std::size_t msg_size) {
        std::size_t chunk_size = calc_chunk_size(msg_size);
        auto it = chunk_handles_.find(chunk_size);
        if (it == chunk_handles_.end()) {
            ipc::shm::handle h;
            ipc::string shm_name = ipc::make_prefix(
                prefix_,
                {"CHUNK_INFO__", ipc::to_string(chunk_size)});
            if (!h.acquire(shm_name.c_str(),
                           sizeof(chunk_info_t) +
                               chunk_info_t::chunks_mem_size(chunk_size))) {
                return {};
            }
            it = chunk_handles_.emplace(chunk_size, std::move(h)).first;
        }
        auto* info = static_cast<chunk_info_t*>(it->second.get());
        if (info == nullptr) return {};
        chunk_t* chunk = info->at(chunk_size, id);
        if (chunk == nullptr) return {};

        // Copy out into an owned buffer. We do not (and must not) recycle
        // the storage slot — that is the producer's bookkeeping.
        auto* mem = static_cast<ipc::byte_t*>(ipc::mem::alloc(msg_size));
        if (mem == nullptr) return {};
        std::memcpy(mem, chunk->data(), msg_size);
        return buff_t{mem, msg_size, ipc::mem::free};
    }

    /// Walk the ring from cur_ to write_index() trying to assemble exactly
    /// one full message. Returns empty buff_t when there is nothing new
    /// (the caller decides whether to wait).
    buff_t try_recv_one(sniffer::meta* out_meta) {
        if (!reader_ || !reader_->valid()) return {};

        std::uint32_t wt = reader_->write_index();
        if (!primed_) {
            cur_    = wt;            // start fresh — never replay history
            primed_ = true;
            return {};
        }

        // Lap detection: if the writer has wrapped past us, drop all pending
        // slots. The current fragment format has no start marker, so resuming
        // from the oldest visible slot could assemble a suffix as a full
        // message after the first fragment has been overwritten.
        constexpr std::uint32_t ring =
            static_cast<std::uint32_t>(ipc::sniffer_ring_slots);
        std::uint32_t lag = wt - cur_; // intentional unsigned wrap
        if (lag > ring) {
            dropped_ += lag;
            cur_ = wt;
            frags_.clear();
        }

        while (cur_ != reader_->write_index()) {
            sniff_msg_t   msg{};
            std::uint32_t this_idx = cur_;
            reader_->copy_slot(this_idx, msg);
            std::uint32_t after_wt = reader_->write_index();
            ++cur_;

            // If the writer lapped this slot while we were copying, the
            // bytes may be torn. Treat as drop and advance.
            if ((after_wt - this_idx) > ring) {
                dropped_++;
                frags_.clear();
                continue;
            }

            std::int32_t r_size =
                static_cast<std::int32_t>(ipc::data_length) + msg.remain_;
            if (r_size <= 0) {
                continue; // uninitialized or torn slot
            }
            std::size_t msg_size = static_cast<std::size_t>(r_size);

            // ---- Large message: data_ is a storage_id_t into chunk pool.
            if (msg.storage_) {
                ipc::storage_id_t sid =
                    *reinterpret_cast<ipc::storage_id_t*>(&msg.data_);
                buff_t out = fetch_storage(sid, msg_size);
                if (!out.empty()) {
                    if (out_meta) {
                        out_meta->cc_id   = msg.cc_id_;
                        out_meta->msg_id  = msg.id_;
                        out_meta->dropped = dropped_;
                    }
                    return out;
                }
                continue;
            }

            // ---- Inline / fragmented message.
            auto it = frags_.find(msg.id_);
            if (msg_size <= ipc::data_length && it == frags_.end()) {
                // Single-fragment message — copy and return.
                auto* mem = static_cast<ipc::byte_t*>(ipc::mem::alloc(msg_size));
                if (mem == nullptr) continue;
                std::memcpy(mem, &msg.data_, msg_size);
                if (out_meta) {
                    out_meta->cc_id   = msg.cc_id_;
                    out_meta->msg_id  = msg.id_;
                    out_meta->dropped = dropped_;
                }
                return buff_t{mem, msg_size, ipc::mem::free};
            }

            // Multi-fragment: msg_size is the *remaining* bytes including
            // this fragment. Reassemble keyed by msg.id_.
            if (it == frags_.end()) {
                cache_entry e;
                e.buf.resize(msg_size);
                std::memcpy(e.buf.data(), &msg.data_, ipc::data_length);
                e.fill = ipc::data_length;
                frags_.emplace(msg.id_, std::move(e));
            }
            else {
                auto& ce = it->second;
                std::size_t take = (msg.remain_ <= 0)
                    ? msg_size
                    : static_cast<std::size_t>(ipc::data_length);
                if (ce.fill + take > ce.buf.size()) {
                    take = ce.buf.size() - ce.fill;
                }
                std::memcpy(ce.buf.data() + ce.fill, &msg.data_, take);
                ce.fill += take;
                if (msg.remain_ <= 0 || ce.fill >= ce.buf.size()) {
                    auto* mem = static_cast<ipc::byte_t*>(
                        ipc::mem::alloc(ce.buf.size()));
                    if (mem == nullptr) {
                        frags_.erase(it);
                        return {};
                    }
                    std::memcpy(mem, ce.buf.data(), ce.buf.size());
                    std::size_t sz = ce.buf.size();
                    if (out_meta) {
                        out_meta->cc_id   = msg.cc_id_;
                        out_meta->msg_id  = msg.id_;
                        out_meta->dropped = dropped_;
                    }
                    frags_.erase(it);
                    return buff_t{mem, sz, ipc::mem::free};
                }
            }
        }
        return {};
    }
};

// -------- Public API forwarding --------

sniffer::sniffer() noexcept = default;

sniffer::sniffer(sniffer&& rhs) noexcept : sniffer() { swap(rhs); }

sniffer::~sniffer() {
    if (p_ != nullptr) {
        delete p_;
        p_ = nullptr;
    }
}

sniffer& sniffer::operator=(sniffer rhs) noexcept {
    swap(rhs);
    return *this;
}

void sniffer::swap(sniffer& rhs) noexcept { std::swap(p_, rhs.p_); }

bool sniffer::open(char const* name, topology t) {
    return open(prefix{nullptr}, name, t);
}

bool sniffer::open(prefix pref, char const* name, topology t) {
    if (name == nullptr || name[0] == '\0') return false;
    if (p_ == nullptr) p_ = new impl_t();
    return p_->open(ipc::make_string(pref.str), ipc::make_string(name), t);
}

void sniffer::close() noexcept {
    if (p_ != nullptr) p_->close();
}

bool sniffer::valid() const noexcept {
    return p_ != nullptr && p_->reader_ && p_->reader_->valid();
}

char const* sniffer::name() const noexcept {
    return (p_ != nullptr) ? p_->name_.c_str() : "";
}

char const* sniffer::prefix_str() const noexcept {
    return (p_ != nullptr) ? p_->prefix_.c_str() : "";
}

sniffer::topology sniffer::get_topology() const noexcept {
    return (p_ != nullptr) ? p_->topo_ : topology::route;
}

std::size_t sniffer::receiver_connections() const noexcept {
    if (p_ == nullptr || !p_->reader_) return 0;
    return p_->reader_->receiver_connections();
}

std::uint64_t sniffer::dropped() const noexcept {
    return (p_ != nullptr) ? p_->dropped_ : 0;
}

void sniffer::skip_to_latest() noexcept {
    if (p_ == nullptr) return;
    p_->primed_ = false;
    p_->frags_.clear();
}

buff_t sniffer::try_recv(meta* out_meta) noexcept {
    if (p_ == nullptr) return {};
    return p_->try_recv_one(out_meta);
}

buff_t sniffer::recv(std::uint64_t timeout_ms, meta* out_meta) noexcept {
    if (p_ == nullptr) return {};

    auto first = p_->try_recv_one(out_meta);
    if (!first.empty()) return first;

    // Lazy-open the waiter on first blocking call.
    // try_recv()-only users (e.g. dzplot polling) never reach here.
    if (!p_->rd_waiter_.valid()) {
        p_->rd_waiter_.open(
            ipc::make_prefix(p_->prefix_, {"RD_CONN__", p_->name_}).c_str());
        // Failure is non-fatal — we fall back to short-sleep polling below.
    }

    auto t0 = std::chrono::steady_clock::now();
    auto deadline_reached = [&]() {
        if (timeout_ms == ipc::invalid_value) return false;
        auto now = std::chrono::steady_clock::now();
        return now - t0 >= std::chrono::milliseconds(timeout_ms);
    };

    while (true) {
        // Prefer the futex-style waiter when available — it's pinged by
        // every sender on push.
        if (p_->rd_waiter_.valid()) {
            std::uint64_t slice = (timeout_ms == ipc::invalid_value)
                ? 100ull
                : (std::min<std::uint64_t>)(100ull, timeout_ms);
            p_->rd_waiter_.wait_if(
                [&] {
                    return p_->reader_ && p_->reader_->valid() &&
                           p_->cur_ == p_->reader_->write_index();
                },
                slice);
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto b = p_->try_recv_one(out_meta);
        if (!b.empty()) return b;
        if (deadline_reached()) return {};
    }
}

} // namespace ipc
