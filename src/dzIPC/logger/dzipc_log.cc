#include "dzIPC/logger/dzipc_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dzIPC/ipc_info_pool.h"

namespace dzIPC
{
namespace logger
{

// =========================================================================
// Internal ROS bag v2 binary helpers (inlined, no separate files)
// =========================================================================

namespace
{

constexpr const char* kBagHdr = "#ROSBAG V2.0\n";
constexpr size_t kBagHdrLen = 13;

// ROS bag v2 op codes (standard mapping):
//   Bag Header  = 0x03    Chunk       = 0x05
//   Connection  = 0x07    Chunk Info  = 0x06
//   Index Data  = 0x04    MessageData = 0x02 (embedded in chunk only)
constexpr uint8_t kOpBagHeader  = 0x03;
constexpr uint8_t kOpChunk      = 0x05;
constexpr uint8_t kOpConnection = 0x07;
constexpr uint8_t kOpChunkInfo  = 0x06;
constexpr uint8_t kOpIndexData  = 0x04;

constexpr const char* kTopicDzipcEvents = "/dzipc_events";
constexpr const char* kTypeTransportPkt = "dzipc_log/TransportPacket";
constexpr const char* kTransportPktMd5 = "8e09bafaf5945d951e3b1b219c73eb6c";
constexpr const char* kTransportPktDefStr =
    "uint64 timestamp\n"
    "string topic\n"
    "string type\n"
    "uint32 domain_id\n"
    "uint32 msg_id\n"
    "uint8 transport\n"
    "uint8 role\n"
    "uint8 event\n"
    "uint8[] payload\n";

uint64_t steady_ns_to_wall_ns()
{
    static auto wall_base = std::chrono::system_clock::now().time_since_epoch();
    static auto steady_base = std::chrono::steady_clock::now().time_since_epoch();
    auto elapsed = std::chrono::steady_clock::now() - steady_base;
    auto wall = wall_base + elapsed;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch()).count());
}

// --- LE helpers ---
inline void w8(std::vector<uint8_t>& d, uint8_t v) { d.push_back(v); }
inline void w32(std::vector<uint8_t>& d, uint32_t v)
{
    d.push_back(static_cast<uint8_t>(v));
    d.push_back(static_cast<uint8_t>(v >> 8));
    d.push_back(static_cast<uint8_t>(v >> 16));
    d.push_back(static_cast<uint8_t>(v >> 24));
}
inline void w64(std::vector<uint8_t>& d, uint64_t v)
{
    for (int i = 0; i < 8; ++i) d.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
inline void wstr(std::vector<uint8_t>& d, const std::string& s)
{
    w32(d, static_cast<uint32_t>(s.size()));
    if (!s.empty()) d.insert(d.end(), s.begin(), s.end());
}
inline void wbytes(std::vector<uint8_t>& d, const uint8_t* b, uint32_t len)
{
    w32(d, len);
    if (len) d.insert(d.end(), b, b + len);
}

// ROS bag record headers are a sequence of length-prefixed key=value fields.
inline void field(std::vector<uint8_t>& h, const std::string& key, const std::string& value)
{
    const std::string s = key + "=" + value;
    w32(h, static_cast<uint32_t>(s.size()));
    h.insert(h.end(), s.begin(), s.end());
}

inline void binary_field(std::vector<uint8_t>& h, const std::string& key, const uint8_t* value, size_t size)
{
    const std::string prefix = key + "=";
    w32(h, static_cast<uint32_t>(prefix.size() + size));
    h.insert(h.end(), prefix.begin(), prefix.end());
    h.insert(h.end(), value, value + size);
}

inline void field_u8(std::vector<uint8_t>& h, const std::string& key, uint8_t value)
{
    binary_field(h, key, &value, sizeof(value));
}

inline void field_u32(std::vector<uint8_t>& h, const std::string& key, uint32_t value)
{
    uint8_t b[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
                    static_cast<uint8_t>(value >> 24)};
    binary_field(h, key, b, sizeof(b));
}

inline void field_u64(std::vector<uint8_t>& h, const std::string& key, uint64_t value)
{
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(value >> (i * 8));
    binary_field(h, key, b, sizeof(b));
}

inline void write_ros_time(std::vector<uint8_t>& out, uint64_t ns)
{
    w32(out, static_cast<uint32_t>(ns / 1000000000ULL));
    w32(out, static_cast<uint32_t>(ns % 1000000000ULL));
}

inline void field_time(std::vector<uint8_t>& h, const std::string& key, uint64_t ns)
{
    std::vector<uint8_t> b;
    write_ros_time(b, ns);
    binary_field(h, key, b.data(), b.size());
}

TransportKind transport_from_entry_kind(info_pool::EntryKind kind)
{
    switch (kind)
    {
        case info_pool::EntryKind::SocketPub:
        case info_pool::EntryKind::SocketSub:
        case info_pool::EntryKind::SocketServer:
        case info_pool::EntryKind::SocketClient:
            return TransportKind::kSocket;
        default:
            return TransportKind::kShm;
    }
}

// --- Record wrapper ---
// Format: [header_len(4LE)] [header_data(hl bytes)] [data_len(4LE)] [data(dl bytes)]
void add_record(std::vector<uint8_t>& out, const std::vector<uint8_t>& header, const std::vector<uint8_t>& data)
{
    w32(out, static_cast<uint32_t>(header.size()));
    out.insert(out.end(), header.begin(), header.end());
    w32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

// --- Build connection record (op=0x05) ---
std::vector<uint8_t> build_conn(uint32_t conn_id, const std::string& topic, const std::string& type,
                                const std::string& md5, const std::string& def)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", kOpConnection);
    field_u32(hdr, "conn", conn_id);
    field(hdr, "topic", topic);
    std::vector<uint8_t> data;
    field(data, "type", type);
    field(data, "md5sum", md5);
    field(data, "message_definition", def);
    field(data, "callerid", "/dzipc_logger");
    field(data, "latching", "0");
    std::vector<uint8_t> rec;
    add_record(rec, hdr, data);
    return rec;
}

// --- Build bag header record (op=0x03) ---
std::vector<uint8_t> build_bag_header(uint64_t index_pos, uint32_t conn_count, uint32_t chunk_count)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", kOpBagHeader);
    field_u64(hdr, "index_pos", index_pos);
    field_u32(hdr, "conn_count", conn_count);
    field_u32(hdr, "chunk_count", chunk_count);
    constexpr size_t kFileHeaderRecordSize = 4096;
    const size_t fixed_overhead = sizeof(uint32_t) * 2 + hdr.size();
    std::vector<uint8_t> data(fixed_overhead < kFileHeaderRecordSize ? kFileHeaderRecordSize - fixed_overhead : 0,
                              static_cast<uint8_t>(' '));
    std::vector<uint8_t> rec;
    add_record(rec, hdr, data);
    return rec;
}

// --- Serialize TransportPacket (ROS1 binary format) ---
std::vector<uint8_t> serialize_transport_packet(uint64_t ts_ns, const std::string& topic, const std::string& type_name,
                                                uint32_t domain_id, uint32_t msg_id, uint8_t transport, uint8_t role,
                                                uint8_t event_kind, const uint8_t* payload, uint32_t payload_len)
{
    std::vector<uint8_t> d;
    w64(d, ts_ns);
    wstr(d, topic);
    wstr(d, type_name);
    w32(d, domain_id);
    w32(d, msg_id);
    w8(d, transport);
    w8(d, role);
    w8(d, event_kind);
    wbytes(d, payload, payload_len);
    return d;
}

// --- Build message-in-chunk sub-record ---
std::vector<uint8_t> build_msg_in_chunk(uint32_t conn_id, uint64_t ts, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> hdr;
    uint8_t conn_bytes[4] = {static_cast<uint8_t>(conn_id), static_cast<uint8_t>(conn_id >> 8),
                             static_cast<uint8_t>(conn_id >> 16), static_cast<uint8_t>(conn_id >> 24)};
    binary_field(hdr, "conn", conn_bytes, sizeof(conn_bytes));
    field_time(hdr, "time", ts);
    std::vector<uint8_t> out;
    add_record(out, hdr, payload);
    return out;
}

// --- Build a whole chunk record (op=0x05) ---
std::vector<uint8_t> build_chunk(const std::vector<uint8_t>& msg_data)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", kOpChunk);
    field(hdr, "compression", "none");
    field_u32(hdr, "size", static_cast<uint32_t>(msg_data.size()));
    std::vector<uint8_t> rec;
    add_record(rec, hdr, msg_data);
    return rec;
}

