#include "dzIPC/logger/dzipc_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstdlib>
#include "dzIPC/ipc_info_pool.h"
#include <ctime>    
#include <iomanip>  
#include <sstream>  
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

// --- Build connection record ---
// header: op(1) + conn_id(4)
// data: "topic=...type=...md5sum=...message_definition=...callerid=...latching=..."
std::vector<uint8_t> build_conn(uint32_t conn_id, const std::string& topic, const std::string& type,
                                const std::string& md5, const std::string& def)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", 0x07);
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

// --- Build bag header record ---
// header: op(1=0x03) + conn_id(4=0)
// data: index_pos=...conn_count=...chunk_count=...
std::vector<uint8_t> build_bag_header(uint64_t index_pos, uint32_t conn_count, uint32_t chunk_count)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", 0x03);
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
// Fields in order: uint64 ts, string topic, string type, uint32 domain_id,
//   uint32 msg_id, uint8 transport, uint8 role, uint8 event, uint8[] payload
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
// Inner message headers use binary conn/time field values.
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

// --- Build a whole chunk record ---
// header: op(5) + conn(0) + ts(chunk_start)
// data:   "none"(4) + uncompressed_size(4) + msg_records_raw
std::vector<uint8_t> build_chunk(const std::vector<uint8_t>& msg_data)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", 0x05);
    field(hdr, "compression", "none");
    field_u32(hdr, "size", static_cast<uint32_t>(msg_data.size()));
    std::vector<uint8_t> rec;
    add_record(rec, hdr, msg_data);
    return rec;
}

// --- Build index data record ---
// header: op(4)+conn(4)+ts(8? no, record wrapper has ts but we put ver in data)
// data:   ver(4) + conn(4) + count(4) + entries(ts:8 + offset:8)*count
std::vector<uint8_t> build_index(uint32_t conn_id, const std::vector<std::pair<uint64_t, uint64_t>>& pairs)
{
    std::vector<uint8_t> hdr;
    field_u8(hdr, "op", 0x04);
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

// --- Build chunk info record ---
// header: op(6)+conn(0)+ts(chunk_start)
// data:   ver(4)+conn_count(4)+entries(conn:4+count:4+start_ts:8+end_ts:8)*n
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
    field_u8(hdr, "op", 0x06);
    field_u32(hdr, "ver", 1);
    field_u64(hdr, "chunk_pos", chunk_pos);
    field_time(hdr, "start_time", chunk_start_ts);
    field_time(hdr, "end_time", chunk_end_ts);
    field_u32(hdr, "count", static_cast<uint32_t>(n));
    std::vector<uint8_t> rec;
    add_record(rec, hdr, data);
    return rec;
}

// =========================================================================
// Logger singleton
// =========================================================================

struct ChunkBuf
{
    uint64_t start_ts{0};
    uint64_t end_ts{0};
    std::vector<uint8_t> msg_data;  // concatenated message records
    std::vector<std::pair<uint64_t, uint64_t>> index_entries;
    uint32_t conn_counts[4]{};  // per-connection message count
};

class LoggerImpl
{
   public:
    static LoggerImpl& instance()
    {
        static LoggerImpl s;
        return s;
    }

    bool start(const std::string& path, size_t max_memory_mb, size_t max_queue_size)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (running_) return false;

        path_ = path;
        max_memory_ = (max_memory_mb > 0) ? max_memory_mb * 1024 * 1024 : 256ULL * 1024 * 1024;
        max_queue_ = (max_queue_size > 0) ? max_queue_size : 100000;
        running_ = true;
        queue_.clear();
        current_chunk_ = ChunkBuf{};
        chunks_.clear();
        index_pos_ = 0;

        // Open file and write header
        out_.clear();
        if (path_.empty()) path_ = "~/.dzipc/log";

