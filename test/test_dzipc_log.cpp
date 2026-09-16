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

    /* 目标是把队列压过 max_queue(10) 从而 arm 住轮转。
     *
     * 原来的写法用 64 KB 负载 + 每轮 sleep(50ms), 结果是掷硬币(实测 12 次跑失败 6 次):
     *   - RecordPublish 会把负载 memcpy 进事件, 所以 64 KB 负载让**生产者**每次 push 的
     *     成本与写线程每次 pop+落盘的成本相当, 谁快谁慢全看调度;
     *   - 每轮之间的 sleep 又给了写线程把队列排空的机会, 下一轮从 0 开始重新爬。
     *
     * 改成小负载 + 不间断紧循环: 写线程每个事件有固定开销(序列化、索引项、ofstream 调用),
     * 而生产者只是一次小 memcpy + 入队, 于是生产者稳定跑赢, 队列必然爬过 10。
     * 超过 eff_max 的推送会被丢弃(这正常), arm 只需要队列**触到** 10 一次。 */
    std::vector<uint8_t> small(64, 0xEF);

    for (int i = 0; i < 400; ++i)
        logger::RecordPublish("/t4", "T4", 0, i, logger::TransportKind::kShm, small.data(), small.size());
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

// ---------------------------------------------------------------------------
// Test 11: ser-cli 的传输记录必须反映**实际**承载。
//
// 【契约变更】本用例原判据是"IPC_SOCKET 必须记 kSocket(1)"。该判据依赖一个已被
// **有意去掉**的性质: ser-cli 的 IPC_SOCKET 曾经等于"强制纯 socket"。现在工厂把
// IPC_SOCKET 归一化成自动选路(server_ipc.cc 的 normalize_sercli_type), 同机必然切
// SHM —— 于是那条断言测的是一个不再存在的契约, 它变红是**正确**的, 不是回归。
//
// 判据随之改为守**新**契约, 三个 case 各守一头:
//   ① IPC_SHM          —— 强制共享内存, 构造期即 Shm, 记 kShm;
//   ② IPC_SOCKET       —— 必须归一到自动选路, 同机切 SHM 后记 kShm。
//      ② 同时是"归一化没被删掉"的回归门: 谁去掉 normalize_sercli_type, 它就会退回
//      kSocket 而失败。
//   ③ IPC_SOCKET_ONLY  —— 强制纯 socket, 不切, 记 kSocket。
//      ③ 是"纯 socket 的日志取值"的见证 —— 契约变更一度让它失去覆盖, IPC_SOCKET_ONLY
//      补回入口之后它重新可测; 谁把 SocketOnly 也接进自动选路, 它就会失败。
// ---------------------------------------------------------------------------
TEST(DzipcLog, SerCliTransportIsRecordedAsActuallyUsed)
{
    struct Case
    {
        const char* tag;
        IPCType type;
        uint8_t expected;
        bool wait_for_shm;   // IPC_SOCKET 走自动选路: 须等两侧都切到 SHM 再发 RPC
    };
    const Case cases[] = {
        {"shm", IPC_SHM, static_cast<uint8_t>(logger::TransportKind::kShm), false},
        {"sock", IPC_SOCKET, static_cast<uint8_t>(logger::TransportKind::kShm), true},
        {"sock_only", IPC_SOCKET_ONLY, static_cast<uint8_t>(logger::TransportKind::kSocket), false}};

    for (const Case& c : cases)
    {
        auto path = (fs::temp_directory_path() / (std::string("test_na_") + c.tag + ".bag")).string();
        std::remove(path.c_str());
        const std::string service_topic = std::string("log_na_") + c.tag + "_"
                                          + std::to_string(logger::NowNs() % 1000000ULL);
        auto srv_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
        ServerCallBackFun cb = [](ServerDataPtr& sd) { sd->response() = std::make_shared<TestLogMsg>(); };

        ASSERT_TRUE(logger::StartDzipcLog(path, 1, 100));
        {
            auto server = ServerIPCPtrMake(service_topic, srv_data, cb, 0, c.type, false);
            server->InitChannel();
            auto cli_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
            auto client = ClientIPCPtrMake(service_topic, cli_data, 0, c.type, false);
            client->InitChannel("");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            if (c.wait_for_shm)
            {
                /* 等**两侧都**到 SHM。只等一侧会让 RPC 走在另一侧的 socket 上, 那时
                 * 记成 socket 是正确行为, 判据却会把它当失败 —— 判据必须钉在"切换完成
                 * 之后"这个状态上(与 Test 10 AutoPathLogsTheLiveTransport 同法)。 */
                const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (std::chrono::steady_clock::now() < dl
                       && !(client->transport_current() == path::Kind::Shm
                            && server->transport_current() == path::Kind::Shm))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                ASSERT_EQ(client->transport_current(), path::Kind::Shm)
                    << c.tag << ": IPC_SOCKET 的 ser-cli 未归一到自动选路(同机应切 SHM)";
                /* 两腿都报 Shm 与"SHM 数据面已能收发"不是同一时刻。 */
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            auto req = std::make_shared<TestLogMsg>();
            auto srv_req = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
            srv_req->request() = req;
            client->send_request(srv_req, 800);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        logger::StopDzipcLog();

        auto data = read_file(path);
        ASSERT_GT(data.size(), 0u) << c.tag << ": bag 为空";
        ASSERT_EQ(data[0], '#') << c.tag << ": bag 头不对";

        /* transport 字节: 与 Auto 用例同一套定位(长度前缀 + 含 TestLogMsg + role/kind 合法)。 */
        auto u32le = [&data](size_t at) {
            return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8)
                   | (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
        };
        int seen = 0;
        for (size_t i = 0; i + 4 + service_topic.size() < data.size(); ++i)
        {
            if (u32le(i) != service_topic.size()) continue;
            if (std::memcmp(&data[i + 4], service_topic.data(), service_topic.size()) != 0) continue;
            const size_t tp = i + 4 + service_topic.size();
            if (tp + 4 > data.size()) continue;
            const size_t tlen = u32le(tp);
            if (tlen == 0 || tlen > 256 || tp + 4 + tlen + 11 > data.size()) continue;
            const std::string tname(reinterpret_cast<const char*>(&data[tp + 4]), tlen);
            if (tname.find("TestLogMsg") == std::string::npos) continue;
            if (data[tp + 4 + tlen + 9] > 2 || data[tp + 4 + tlen + 10] > 2) continue;
            EXPECT_EQ(data[tp + 4 + tlen + 8], c.expected)
                << c.tag << ": 非 Auto 路径的 transport 记录被动过";
            ++seen;
        }
        EXPECT_GT(seen, 0) << c.tag << ": 一条本用例的记录都没解析出来";
        std::remove(path.c_str());
    }
}

// ---------------------------------------------------------------------------
// Test 12: topic_ipc(pub/sub) 的日志路径**见不到 Auto** —— 断言这条前提。
//
// pub/sub 没有自动选路(只有 shm/socket 两个分支), 构造期类型恒等于实际传输, 所以
// 它的日志不需要(也不应该)改成读活值。这条断言把"前提"钉住: 一旦将来给 pub/sub
// 加 Auto, 这里会先红, 而不是让日志静默记错。
// ---------------------------------------------------------------------------
TEST(DzipcLog, PubSubRejectsAutoInsteadOfMislabeling)
{
    auto td = TopicDataPtrMake<TestLogMsg>(42);
    EXPECT_THROW(PublisherIPCPtrMake(td, "/t5_pubsub_auto", 0, IPCType::Auto, false), std::invalid_argument)
        << "pub/sub 接受了 IPCType::Auto: 它会按 shm/socket 分支之外的路径走, 日志也无活值可读";
    EXPECT_THROW(SubscriberIPCPtrMake(td, "/t5_pubsub_auto", 0, 8, IPCType::Auto, false), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Test 10: 日志的 TransportKind 必须读**当下**的传输 (T2 §7 R5)。
//
// IPCType::Auto 下构造期类型与实际承载不再相等; 若日志仍读构造期值, 切到 SHM
// 之后的所有记录都会被记成 socket, 排查时把人引到错方向。
//
// 判据取 bag 里 TransportPacket 的 transport 字节(0=kShm/1=kSocket)本身, 不看
// 日志文本 —— 这是任务列表"不以日志看起来成功为唯一证据"的同一条。
// ---------------------------------------------------------------------------
TEST(DzipcLog, AutoPathLogsTheLiveTransport)
{
    auto path = (fs::temp_directory_path() / "test_autopath_transport.bag").string();
    std::remove(path.c_str());

    const std::string service_topic = "log_auto_" + std::to_string(logger::NowNs() % 1000000ULL);
    std::atomic<bool> req_received{false};
    auto srv_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
    ServerCallBackFun cb = [&req_received](ServerDataPtr& sd)
    {
        req_received.store(true);
        sd->response() = std::make_shared<TestLogMsg>();
    };

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 100));
    /* ⛔ 日志是**单例**: 本用例里任何 ASSERT_* 提前返回都会跳过 StopDzipcLog,
     * 于是后续用例的 StartDzipcLog 直接失败(表现为"另一个用例坏了")。用一个
     * RAII 收尾, 让所有提前返回都安全。 */
    struct LogStop
    {
        ~LogStop() { logger::StopDzipcLog(); }
    } log_stop;

    std::shared_ptr<pimpl::server_ipc_impl> server;
    std::shared_ptr<pimpl::client_ipc_impl> client;
    server = ServerIPCPtrMake(service_topic, srv_data, cb, 0, IPC_SOCKET, false);
    server->InitChannel();
    auto cli_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
    client = ClientIPCPtrMake(service_topic, cli_data, 0, IPC_SOCKET, false);
    client->InitChannel("");

    /* 等**两侧都**切到 SHM —— 服务端先建、客户端后接, 只等一侧会让 RPC 走在
     * 另一侧的 socket 上, 于是记成 socket 是**正确**行为, 判据却把它当失败。
     * 这条不是"等得久一点", 是判据必须钉在"切换完成之后"这个状态上。 */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline
           && !(client->transport_current() == path::Kind::Shm && server->transport_current() == path::Kind::Shm))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool switched = client->transport_current() == path::Kind::Shm
                          && server->transport_current() == path::Kind::Shm;

    /* 切换落地后先让数据面稳定一下再发: 两腿都报 Shm 与"SHM 数据面已能收发"不是
     * 同一时刻(客户端置 use_shm_ 与 shm 腿真正可服务之间有一个短窗口)。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto srv_req = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
    srv_req->request() = std::make_shared<TestLogMsg>();
    const bool rpc_ok = client->send_request(srv_req, 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    logger::StopDzipcLog();

    auto data = read_file(path);
    ASSERT_GT(data.size(), 0u);

    /* 定位 TransportPacket 的 transport 字节。
     * 布局(serialize_transport_packet): ts64 · [u32+bytes]topic · [u32+bytes]type ·
     * u32 domain · u32 msg_id · u8 transport · u8 role · u8 event_kind · payload。
     * type 的长度**不写死**: 记录里存的是 demangle 后的名字(匿名命名空间下带前缀),
     * 所以先读长度再跳过, 只用"含 TestLogMsg"做身份校验。 */
    const std::string type_mark = "TestLogMsg";
    auto u32le = [&data](size_t at) {
        return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8)
               | (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
    };
    std::vector<uint8_t> transports;
    for (size_t i = 0; i + 4 + service_topic.size() < data.size(); ++i)
    {
        if (u32le(i) != service_topic.size()) continue;
        if (std::memcmp(&data[i + 4], service_topic.data(), service_topic.size()) != 0) continue;
        const size_t tp = i + 4 + service_topic.size();
        if (tp + 4 > data.size()) continue;
        const size_t tlen = u32le(tp);
        if (tlen == 0 || tlen > 256 || tp + 4 + tlen + 9 > data.size()) continue;
        const std::string tname(reinterpret_cast<const char*>(&data[tp + 4]), tlen);
        if (tname.find(type_mark) == std::string::npos) continue;
        /* role/kind 必须在枚举范围内 —— 这一条同时把"命中的是转写记录/连接记录"
         * 的假匹配挡掉: 那些记录里也含同样的 topic/type 字面量, 但后面的字节不是
         * 合法的 role/kind。不做这一步会把非 TransportPacket 的记录也当证据。 */
        if (data[tp + 4 + tlen + 9] > 2 || data[tp + 4 + tlen + 10] > 2) continue;
        transports.push_back(data[tp + 4 + tlen + 8]);
    }

    /* ⛔ 判据只钉在"有记录时记录对不对": transports 为空 = 本会话没产生可判定的
     * TransportPacket, 无从检验, 如实 skip 并说明。
     * rpc_ok **只作诊断事实记录**(随 skip 消息与断言失败消息一起输出), 不作门槛
     * —— 一次 RPC 的成败受对端时序影响, 拿它当门会把"日志记错了"与"这次往返没
     * 成功"混成同一个 skip, 判据就失去了分辨力; 而 transport 字节本身与 RPC 返回
     * 值无关, 记录在就说明日志取到了当下传输。 */
    if (transports.empty())
    {
        std::remove(path.c_str());
        GTEST_SKIP() << "Auto 会话未产生可判定的记录 (rpc_ok=" << rpc_ok << ", cli="
                     << dzIPC::path::to_string(client->transport_current()) << ", srv="
                     << dzIPC::path::to_string(server->transport_current()) << ")";
    }

    if (!switched)
    {
        /* 本环境没切到 SHM(例如池证据不足) ⇒ 该用例的判据无从检验, 如实跳过
         * 而不是假装通过。 */
        std::remove(path.c_str());
        GTEST_SKIP() << "IPC_SOCKET(自动选路) 未在本环境切到 SHM, 无法判定 transport 记录正确性";
    }

    /* 切换完成后本用例只发了一次 RPC, 它产生的每条记录(客户端请求/响应、服务端
     * 请求/响应)都应当记在 SHM 上。任何一条记成 socket 都说明 transport 取的是
     * 构造期值。rpc_ok 只作诊断事实随消息输出。 */
    for (uint8_t t : transports)
    {
        EXPECT_EQ(t, static_cast<uint8_t>(logger::TransportKind::kShm))
            << "切到 SHM 之后日志仍记成 socket —— transport 读的是构造期 IPCType"
            << " (记录数=" << transports.size() << ", rpc_ok=" << rpc_ok << ")";
    }

    std::remove(path.c_str());
}
