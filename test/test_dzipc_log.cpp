/// Tests for dzipc_log, the ROS bag v2 recording module.
/// Verifies:
///   1. No recording when not started
///   2. Start/Stop idempotent
///   3. Publish produces non-empty bag
///   4. Bag header contains "#ROSBAG V2.0"
///   5. TransportPacket key fields present
///   6. Service request/response coverage
///   7. Stop after Stop is safe
///   8. IpcInfoPool endpoint metadata
///   9. Nodelet publish recording
///   T0  Legacy 3-param API
///   T1  max_memory_mb rotation
///   T2  max_file_size_mb rotation
///   T3  max_duration_sec rotation (idle timeout)
///   T4  max_queue_size rotation + augmented capacity
///   T5  Hysteresis prevents continuous re-rotation
///   T6  max_queue=1 edge case
///   T7  Cross-bag event uniqueness (no loss/dup)

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/dzipc.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/server_ipc.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

namespace fs = std::filesystem;
using namespace dzIPC;

namespace
{

// ---------------------------------------------------------------------------
// A minimal test message for publish/subscribe
// ---------------------------------------------------------------------------
class TestLogMsg : public IpcMsgBase
{
   public:
    TestLogMsg() { set_msg_id(42); }
    ipc::buffer serialize() override
    {
        uint8_t* p = new uint8_t[8];
        std::memcpy(p, "HELLO42", 8);
        return ipc::buffer(p, 8, [](void* q, std::size_t) { delete[] static_cast<uint8_t*>(q); });
    }
    void deserialize(const ipc::buffer&) override {}
    TestLogMsg* clone() const override { return new TestLogMsg(*this); }
};

struct NodeletReset
{
    ~NodeletReset() { dzIPC::EnableNodelet(false); }
};

// ---------------------------------------------------------------------------
// Helper: read entire file into a vector
// ---------------------------------------------------------------------------
std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    return data;
}