        {    
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) {
                    return std::string(home) + path.substr(1);
                }
            }
        }
        path_ = make_timestamped_filename(path_);

        out_.open(path_, std::ios::binary | std::ios::trunc);
        if (!out_.is_open())
        {
            running_ = false;
            return false;
        }
        out_.write(kBagHdr, static_cast<std::streamsize>(kBagHdrLen));

        // Write bag header as placeholder (we'll update at end)
        // For now just write a dummy bag header; real one will be at file end
        // Actually in ROS bag v2 the bag header record goes FIRST.
        // We'll write it with placeholder index_pos and rewrite at end.
        bag_header_pos_ = out_.tellp();
        auto bh = build_bag_header(0, 1, 0);  // one TransportPacket connection
        bag_header_size_ = bh.size();
        out_.write(reinterpret_cast<const char*>(bh.data()), static_cast<std::streamsize>(bh.size()));

        try
        {
            record_endpoint_meta_locked();
            writer_ = std::thread(&LoggerImpl::writer_loop, this);
        }
        catch (...)
        {
            running_ = false;
            queue_.clear();
            out_.close();
            return false;
        }
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();

        if (writer_.joinable()) writer_.join();

        // Final flush + finalize file
        std::lock_guard<std::mutex> lk(m_);
        if (out_.is_open())
        {
            try
            {
                flush_current_chunk_locked();
                write_index_and_chunk_info_locked();
                rewrite_bag_header_locked();
            }
            catch (...)
            {
                // A partial bag is preferable to terminating the application.
            }
            out_.close();
        }
    }

    bool is_running() const
    {
        // Use relaxed: only accessed under mutex in start/stop, and we want
        // a fast check from hooks.  The atomic guarantees atomic read.
        return running_.load(std::memory_order_relaxed);
    }

    void record(LogEvent event)
    {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (event.timestamp_ns == 0) event.timestamp_ns = NowNs();
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return;
        if (queue_.size() >= max_queue_) return;  // drop
        queue_.push_back(std::move(event));
        cv_.notify_one();
    }

   private:
    LoggerImpl() = default;
    ~LoggerImpl()
    {
        if (running_.load(std::memory_order_relaxed)) stop();
    }

    LoggerImpl(const LoggerImpl&) = delete;
    LoggerImpl& operator=(const LoggerImpl&) = delete;

    std::string make_timestamped_filename(const std::string& base_path) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;

        std::tm* tm_local = std::localtime(&now_time_t);

        std::ostringstream oss;
        oss << base_path<<"dzipc_log_"  // Base name
            << std::put_time(tm_local, "%Y%m%dT%H%M%S")   // 如 20260802T143052
            << "." << std::setw(3) << std::setfill('0') << now_ms.count()  // 毫秒
            << ".bag";
        return oss.str();
    }

    void writer_loop()
    {
        while (true)
        {
            LogEvent event;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) break;
                if (queue_.empty()) continue;
                event = std::move(queue_.front());
                queue_.pop_front();
            }
            try
            {
                process_event(std::move(event));
            }
            catch (...)
            {
                // Drop an event that cannot be serialized or buffered.
            }
        }
    }

    void process_event(LogEvent event)
    {
        if (event.payload && event.payload->size() > std::numeric_limits<uint32_t>::max()) return;
        // Serialize TransportPacket envelope
        auto pkt = serialize_transport_packet(event.timestamp_ns, event.topic, event.type_name, event.domain_id,
                                              event.msg_id, static_cast<uint8_t>(event.transport),
                                              static_cast<uint8_t>(event.role), static_cast<uint8_t>(event.event_kind),
                                              event.payload ? event.payload->data() : nullptr,
                                              event.payload ? static_cast<uint32_t>(event.payload->size()) : 0);

        // Build in-chunk message record for connection 0 (TransportPacket).
        auto msg_rec = build_msg_in_chunk(0, event.timestamp_ns, pkt);

        std::lock_guard<std::mutex> lk(m_);
        if (current_chunk_.msg_data.empty())
        {
            current_chunk_.start_ts = event.timestamp_ns;
        }
        current_chunk_.end_ts = event.timestamp_ns;
        current_chunk_.conn_counts[0]++;
        current_chunk_.index_entries.emplace_back(event.timestamp_ns,
                                                  static_cast<uint64_t>(current_chunk_.msg_data.size()));
        current_chunk_.msg_data.insert(current_chunk_.msg_data.end(), msg_rec.begin(), msg_rec.end());

        // Flush chunk if over budget
        if (current_chunk_.msg_data.size() >= max_memory_)
        {
            flush_current_chunk_locked();
        }
    }

    void flush_current_chunk_locked()
    {
        if (current_chunk_.msg_data.empty()) return;
        if (!out_.is_open()) return;

        uint64_t offset = static_cast<uint64_t>(out_.tellp());
        auto chunk = build_chunk(current_chunk_.msg_data);
        out_.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));

        // Each chunk is immediately followed by its connection index.  Entry
        // offsets are relative to the uncompressed chunk data.
        auto idx = build_index(0, current_chunk_.index_entries);
        out_.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size()));

        // Record chunk info
        ChunkInfo ci;
        ci.chunk_pos = offset;
        ci.start_ts = current_chunk_.start_ts;
        ci.end_ts = current_chunk_.end_ts;
        for (int i = 0; i < 4; ++i) ci.conn_counts[i] = current_chunk_.conn_counts[i];
        ci.conn_ids = {0};
        chunks_.push_back(ci);

        current_chunk_ = ChunkBuf{};
    }

    void write_index_and_chunk_info_locked()
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

    void rewrite_bag_header_locked()
    {
        // Rewrite the fixed-size placeholder at the beginning.  ROS tools use
        // this record to locate index data; appending a second header is not
        // valid and leaves index_pos pointing at the wrong location.
        uint32_t chunk_count = static_cast<uint32_t>(chunks_.size());
        auto bh = build_bag_header(index_pos_, 1, chunk_count);
        if (bh.size() != bag_header_size_) return;
        out_.flush();
        out_.seekp(bag_header_pos_);
        out_.write(reinterpret_cast<const char*>(bh.data()), static_cast<std::streamsize>(bh.size()));
        out_.flush();
        out_.seekp(0, std::ios::end);
    }

    void record_endpoint_meta_locked()
    {
        // Snapshot IpcInfoPool and record metadata events
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
            // Map EntryKind to RoleKind
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
    size_t max_memory_{256 * 1024 * 1024};
    size_t max_queue_{100000};

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
};

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool StartDzipcLog(const std::string& path="~/.dzipc/log", size_t max_memory_mb=512, size_t max_queue_size=100000)
{
    return LoggerImpl::instance().start(path, max_memory_mb, max_queue_size);
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
    // Called externally; delegates to instance which handles locking
    // We just push async so it goes through the writer loop
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