// --- Build index data record (op=0x07) ---
std::vector<uint8_t> build_index(uint32_t conn_id, const std::vector<std::pair<uint64_t, uint64_t>>& pairs)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", kOpIndexData);
    field_u32(hdr, "conn", conn_id);
    field_u32(hdr, "ver", 1);
    field_u32(hdr, "count", static_cast<uint32_t>(pairs.size()));
    std::vector<uint8_t> data;
    for (auto& p : pairs)
    {
        write_ros_time(data, p.first);
        w32(data, static_cast<uint32_t>(p.second));
    }
    std::vector<uint8_t> rec;
    add_record(rec, hdr, data);
    return rec;
}

// --- Build chunk info record (op=0x06) ---
std::vector<uint8_t> build_chunk_info(uint64_t chunk_pos, uint64_t chunk_start_ts, uint64_t chunk_end_ts,
                                      const std::vector<uint32_t>& conn_ids, const std::vector<uint32_t>& counts)
{
    size_t n = conn_ids.size();
    std::vector<uint8_t> data;
    for (size_t i = 0; i < n; ++i)
    {
        w32(data, conn_ids[i]);
        w32(data, counts[i]);
    }

    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", kOpChunkInfo);
    field_u32(hdr, "ver", 1);
    field_u64(hdr, "chunk_pos", chunk_pos);
    field_time(hdr, "start_time", chunk_start_ts);
    field_time(hdr, "end_time", chunk_end_ts);
    field_u32(hdr, "count", static_cast<uint32_t>(n));
    std::vector<uint8_t> rec;
    add_record(rec, hdr, data);
    return rec;
}