// ---------------------------------------------------------------------------
// Helper: LE read helpers for bag file verification
// ---------------------------------------------------------------------------
uint32_t r32le(const uint8_t* b, size_t off)
{
    return b[off] | (static_cast<uint32_t>(b[off + 1]) << 8) | (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint64_t r64le(const uint8_t* b, size_t off)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return value;
}

std::map<std::string, std::vector<uint8_t>> record_fields(const std::vector<uint8_t>& data, size_t record_offset)
{
    std::map<std::string, std::vector<uint8_t>> fields;
    if (record_offset + 4 > data.size()) return fields;
    const uint32_t header_len = r32le(data.data(), record_offset);
    size_t pos = record_offset + 4;
    const size_t end = pos + header_len;
    while (pos + 4 <= end && end <= data.size())
    {
        const uint32_t len = r32le(data.data(), pos);
        pos += 4;
        if (pos + len > end) break;
        auto eq = std::find(data.begin() + static_cast<std::ptrdiff_t>(pos),
                            data.begin() + static_cast<std::ptrdiff_t>(pos + len), static_cast<uint8_t>('='));
        if (eq == data.begin() + static_cast<std::ptrdiff_t>(pos + len)) break;
        const size_t eq_pos = static_cast<size_t>(eq - data.begin());
        std::string key(reinterpret_cast<const char*>(data.data() + pos), eq_pos - pos);
        fields[key] = std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(eq_pos + 1),
                                           data.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    }
    return fields;
}

// ---------------------------------------------------------------------------
// Helper: list bag files matching a prefix and optional polling
// ---------------------------------------------------------------------------
std::vector<std::string> list_bags(const std::string& marker, int min_count = 0, int poll_ms = 0, int max_polls = 20)
{
    for (int attempt = 0; attempt <= max_polls; ++attempt)
    {
        std::vector<std::string> result;
        for (auto& de : fs::directory_iterator(fs::temp_directory_path()))
        {
            auto name = de.path().filename().string();
            if (name.find(marker) != std::string::npos && name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".bag") == 0)
                result.push_back(de.path().string());
        }
        std::sort(result.begin(), result.end());
        if (result.size() >= static_cast<size_t>(min_count)) return result;
        if (poll_ms > 0 && attempt < max_polls)
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    // Final attempt without min_count
    std::vector<std::string> result;
    for (auto& de : fs::directory_iterator(fs::temp_directory_path()))
    {
        auto name = de.path().filename().string();
        if (name.find(marker) != std::string::npos && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".bag") == 0)
            result.push_back(de.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

/// RAII guard: stops logger and removes only bags matching a unique marker string
struct LogCleanup
{
    std::string marker_;
    LogCleanup(std::string m) : marker_(std::move(m))
    {
        // Clean up any leftover from previous interrupted run
        auto bags = list_bags(marker_);
        for (auto& b : bags) { std::error_code ec; fs::remove(b, ec); }
    }
    ~LogCleanup()
    {
        logger::StopDzipcLog();
        auto bags = list_bags(marker_);
        for (auto& b : bags) { std::error_code ec; fs::remove(b, ec); }
    }
};

/// Verify a single bag file has valid structure
void verify_bag(const std::string& path)
{
    auto data = read_file(path);
    ASSERT_GE(data.size(), 13u) << "Bag " << path << " too small";
    ASSERT_EQ(std::string(reinterpret_cast<char*>(data.data()), 13), "#ROSBAG V2.0\n")
        << "Bag " << path << " missing magic";

    // File header record (at offset 13)
    auto fh = record_fields(data, 13);
    ASSERT_EQ(fh.at("op").size(), 1u) << "Bag " << path << " missing op field";
    EXPECT_EQ(fh.at("op")[0], 0x03) << "Bag " << path << " FileHeader op != 0x03";
    EXPECT_EQ(fh.at("conn_count").size(), 4u);
    EXPECT_EQ(fh.at("chunk_count").size(), 4u);

    // index_pos must be within file
    ASSERT_EQ(fh.at("index_pos").size(), 8u);
    uint64_t idx_pos = r64le(fh.at("index_pos").data(), 0);
    EXPECT_GT(idx_pos, 0u) << "Bag " << path << " index_pos is 0 (not finalized)";
    EXPECT_LT(idx_pos, data.size()) << "Bag " << path << " index_pos out of range";

    // Connection record at index_pos
    auto conn = record_fields(data, static_cast<size_t>(idx_pos));
    EXPECT_EQ(conn.at("op").size(), 1u);
    EXPECT_EQ(conn.at("op")[0], 0x07) << "Bag " << path << " Connection op != 0x07";
}

// ---------------------------------------------------------------------------
// Test 1: Default — not recording
// ---------------------------------------------------------------------------
TEST(DzipcLog, NotRecordingByDefault) { EXPECT_FALSE(logger::IsDzipcLogRunning()); }

// ---------------------------------------------------------------------------
// Test 2: Start/Stop idempotent
// ---------------------------------------------------------------------------
TEST(DzipcLog, StartStopIdempotent)
{
    auto path = (fs::temp_directory_path() / "test_idem.bag").string();
    std::remove(path.c_str());

    EXPECT_TRUE(logger::StartDzipcLog(path, 1, 100));
    EXPECT_TRUE(logger::IsDzipcLogRunning());
    EXPECT_FALSE(logger::StartDzipcLog(path, 1, 100));

    logger::StopDzipcLog();
    EXPECT_FALSE(logger::IsDzipcLogRunning());
    logger::StopDzipcLog();
    EXPECT_FALSE(logger::IsDzipcLogRunning());

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 3: Publish generates non-empty bag
// ---------------------------------------------------------------------------
TEST(DzipcLog, PublishGeneratesNonEmptyBag)
{
    auto path = (fs::temp_directory_path() / "test_pub.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));

    auto td = TopicDataPtrMake<TestLogMsg>(42);
    auto pub = PublisherIPCPtrMake(td, "/test_log_topic", 0, IPC_SHM, false);
    pub->InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pub->publish(std::make_shared<TestLogMsg>());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u);
    ASSERT_GE(data.size(), 13u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(data.data()), 13), "#ROSBAG V2.0\n");
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 4: Header contains #ROSBAG V2.0
// ---------------------------------------------------------------------------
TEST(DzipcLog, HeaderContainsRosbagV2)
{
    auto path = (fs::temp_directory_path() / "test_hdr.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    ASSERT_GE(data.size(), 13u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(data.data()), 13), "#ROSBAG V2.0\n");
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 5: TransportPacket fields in bag chunk data
// ---------------------------------------------------------------------------
TEST(DzipcLog, TransportPacketKeyFields)
{
    auto path = (fs::temp_directory_path() / "test_fields.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));

    auto td = TopicDataPtrMake<TestLogMsg>(42);
    auto pub = PublisherIPCPtrMake(td, "/test_fields_topic", 7, IPC_SHM, false);
    pub->InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pub->publish(std::make_shared<TestLogMsg>());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    ASSERT_GT(data.size(), 13u);

    std::string topic_str = "/test_fields_topic";
    auto found = std::search(data.begin(), data.end(), topic_str.begin(), topic_str.end());
    EXPECT_NE(found, data.end()) << "Topic name not found in bag file";

    std::string tp_type = "dzipc_log/TransportPacket";
    found = std::search(data.begin(), data.end(), tp_type.begin(), tp_type.end());
    EXPECT_NE(found, data.end()) << "TransportPacket type not found in bag file";

    const auto file_header = record_fields(data, 13);
    ASSERT_EQ(file_header.at("op").size(), 1u);
    EXPECT_EQ(file_header.at("op")[0], 0x03);
    ASSERT_EQ(file_header.at("index_pos").size(), 8u);
    const uint64_t index_pos = r64le(file_header.at("index_pos").data(), 0);
    ASSERT_LT(index_pos, data.size());
    const auto chunk_header = record_fields(data, 13 + 4096);
    ASSERT_EQ(chunk_header.at("op").size(), 1u);
    EXPECT_EQ(chunk_header.at("op")[0], 0x05);
    const auto connection_header = record_fields(data, static_cast<size_t>(index_pos));
    ASSERT_EQ(connection_header.at("op").size(), 1u);
    EXPECT_EQ(connection_header.at("op")[0], 0x07);

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 6: Service request/response coverage
// ---------------------------------------------------------------------------
TEST(DzipcLog, ServiceRequestResponseRecords)
{
    auto path = (fs::temp_directory_path() / "test_srv.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 100));

    const std::string service_topic = "log_srv_" + std::to_string(logger::NowNs() % 1000000ULL);
    std::atomic<bool> req_received{false};
    auto srv_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);

    ServerCallBackFun cb = [&req_received](ServerDataPtr& sd)
    {
        req_received.store(true);
        auto rsp = std::make_shared<TestLogMsg>();
        sd->response() = rsp;
    };

    std::shared_ptr<pimpl::server_ipc_impl> server;
    std::shared_ptr<pimpl::client_ipc_impl> client;
    try
    {
        server = ServerIPCPtrMake(service_topic, srv_data, cb, 0, IPC_SHM, false);
        server->InitChannel();
        auto cli_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
        client = ClientIPCPtrMake(service_topic, cli_data, 0, IPC_SHM, false);
        client->InitChannel("");
    }
    catch (...)
    {
        logger::StopDzipcLog();
        throw;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto req = std::make_shared<TestLogMsg>();
    auto srv_req = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
    srv_req->request() = req;
    client->send_request(srv_req, 500);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger::StopDzipcLog();

    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u);
    std::string srv_topic = service_topic;
    auto found = std::search(data.begin(), data.end(), srv_topic.begin(), srv_topic.end());
    EXPECT_NE(found, data.end()) << "Service topic not found in bag";
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 7: Stop after Stop is safe (idempotent)
// ---------------------------------------------------------------------------
TEST(DzipcLog, StopIdempotentAfterStop)
{
    auto path = (fs::temp_directory_path() / "test_stop2.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path));
    logger::StopDzipcLog();
    EXPECT_NO_THROW(logger::StopDzipcLog());
    EXPECT_NO_THROW(logger::StopDzipcLog());
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 8: IpcInfoPool endpoint metadata recorded
// ---------------------------------------------------------------------------
TEST(DzipcLog, EndpointMetaWrittenOnStart)
{
    auto path = (fs::temp_directory_path() / "test_meta.bag").string();
    std::remove(path.c_str());

    const auto slot = info_pool::IpcInfoPool::instance().register_entry(
        {info_pool::EntryKind::ShmPub, "/test_meta_topic", "TestType", "", 0, ""});

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u);

    std::string meta_topic = "/test_meta_topic";
    auto found = std::search(data.begin(), data.end(), meta_topic.begin(), meta_topic.end());
    EXPECT_NE(found, data.end()) << "Metadata topic not found in bag";

    info_pool::IpcInfoPool::instance().unregister_entry(slot);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 9: Nodelet publish is recorded
// ---------------------------------------------------------------------------
TEST(DzipcLog, NodeletEnabledPublishIsRecorded)
{
    NodeletReset reset;
    dzIPC::EnableNodelet(true);
    const auto path = (fs::temp_directory_path() / "test_nodelet_log.bag").string();
    std::remove(path.c_str());

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 100));
    const std::string topic = "log_nodelet_" + std::to_string(logger::NowNs() % 1000000ULL);
    auto sub_data = TopicDataPtrMake<TestLogMsg>(42);
    auto pub_data = TopicDataPtrMake<TestLogMsg>(42);
    auto sub = SubscriberIPCPtrMake(sub_data, topic, 0, 8, IPC_SHM, false);
    auto pub = PublisherIPCPtrMake(pub_data, topic, 0, IPC_SHM, false);
    sub->InitChannel();
    pub->InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    for (int i = 0; i < 3; ++i) EXPECT_TRUE(pub->publish(std::make_shared<TestLogMsg>()));
    logger::StopDzipcLog();

    const auto data = read_file(path);
    const auto found = std::search(data.begin(), data.end(), topic.begin(), topic.end());
    EXPECT_NE(found, data.end());
    std::remove(path.c_str());
}

// =========================================================================
// Rotation tests
// =========================================================================

// T0: Old 3-param API works, uses explicit .bag path
TEST(DzipcLogRotation, OldApiSingleBag)
{
    const std::string marker = "T0oldapi";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));
    for (int i = 0; i < 5; ++i)
        logger::RecordPublish("/t0", "T0", 0, i, logger::TransportKind::kShm, nullptr, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger::StopDzipcLog();

    verify_bag(path);
}

// T1: max_memory_mb triggers rotation — assert ≥ 2 bags
TEST(DzipcLogRotation, MemoryBudgetRotates)
{
    const std::string marker = "T1mem";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    // 1 MB memory budget
    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 100000));

    // 3 × 600 KB > 1 MiB — forces rotation
    std::vector<uint8_t> big(600 * 1024, 0xAB);
    for (int i = 0; i < 3; ++i)
        logger::RecordPublish("/t1", "T1", 0, i, logger::TransportKind::kShm, big.data(), big.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 2u) << "Should rotate to at least 2 bags, got " << bags.size();
    for (auto& b : bags) verify_bag(b);
}

// T2: max_file_size_mb triggers rotation — assert ≥ 2 bags
TEST(DzipcLogRotation, FileSizeTriggersRotation)
{
    const std::string marker = "T2fs";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    logger::RotationOptions opts;
    opts.max_file_size_mb = 1;  // rotate at 1 MiB
    ASSERT_TRUE(logger::StartDzipcLog(path, 512, 100000, opts));

    std::vector<uint8_t> big(60000, 0xCD);  // ~60 KB per event
    for (int i = 0; i < 40; ++i)
        logger::RecordPublish("/t2", "T2", 0, i, logger::TransportKind::kShm, big.data(), big.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 2u) << "Should rotate to at least 2 bags, got " << bags.size();
    for (auto& b : bags) verify_bag(b);
}

// T3: max_duration_sec triggers rotation via idle timeout — assert ≥ 2 bags
TEST(DzipcLogRotation, DurationTriggersRotation)
{
    const std::string marker = "T3dur";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    logger::RotationOptions opts;
    opts.max_duration_sec = 1;  // 1 second
    ASSERT_TRUE(logger::StartDzipcLog(path, 256, 100000, opts));

    // Publish then sleep past deadline
    logger::RecordPublish("/t3", "T3", 0, 0, logger::TransportKind::kShm, nullptr, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Publish again — should trigger duration-based rotation
    logger::RecordPublish("/t3", "T3", 0, 1, logger::TransportKind::kShm, nullptr, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 2u) << "Duration should trigger rotation for >=2 bags, got " << bags.size();
    for (auto& b : bags) verify_bag(b);
}

// T4: max_queue_size triggers rotation with augmented capacity — assert ≥ 2 bags
TEST(DzipcLogRotation, QueueDepthTriggersRotation)
{
    const std::string marker = "T4q";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    ASSERT_TRUE(logger::StartDzipcLog(path, 256, 10));

    // 64 KB payload so writer can't instantly drain — queue arms
    std::vector<uint8_t> big(64 * 1024, 0xEF);

    // Flood faster than writer can drain (12 > 10 base, < 15 augmented)
    for (int round = 0; round < 4; ++round)
    {
        for (int i = 0; i < 12; ++i)
            logger::RecordPublish("/t4", "T4", 0, i, logger::TransportKind::kShm, big.data(), big.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 2u) << "Queue depth should trigger rotation for >=2 bags, got " << bags.size();
    for (auto& b : bags) verify_bag(b);
}

// T5: Hysteresis prevents rapid-fire re-rotation
TEST(DzipcLogRotation, HysteresisPreventsChurn)
{
    const std::string marker = "T5hyst";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    ASSERT_TRUE(logger::StartDzipcLog(path, 256, 5));

    // 64 KB payload so writer can't instantly drain
    std::vector<uint8_t> big(64 * 1024, 0xCD);

    for (int round = 0; round < 3; ++round)
    {
        for (int i = 0; i < 8; ++i)
            logger::RecordPublish("/t5", "T5", 0, i, logger::TransportKind::kShm, big.data(), big.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 2u) << "Should produce at least 2 bags, got " << bags.size();
    EXPECT_LE(bags.size(), 5u) << "Hysteresis should prevent excessive bag churn, got " << bags.size();
    for (auto& b : bags) verify_bag(b);
}

// T6: max_queue=1 edge case — must not hang or stuck-at-zero watermark
TEST(DzipcLogRotation, MaxQueueOneEdgeCase)
{
    const std::string marker = "T6q1";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    ASSERT_TRUE(logger::StartDzipcLog(path, 256, 1));

    for (int i = 0; i < 5; ++i)
    {
        logger::RecordPublish("/t6", "T6", 0, i, logger::TransportKind::kShm, nullptr, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    EXPECT_GE(bags.size(), 1u);
    for (auto& b : bags) verify_bag(b);
}

// T7: Cross-bag event uniqueness — verify no events lost or duplicated
TEST(DzipcLogRotation, CrossBagNoLossNoDup)
{
    const std::string marker = "T7xbag";
    auto path = (fs::temp_directory_path() / (marker + ".bag")).string();
    LogCleanup cleanup(marker);

    logger::RotationOptions opts;
    opts.max_duration_sec = 1;
    ASSERT_TRUE(logger::StartDzipcLog(path, 256, 100000, opts));

    const std::string tag = "T7_UNIQ_" + std::to_string(logger::NowNs());
    const int total_events = 20;
    for (int i = 0; i < total_events; ++i)
    {
        // 用定宽零填充编号避免子串重叠（如 _1 误匹配 _10.._19）
        char topic_buf[256];
        std::snprintf(topic_buf, sizeof(topic_buf), "/t7_%s_%04d", tag.c_str(), i);
        logger::RecordPublish(topic_buf, "T7", 0, i, logger::TransportKind::kShm, nullptr, 0);
        if (i == 10) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    logger::StopDzipcLog();

    auto bags = list_bags(marker);
    ASSERT_GE(bags.size(), 2u) << "Should have cross-bag rotation, got " << bags.size() << " bag(s)";

    // Count occurrences of each unique needle across all bags
    std::map<std::string, int> needle_counts;
    for (int i = 0; i < total_events; ++i)
    {
        char needle_buf[256];
        std::snprintf(needle_buf, sizeof(needle_buf), "/t7_%s_%04d", tag.c_str(), i);
        needle_counts[needle_buf] = 0;
    }

    for (auto& b : bags)
    {
        verify_bag(b);
        auto data = read_file(b);
        for (auto& kv : needle_counts)
        {
            // Count all occurrences of needle in this bag's binary data
            int count = 0;
            auto it = data.begin();
            while (true)
            {
                it = std::search(it, data.end(), kv.first.begin(), kv.first.end());
                if (it == data.end()) break;
                count++;
                ++it;
            }
            kv.second += count;
        }
    }

    for (auto& kv : needle_counts)
    {
        EXPECT_EQ(kv.second, 1) << "Needle " << kv.first << " found " << kv.second << " times (expected 1)";
    }
}

}  // namespace
