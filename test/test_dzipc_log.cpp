/// Tests for dzipc_log, the ROS bag v2 recording module.
/// Verifies:
///   1. No recording when not started
///   2. Start/Stop idempotent
///   3. Publish produces non-empty bag
///   4. Bag header contains "#ROSBAG V2.0"
///   5. TransportPacket key fields present
///   6. Service request/response coverage
///   7. Stop after Stop is safe

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
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

    // First start
    EXPECT_TRUE(logger::StartDzipcLog(path, 1, 100));
    EXPECT_TRUE(logger::IsDzipcLogRunning());

    // Second start should fail (already running)
    EXPECT_FALSE(logger::StartDzipcLog(path, 1, 100));

    // First stop
    logger::StopDzipcLog();
    EXPECT_FALSE(logger::IsDzipcLogRunning());

    // Second stop should be safe
    logger::StopDzipcLog();
    EXPECT_FALSE(logger::IsDzipcLogRunning());

    // Cleanup
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 3: Publish generates non-empty bag
// ---------------------------------------------------------------------------
TEST(DzipcLog, PublishGeneratesNonEmptyBag)
{
    auto path = (fs::temp_directory_path() / "test_pub.bag").string();
    std::remove(path.c_str());

    // Start logger
    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));

    // Create a publisher via the public API factory
    auto td = TopicDataPtrMake<TestLogMsg>(42);
    auto pub = PublisherIPCPtrMake(td, "/test_log_topic", 0, IPC_SHM, false);
    pub->InitChannel();

    // Give a moment for control plane to come up, then publish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto msg = std::make_shared<TestLogMsg>();
    pub->publish(msg);

    // Small delay for logger to process, then stop
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    // Read file
    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u) << "Bag file should not be empty";

    // Verify header
    ASSERT_GE(data.size(), 13u);
    std::string hdr(reinterpret_cast<char*>(data.data()), 13);
    EXPECT_EQ(hdr, "#ROSBAG V2.0\n") << "Bag file should start with #ROSBAG V2.0";

    // Cleanup
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
    // Stop immediately — should still produce a valid bag with header + metadata
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

    auto msg = std::make_shared<TestLogMsg>();
    pub->publish(msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    ASSERT_GT(data.size(), 13u);

    // We have at least: header(13) + bag_header_record + conn0 + conn1 + chunk
    // Inside chunk: msg sub-records. Search for the topic string in the binary.
    std::string topic_str = "/test_fields_topic";
    // The topic name should appear somewhere in the serialized payload
    auto found = std::search(data.begin(), data.end(), topic_str.begin(), topic_str.end());
    EXPECT_NE(found, data.end()) << "Topic name not found in bag file";

    // Also check for the TransportPacket type marker
    std::string tp_type = "dzipc_log/TransportPacket";
    found = std::search(data.begin(), data.end(), tp_type.begin(), tp_type.end());
    EXPECT_NE(found, data.end()) << "TransportPacket type not found in bag file";

    // Verify the top-level ROS bag record structure and index pointer.
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

    // Create server + client for a unique service topic so stale SHM objects
    // from an interrupted test cannot make the control-plane open fail.
    const std::string service_topic = "log_srv_" + std::to_string(logger::NowNs() % 1000000ULL);
    std::atomic<bool> req_received{false};
    auto srv_data = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);

    ServerCallBackFun cb = [&req_received](ServerDataPtr& sd)
    {
        req_received.store(true);
        // Set response data (swap out the response msg)
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

    // Wait for handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto req = std::make_shared<TestLogMsg>();
    auto srv_req = ServerDataPtrMake<TestLogMsg, TestLogMsg>(100);
    srv_req->request() = req;
    client->send_request(srv_req, 500);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger::StopDzipcLog();

    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u) << "Bag file after service request should not be empty";

    // Verify the service topic appears in the bag
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
    // Should not crash or throw
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

    // Register an entry in IpcInfoPool before starting logger
    const auto slot = info_pool::IpcInfoPool::instance().register_entry(
        {info_pool::EntryKind::ShmPub, "/test_meta_topic", "TestType", "", 0, ""});

    ASSERT_TRUE(logger::StartDzipcLog(path, 1, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    logger::StopDzipcLog();

    auto data = read_file(path);
    EXPECT_GT(data.size(), 0u);

    // The endpoint metadata topic should appear
    std::string meta_topic = "/test_meta_topic";
    auto found = std::search(data.begin(), data.end(), meta_topic.begin(), meta_topic.end());
    EXPECT_NE(found, data.end()) << "Metadata topic not found in bag";

    info_pool::IpcInfoPool::instance().unregister_entry(slot);

    std::remove(path.c_str());
}

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

}  // namespace