// --- Expand leading ~ in a path ---
std::string expand_tilde(const std::string& path)
{
    if (!path.empty() && path[0] == '~')
    {
        const char* home = std::getenv("HOME");
        if (home)
        {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

// --- Generate timestamped filename ---
std::string make_timestamped_filename(const std::string& base_path, uint64_t rotation_counter)
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm* tm_local = std::localtime(&now_time_t);

    std::ostringstream oss;
    oss << base_path << "dzipc_log_"
        << std::put_time(tm_local, "%Y%m%dT%H%M%S")
        << "." << std::setw(3) << std::setfill('0') << now_ms.count();
    if (rotation_counter > 0)
    {
        oss << "_" << rotation_counter;
    }
    oss << ".bag";
    return oss.str();
}

// =========================================================================
// Logger singleton
// =========================================================================

struct ChunkBuf
{
    uint64_t start_ts{0};
    uint64_t end_ts{0};
    std::vector<uint8_t> msg_data;
    std::vector<std::pair<uint64_t, uint64_t>> index_entries;
    uint32_t conn_counts[4]{};
};

// Overflow-safe helpers
inline size_t safe_mb_to_bytes(size_t mb)
{
    if (mb > std::numeric_limits<size_t>::max() / (1024 * 1024))
        return std::numeric_limits<size_t>::max();
    return mb * 1024 * 1024;
}

inline size_t safe_add_saturate(size_t a, size_t b)
{
    if (a > std::numeric_limits<size_t>::max() - b)
        return std::numeric_limits<size_t>::max();
    return a + b;
}

inline size_t safe_mul_saturate(size_t a, size_t b)
{
    if (a > 0 && b > std::numeric_limits<size_t>::max() / a)
        return std::numeric_limits<size_t>::max();
    return a * b;
}

class LoggerImpl
{
   public:
    static LoggerImpl& instance()
    {
        static LoggerImpl s;
        return s;
    }

    bool start(const std::string& path, size_t max_memory_mb, size_t max_queue_size,
               const RotationOptions& rotation)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (running_ || writer_.joinable()) return false;

        // Fill default first, then expand tilde
        base_path_ = path.empty() ? "~/.dzipc/log" : path;
        base_path_ = expand_tilde(base_path_);

        // If path ends with .bag, use it directly as the first file; strip
        // the .bag suffix to form the stem used for rotated filenames.
        has_explicit_first_ = false;
        if (base_path_.size() > 4 && base_path_.compare(base_path_.size() - 4, 4, ".bag") == 0)
        {
            has_explicit_first_ = true;
            base_path_.resize(base_path_.size() - 4);  // strip ".bag"
        }

        max_memory_ = (max_memory_mb > 0) ? safe_mb_to_bytes(max_memory_mb) : safe_mb_to_bytes(256);
        max_queue_ = (max_queue_size > 0) ? max_queue_size : 100000;
        max_file_size_ = (rotation.max_file_size_mb > 0) ? safe_mb_to_bytes(rotation.max_file_size_mb) : 0;
        max_duration_sec_ = rotation.max_duration_sec;

        running_ = true;
        { std::deque<LogEvent> empty; empty.swap(queue_); }
        current_chunk_ = ChunkBuf{};
        { std::vector<ChunkInfo> empty; empty.swap(chunks_); }
        index_pos_ = 0;
        bag_start_time_ = 0;
        bag_start_steady_ = {};
        rotation_in_progress_ = false;
        queue_rotation_armed_ = false;
        queue_rearm_blocked_ = false;
        queue_release_pending_ = false;
        rotation_counter_ = 0;
        dropped_count_ = 0;

        path_ = has_explicit_first_ ? (base_path_ + ".bag")
                                    : make_timestamped_filename(base_path_, rotation_counter_);
        out_.clear();
        out_.open(path_, std::ios::binary | std::ios::trunc);
        if (!out_.is_open())
        {
            running_ = false;
            return false;
        }
        out_.write(kBagHdr, static_cast<std::streamsize>(kBagHdrLen));

        bag_header_pos_ = out_.tellp();
        auto bh = build_bag_header(0, 1, 0);
        bag_header_size_ = bh.size();
        out_.write(reinterpret_cast<const char*>(bh.data()), static_cast<std::streamsize>(bh.size()));

        bytes_written_ = static_cast<size_t>(kBagHdrLen) + bh.size();

        try
        {
            record_endpoint_meta_locked();
            // If initial metadata snapshot pushed queue past base, arm immediately
            if (max_queue_ > 0 && queue_.size() >= max_queue_)
                queue_rotation_armed_ = true;
            writer_ = std::thread(&LoggerImpl::writer_loop, this);
        }
        catch (...)
        {
            running_ = false;
            { std::deque<LogEvent> empty; empty.swap(queue_); }
            out_.close();
            return false;
        }
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            running_ = false;
        }
        cv_.notify_all();
        if (writer_.joinable()) writer_.join();

        // Finalize last bag — writer has exited, safe for I/O here
        std::lock_guard<std::mutex> lk(m_);
        finalize_current_bag_locked();
        release_memory_locked();
    }

    bool is_running() const
    {
        return running_.load(std::memory_order_relaxed);
    }

    void record(LogEvent event)
    {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (event.timestamp_ns == 0) event.timestamp_ns = NowNs();
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!running_) return;

            // Augmented capacity (1.5x) when armed, rotating, or rearm-blocked
            bool augmented = queue_rotation_armed_ || rotation_in_progress_ || queue_rearm_blocked_;
            size_t eff_max = augmented ? augmented_queue_cap() : max_queue_;

            if (eff_max > 0 && queue_.size() >= eff_max)
            {
                dropped_count_++;
                return;
            }
            queue_.push_back(std::move(event));

            // Arm queue-depth trigger — but NOT while rearm-blocked
            if (max_queue_ > 0 && queue_.size() >= max_queue_ && !queue_rearm_blocked_)
            {
                queue_rotation_armed_ = true;
            }

            cv_.notify_one();
        }
    }

   private:
    LoggerImpl() = default;
    ~LoggerImpl()
    {
        stop();
    }

    LoggerImpl(const LoggerImpl&) = delete;
    LoggerImpl& operator=(const LoggerImpl&) = delete;

    // --- Augmented queue capacity: 1.5x base with overflow guard ---
    size_t augmented_queue_cap() const
    {
        if (max_queue_ > std::numeric_limits<size_t>::max() / 3 * 2)
            return std::numeric_limits<size_t>::max();
        return max_queue_ + (max_queue_ / 2);
    }

    // --- Low watermark for re-arm: max(1, max_queue_/2) to avoid stuck-at-zero ---
    size_t low_watermark() const
    {
        size_t half = max_queue_ / 2;
        return (half < 1 && max_queue_ >= 1) ? 1 : half;
    }

    // --- Serialize one event to a message-in-chunk (lock-free) ---
    std::vector<uint8_t> serialize_event_to_msg_rec(const LogEvent& event)
    {
        if (event.payload && event.payload->size() > std::numeric_limits<uint32_t>::max()) return {};

        auto pkt = serialize_transport_packet(
            event.timestamp_ns, event.topic, event.type_name, event.domain_id, event.msg_id,
            static_cast<uint8_t>(event.transport), static_cast<uint8_t>(event.role),
            static_cast<uint8_t>(event.event_kind),
            event.payload ? event.payload->data() : nullptr,
            event.payload ? static_cast<uint32_t>(event.payload->size()) : 0);

        return build_msg_in_chunk(0, event.timestamp_ns, pkt);
    }

    // --- Writer thread main loop ---
    void writer_loop()
    {
        while (true)
        {
            LogEvent event;
            bool got_event = false;
            {
                std::unique_lock<std::mutex> lk(m_);
                got_event = wait_for_event_or_deadline(lk);

                if (!running_ && queue_.empty()) break;

                if (!got_event)
                {
                    // Duration timeout with data in chunk → trigger rotation
                    if (should_rotate_locked())
                        do_rotate(lk);
                    maybe_unblock_rearm_locked();
                    continue;
                }

                event = std::move(queue_.front());
                queue_.pop_front();
            }

            try
            {
                auto msg_rec = serialize_event_to_msg_rec(event);
                if (msg_rec.empty()) continue;

                std::unique_lock<std::mutex> lk(m_);
                append_to_chunk_locked(event.timestamp_ns, std::move(msg_rec));

                if (should_rotate_locked())
                    do_rotate(lk);
                maybe_unblock_rearm_locked();
            }
            catch (...)
            {
                // Drop event that cannot be serialized or buffered.
            }
        }
        // Writer exiting: release remaining queue memory
        {
            std::lock_guard<std::mutex> lk(m_);
            { std::deque<LogEvent> empty; empty.swap(queue_); }
        }
    }

    // --- Wait for event or duration deadline (m_ held on entry/exit) ---
    bool wait_for_event_or_deadline(std::unique_lock<std::mutex>& lk)
    {
        auto pred = [this] { return !running_ || !queue_.empty(); };

        if (max_duration_sec_ > 0 && bag_start_steady_.time_since_epoch().count() > 0)
        {
            auto deadline = bag_start_steady_ + std::chrono::seconds(max_duration_sec_);
            cv_.wait_until(lk, deadline, pred);
        }
        else
        {
            cv_.wait(lk, pred);
        }

        if (!running_ && queue_.empty()) return false;
        return !queue_.empty();
    }

    // --- Append serialized message record to current chunk (m_ held) ---
    void append_to_chunk_locked(uint64_t ts_ns, std::vector<uint8_t> msg_rec)
    {
        if (current_chunk_.msg_data.empty())
        {
            current_chunk_.start_ts = ts_ns;
            if (bag_start_time_ == 0)
            {
                bag_start_time_ = ts_ns;
                bag_start_steady_ = std::chrono::steady_clock::now();
            }
        }
        current_chunk_.end_ts = ts_ns;
        current_chunk_.conn_counts[0]++;
        current_chunk_.index_entries.emplace_back(ts_ns, static_cast<uint64_t>(current_chunk_.msg_data.size()));
        current_chunk_.msg_data.insert(current_chunk_.msg_data.end(), msg_rec.begin(), msg_rec.end());
    }

    // --- Check rotation conditions (m_ held) ---
    bool should_rotate_locked()
    {
        if (current_chunk_.msg_data.empty()) return false;

        // 1. Memory budget
        if (current_chunk_.msg_data.size() >= max_memory_) return true;

        // 2. File size: projected with saturating arithmetic
        if (max_file_size_ > 0)
        {
            // overhead: chunk header ~30 + index entries*28 + chunk-info ~80
            size_t overhead = safe_add_saturate(
                30, safe_add_saturate(safe_mul_saturate(current_chunk_.index_entries.size(), 28), 80));
            size_t projected = safe_add_saturate(
                bytes_written_, safe_add_saturate(current_chunk_.msg_data.size(), overhead));
            if (projected >= max_file_size_) return true;
        }

        // 3. Duration (monotonic steady_clock)
        if (max_duration_sec_ > 0 && bag_start_steady_.time_since_epoch().count() > 0)
        {
            auto elapsed = std::chrono::steady_clock::now() - bag_start_steady_;
            if (static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()) >= max_duration_sec_)
                return true;
        }

        // 4. Queue depth: latched armed flag
        if (max_queue_ > 0 && queue_rotation_armed_) return true;

        return false;
    }

    // --- Flush current chunk to bag file (writer-only, m_ held but sole writer) ---
    size_t flush_current_chunk_locked()
    {
        if (current_chunk_.msg_data.empty()) return 0;
        if (!out_.is_open()) return 0;

        size_t written = 0;
        uint64_t offset = static_cast<uint64_t>(out_.tellp());

        auto chunk = build_chunk(current_chunk_.msg_data);
        out_.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        written += chunk.size();

        auto idx = build_index(0, current_chunk_.index_entries);
        out_.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
        written += idx.size();

        ChunkInfo ci;
        ci.chunk_pos = offset;
        ci.start_ts = current_chunk_.start_ts;
        ci.end_ts = current_chunk_.end_ts;
        for (int i = 0; i < 4; ++i) ci.conn_counts[i] = current_chunk_.conn_counts[i];
        ci.conn_ids = {0};
        chunks_.push_back(ci);

        current_chunk_ = ChunkBuf{};
        bytes_written_ += written;
        return written;
    }

    // --- Write tail records (writer only) ---
    void finalize_bag_index()
    {
        out_.flush();
        index_pos_ = static_cast<uint64_t>(out_.tellp());

        auto conn = build_conn(0, kTopicDzipcEvents, kTypeTransportPkt, kTransportPktMd5, kTransportPktDefStr);
        out_.write(reinterpret_cast<const char*>(conn.data()), static_cast<std::streamsize>(conn.size()));

        for (auto& ci : chunks_)
        {
            std::vector<uint32_t> counts;
            for (auto cid : ci.conn_ids) counts.push_back(ci.conn_counts[cid]);
            auto cirec = build_chunk_info(ci.chunk_pos, ci.start_ts, ci.end_ts, ci.conn_ids, counts);
            out_.write(reinterpret_cast<const char*>(cirec.data()), static_cast<std::streamsize>(cirec.size()));
        }
    }

    void rewrite_bag_header()
    {
        uint32_t chunk_count = static_cast<uint32_t>(chunks_.size());
        auto bh = build_bag_header(index_pos_, 1, chunk_count);
        if (bh.size() != bag_header_size_) return;
        out_.flush();
        out_.seekp(bag_header_pos_);
        out_.write(reinterpret_cast<const char*>(bh.data()), static_cast<std::streamsize>(bh.size()));
        out_.flush();
        out_.seekp(0, std::ios::end);
    }

    // --- Finalize: flush + tail + header + close (writer only) ---
    void finalize_current_bag()
    {
        if (!out_.is_open()) return;
        try
        {
            flush_current_chunk_locked();
            finalize_bag_index();
            rewrite_bag_header();
        }
        catch (...)
        {
            // Partial bag is preferable to crashing the application.
        }
        out_.close();
    }

    // --- Stop-time finalize (m_ held, writer already joined) ---
    void finalize_current_bag_locked()
    {
        finalize_current_bag();
    }

    // --- Rotation: all file I/O without m_ to avoid blocking record() ---
    void do_rotate(std::unique_lock<std::mutex>& lk)
    {
        rotation_in_progress_ = true;
        lk.unlock();

        // All file I/O without mutex — sole writer thread
        try
        {
            finalize_current_bag();

            rotation_counter_++;
            path_ = make_timestamped_filename(base_path_, rotation_counter_);
            out_.clear();
            out_.open(path_, std::ios::binary | std::ios::trunc);
        }
        catch (...)
        {
            lk.lock();
            running_ = false;
            rotation_in_progress_ = false;
            queue_rotation_armed_ = false;
            queue_rearm_blocked_ = false;
            release_memory_locked();
            return;
        }

        lk.lock();

        if (!out_.is_open())
        {
            // Open failed — stop logging, drain, writer will exit
            running_ = false;
            rotation_in_progress_ = false;
            queue_rotation_armed_ = false;
            queue_rearm_blocked_ = false;
            release_memory_locked();
            return;
        }

        // Write new bag header
        out_.write(kBagHdr, static_cast<std::streamsize>(kBagHdrLen));
        bag_header_pos_ = out_.tellp();
        auto bh = build_bag_header(0, 1, 0);
        bag_header_size_ = bh.size();
        out_.write(reinterpret_cast<const char*>(bh.data()), static_cast<std::streamsize>(bh.size()));

        // Swap to release old container allocations
        { std::vector<ChunkInfo> empty; empty.swap(chunks_); }
        { ChunkBuf empty; std::swap(current_chunk_, empty); }
        bytes_written_ = static_cast<size_t>(kBagHdrLen) + bh.size();
        bag_start_time_ = 0;
        bag_start_steady_ = {};
        rotation_in_progress_ = false;

        // Hysteresis: consume arm, block re-arm if backlog still >= low watermark
        queue_rotation_armed_ = false;
        if (queue_.size() >= low_watermark())
        {
            queue_rearm_blocked_ = true;
        }
        else
        {
            queue_rearm_blocked_ = false;
            if (queue_.empty())
            {
                std::deque<LogEvent> empty;
                empty.swap(queue_);
            }
        }

        // Schedule lazy deque release if pending events remain
        // (memory/file/duration rotation may leave events in queue)
        if (!queue_.empty())
            queue_release_pending_ = true;
    }

    // --- Clear rearm-blocked when queue drains below low watermark (m_ held) ---
    void maybe_unblock_rearm_locked()
    {
        // Unblock re-arm when queue drains below low watermark
        if (queue_rearm_blocked_ && queue_.size() < low_watermark())
        {
            queue_rearm_blocked_ = false;
            if (queue_.empty())
            {
                std::deque<LogEvent> empty;
                empty.swap(queue_);
            }
        }

        // Lazy deque release after non-queue-triggered rotation
        if (queue_release_pending_ && queue_.empty())
        {
            queue_release_pending_ = false;
            std::deque<LogEvent> empty;
            empty.swap(queue_);
        }
    }

    // --- Release all container memory via swap (m_ held) ---
    void release_memory_locked()
    {
        queue_rotation_armed_ = false;
        queue_rearm_blocked_ = false;
        queue_release_pending_ = false;
        { std::deque<LogEvent> empty; empty.swap(queue_); }
        { std::vector<ChunkInfo> empty; empty.swap(chunks_); }
        { ChunkBuf empty; std::swap(current_chunk_, empty); }
    }

    void record_endpoint_meta_locked()
    {
        auto entries = info_pool::IpcInfoPool::instance().snapshot(false);
        for (auto& e : entries)
        {
            if (!e.in_use) continue;
            LogEvent meta;
            meta.timestamp_ns = NowNs();
            meta.topic = e.topic_name;
            meta.type_name = e.type_name;
            meta.domain_id = static_cast<uint32_t>(e.domain_id);
            meta.msg_id = 0;
            meta.transport = transport_from_entry_kind(e.kind);
            auto k = e.kind;
            if (k == info_pool::EntryKind::ShmPub || k == info_pool::EntryKind::SocketPub)
                meta.role = RoleKind::kPublisher;
            else if (k == info_pool::EntryKind::ShmSub || k == info_pool::EntryKind::SocketSub)
                meta.role = RoleKind::kSubscriber;
            else if (k == info_pool::EntryKind::ShmClient || k == info_pool::EntryKind::SocketClient)
                meta.role = RoleKind::kClient;
            else if (k == info_pool::EntryKind::ShmServer || k == info_pool::EntryKind::SocketServer)
                meta.role = RoleKind::kServer;
            else
                continue;
            meta.event_kind = EventKind::kEndpointMeta;
            meta.payload = nullptr;
            queue_.push_back(std::move(meta));
        }
        cv_.notify_one();
    }

    // --- State ---
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread writer_;
    std::ofstream out_;
    std::string path_;
    std::string base_path_;
    bool has_explicit_first_{false};
    size_t max_memory_{safe_mb_to_bytes(256)};
    size_t max_queue_{100000};
    size_t max_file_size_{0};
    uint64_t max_duration_sec_{0};

    std::deque<LogEvent> queue_;
    ChunkBuf current_chunk_;

    struct ChunkInfo
    {
        uint64_t chunk_pos{0};
        uint64_t start_ts{0}, end_ts{0};
        std::vector<uint32_t> conn_ids;
        uint32_t conn_counts[4]{};
    };
    std::vector<ChunkInfo> chunks_;
    std::streampos bag_header_pos_{0};
    std::size_t bag_header_size_{0};
    uint64_t index_pos_{0};

    // Rotation state
    size_t bytes_written_{0};
    uint64_t bag_start_time_{0};
    std::chrono::steady_clock::time_point bag_start_steady_{};
    bool rotation_in_progress_{false};
    bool queue_rotation_armed_{false};   // arm: queue crossed base → rotate at next chance
    bool queue_rearm_blocked_{false};     // protect: prevent re-arm until backlog < half
    bool queue_release_pending_{false};   // lazy deque swap after non-queue rotation
    uint64_t rotation_counter_{0};
    size_t dropped_count_{0};
};

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool StartDzipcLog(const std::string& path, size_t max_memory_mb, size_t max_queue_size)
{
    RotationOptions defaults;
    return LoggerImpl::instance().start(path, max_memory_mb, max_queue_size, defaults);
}

bool StartDzipcLog(const std::string& path, size_t max_memory_mb, size_t max_queue_size,
                   const RotationOptions& rotation)
{
    return LoggerImpl::instance().start(path, max_memory_mb, max_queue_size, rotation);
}

void StopDzipcLog() { LoggerImpl::instance().stop(); }

bool IsDzipcLogRunning() { return LoggerImpl::instance().is_running(); }

void RecordEvent(LogEvent event) { LoggerImpl::instance().record(std::move(event)); }

void RecordPublish(const std::string& topic, const std::string& type_name, uint32_t domain_id, uint32_t msg_id,
                   TransportKind transport, const uint8_t* payload_raw, size_t payload_size)
{
    LogEvent ev;
    ev.timestamp_ns = NowNs();
    ev.topic = topic;
    ev.type_name = type_name;
    ev.domain_id = domain_id;
    ev.msg_id = msg_id;
    ev.transport = transport;
    ev.role = RoleKind::kPublisher;
    ev.event_kind = EventKind::kPublish;
    if (payload_size > 0 && payload_raw)
    {
        ev.payload = std::make_shared<std::vector<uint8_t>>(payload_raw, payload_raw + payload_size);
    }
    RecordEvent(std::move(ev));
}

void RecordRequest(const std::string& topic, const std::string& type_name, uint32_t domain_id, uint32_t msg_id,
                   TransportKind transport, const uint8_t* payload_raw, size_t payload_size)
{
    LogEvent ev;
    ev.timestamp_ns = NowNs();
    ev.topic = topic;
    ev.type_name = type_name;
    ev.domain_id = domain_id;
    ev.msg_id = msg_id;
    ev.transport = transport;
    ev.role = RoleKind::kClient;
    ev.event_kind = EventKind::kRequest;
    if (payload_size > 0 && payload_raw)
    {
        ev.payload = std::make_shared<std::vector<uint8_t>>(payload_raw, payload_raw + payload_size);
    }
    RecordEvent(std::move(ev));
}

void RecordResponse(const std::string& topic, const std::string& type_name, uint32_t domain_id, uint32_t msg_id,
                    TransportKind transport, const uint8_t* payload_raw, size_t payload_size)
{
    LogEvent ev;
    ev.timestamp_ns = NowNs();
    ev.topic = topic;
    ev.type_name = type_name;
    ev.domain_id = domain_id;
    ev.msg_id = msg_id;
    ev.transport = transport;
    ev.role = RoleKind::kServer;
    ev.event_kind = EventKind::kResponse;
    if (payload_size > 0 && payload_raw)
    {
        ev.payload = std::make_shared<std::vector<uint8_t>>(payload_raw, payload_raw + payload_size);
    }
    RecordEvent(std::move(ev));
}

void RecordEndpointMeta()
{
    auto entries = info_pool::IpcInfoPool::instance().snapshot(false);
    for (auto& e : entries)
    {
        if (!e.in_use) continue;
        LogEvent meta;
        meta.timestamp_ns = NowNs();
        meta.topic = e.topic_name;
        meta.type_name = e.type_name;
        meta.domain_id = static_cast<uint32_t>(e.domain_id);
        meta.msg_id = 0;
        meta.transport = transport_from_entry_kind(e.kind);
        auto k = e.kind;
        if (k == info_pool::EntryKind::ShmPub || k == info_pool::EntryKind::SocketPub)
            meta.role = RoleKind::kPublisher;
        else if (k == info_pool::EntryKind::ShmSub || k == info_pool::EntryKind::SocketSub)
            meta.role = RoleKind::kSubscriber;
        else if (k == info_pool::EntryKind::ShmClient || k == info_pool::EntryKind::SocketClient)
            meta.role = RoleKind::kClient;
        else if (k == info_pool::EntryKind::ShmServer || k == info_pool::EntryKind::SocketServer)
            meta.role = RoleKind::kServer;
        else
            continue;
        meta.event_kind = EventKind::kEndpointMeta;
        meta.payload = nullptr;
        RecordEvent(std::move(meta));
    }
}

uint64_t NowNs() { return steady_ns_to_wall_ns(); }

}  // namespace logger
}  // namespace dzIPC
