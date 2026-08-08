#include "dzIPC/dzipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/srv_data.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// For stderr capture in SC-03 and SC-05.
#include <sstream>

#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/local_pub_sub_registry.h"

namespace {

/* ============================================================================
 * Counting messages: track serialize/deserialize/clone call counts
 * to verify fast-path behavior.
 * ============================================================================ */

struct SerCliCountStats {
    std::atomic<int> serialize{0};
    std::atomic<int> deserialize{0};
    std::atomic<int> clone{0};

    void reset()
    {
        serialize.store(0, std::memory_order_relaxed);
        deserialize.store(0, std::memory_order_relaxed);
        clone.store(0, std::memory_order_relaxed);
    }

    SerCliCountStats() = default;
    SerCliCountStats(const SerCliCountStats&) {}   // independent counters on copy
    SerCliCountStats& operator=(const SerCliCountStats&) { return *this; }
};

class CountingRequest : public IpcMsgBase {
public:
    std::shared_ptr<SerCliCountStats> stats;
    int32_t value{0};
    std::string marker;

    CountingRequest() : stats(std::make_shared<SerCliCountStats>()) {}
    explicit CountingRequest(std::shared_ptr<SerCliCountStats> s) : stats(std::move(s)) {}

    ipc::buffer serialize() override
    {
        stats->serialize.fetch_add(1, std::memory_order_relaxed);
        int32_t marker_len = static_cast<int32_t>(marker.size());
        uint32_t total = sizeof(value) + sizeof(marker_len) + static_cast<uint32_t>(marker_len);
        ipc::buffer buf = serialize_data_cut(total);
        uint32_t offset = 0;
        uint16_t page = 1;
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&value), page, offset, sizeof(value));
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&marker_len), page, offset, sizeof(marker_len));
        if (marker_len > 0)
        {
            adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                             reinterpret_cast<const uint8_t*>(marker.data()), page, offset,
                             static_cast<uint32_t>(marker_len));
        }
        add_tail_msg(static_cast<uint8_t*>(buf.data()) + offset, page);
        return buf;
    }

    void deserialize(const ipc::buffer& buf) override
    {
        stats->deserialize.fetch_add(1, std::memory_order_relaxed);
        deserialize_data_cut(static_cast<uint32_t>(buf.size()));
        uint32_t offset = 0;
        int32_t marker_len = 0;
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&value), static_cast<const uint8_t*>(buf.data()),
                          offset, sizeof(value));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&marker_len), static_cast<const uint8_t*>(buf.data()),
                          offset, sizeof(marker_len));
        if (marker_len > 0)
        {
            marker.resize(static_cast<size_t>(marker_len));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(marker.data()),
                              static_cast<const uint8_t*>(buf.data()), offset,
                              static_cast<uint32_t>(marker_len));
        }
        else
        {
            marker.clear();
        }
    }

    CountingRequest* clone() const override
    {
        stats->clone.fetch_add(1, std::memory_order_relaxed);
        auto* c = new CountingRequest(stats);
        c->value = value;
        c->marker = marker;
        c->set_msg_id(dz_ipc_msg_id);
        return c;
    }
};

class CountingResponse : public IpcMsgBase {
public:
    std::shared_ptr<SerCliCountStats> stats;
    int32_t result{0};
    std::string marker;

    CountingResponse() : stats(std::make_shared<SerCliCountStats>()) {}
    explicit CountingResponse(std::shared_ptr<SerCliCountStats> s) : stats(std::move(s)) {}

    ipc::buffer serialize() override
    {
        stats->serialize.fetch_add(1, std::memory_order_relaxed);
        int32_t marker_len = static_cast<int32_t>(marker.size());
        uint32_t total = sizeof(result) + sizeof(marker_len) + static_cast<uint32_t>(marker_len);
        ipc::buffer buf = serialize_data_cut(total);
        uint32_t offset = 0;
        uint16_t page = 1;
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&result), page, offset, sizeof(result));
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                         reinterpret_cast<const uint8_t*>(&marker_len), page, offset, sizeof(marker_len));
        if (marker_len > 0)
        {
            adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()),
                             reinterpret_cast<const uint8_t*>(marker.data()), page, offset,
                             static_cast<uint32_t>(marker_len));
        }
        add_tail_msg(static_cast<uint8_t*>(buf.data()) + offset, page);
        return buf;
    }

    void deserialize(const ipc::buffer& buf) override
    {
        stats->deserialize.fetch_add(1, std::memory_order_relaxed);
        deserialize_data_cut(static_cast<uint32_t>(buf.size()));
        uint32_t offset = 0;
        int32_t marker_len = 0;
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&result), static_cast<const uint8_t*>(buf.data()),
                          offset, sizeof(result));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&marker_len), static_cast<const uint8_t*>(buf.data()),
                          offset, sizeof(marker_len));
        if (marker_len > 0)
        {
            marker.resize(static_cast<size_t>(marker_len));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(marker.data()),
                              static_cast<const uint8_t*>(buf.data()), offset,
                              static_cast<uint32_t>(marker_len));
        }
        else
        {
            marker.clear();
        }
    }

    CountingResponse* clone() const override
    {
        stats->clone.fetch_add(1, std::memory_order_relaxed);
        auto* c = new CountingResponse(stats);
        c->result = result;
        c->marker = marker;
        c->set_msg_id(dz_ipc_msg_id);
        return c;
    }
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

using namespace dzIPC;

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct SerCliPair {
    std::unique_ptr<shm::shm_ser_ipc> server;
    std::unique_ptr<shm::shm_cli_ipc> client;
    std::shared_ptr<SerCliCountStats> req_stats;
    std::shared_ptr<SerCliCountStats> rsp_stats;
    std::shared_ptr<ServiceData> srv_msg;
    std::shared_ptr<ServiceData> cli_msg;
};

SerCliPair make_ser_cli_pair(const std::string& topic, size_t domain, int msg_id = 0)
{
    SerCliPair pair;
    pair.req_stats = std::make_shared<SerCliCountStats>();
    pair.rsp_stats = std::make_shared<SerCliCountStats>();

    pair.srv_msg = std::make_shared<ServiceData>(
        std::make_shared<CountingRequest>(pair.req_stats),
        std::make_shared<CountingResponse>(pair.rsp_stats), msg_id);

    pair.cli_msg = std::make_shared<ServiceData>(
        std::make_shared<CountingRequest>(pair.req_stats),
        std::make_shared<CountingResponse>(pair.rsp_stats), msg_id);

    // Use a separate stats for server-side request/response to avoid
    // double-counting when both sides share the same template.
    auto srv_req_stats = std::make_shared<SerCliCountStats>();
    auto srv_rsp_stats = std::make_shared<SerCliCountStats>();
    auto srv_msg_data = std::make_shared<ServiceData>(
        std::make_shared<CountingRequest>(srv_req_stats),
        std::make_shared<CountingResponse>(srv_rsp_stats), msg_id);

    pair.server = std::make_unique<shm::shm_ser_ipc>(
        topic, srv_msg_data,
        [](std::shared_ptr<ServiceData>& msg)
        {
            auto req = std::static_pointer_cast<CountingRequest>(msg->request());
            auto rsp = std::static_pointer_cast<CountingResponse>(msg->response());
            rsp->result = req->value + 1;
            rsp->marker = "response_to_" + req->marker;
        },
        domain, false);

    pair.client = std::make_unique<shm::shm_cli_ipc>(topic, pair.cli_msg, domain, false);

    return pair;
}

/* ============================================================================
 * SC-01: Fast-path with EnableNodelet(true) — request-response via fast path.
 *
 * K=3 门槛: 前 2 次请求走 SHM，第 3 次进入快速路径。
 * 快速路径: 跳过 request serialize/response deserialize。
 * ============================================================================ */

TEST(ShmSerCliNodelet, SameProcessFastPath)
{
    dzIPC::EnableNodelet(true);

    auto pair = make_ser_cli_pair("sc_nodelet_fp", 1);
    pair.server->InitChannel();
    pair.client->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.client->handshake_completed(); }, 3000))
        << "client/server handshake timed out";

    // K=3 warmup: first 2 requests go through SHM.
    for (int i = 0; i < 2; ++i)
    {
        pair.cli_msg->request()->msgcast<CountingRequest>()->value = i;
        pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "warmup";
        ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500))
            << "warmup request " << i << " failed";
        auto rsp = pair.cli_msg->response()->msgcast<CountingResponse>();
        EXPECT_EQ(rsp->result, i + 1);
        EXPECT_EQ(rsp->marker, "response_to_warmup");
    }

    // Reset counters, 3rd request should use fast path.
    pair.req_stats->reset();
    pair.rsp_stats->reset();

    pair.cli_msg->request()->msgcast<CountingRequest>()->value = 42;
    pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "fp_target";
    ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500))
        << "fast-path request failed";

    auto rsp = pair.cli_msg->response()->msgcast<CountingResponse>();
    EXPECT_EQ(rsp->result, 43);
    EXPECT_EQ(rsp->marker, "response_to_fp_target");

    // Fast path: request is cloned but NOT serialized.
    EXPECT_EQ(pair.req_stats->serialize.load(), 0)
        << "fast path must not serialize request";
}

/* ============================================================================
 * SC-02: Default off — switch disabled, normal SHM path with serialization.
 * ============================================================================ */

TEST(ShmSerCliNodelet, DefaultDisabledUsesShm)
{
    // Ensure switch is off (default).
    dzIPC::EnableNodelet(false);

    auto pair = make_ser_cli_pair("sc_disabled", 2);
    pair.server->InitChannel();
    pair.client->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.client->handshake_completed(); }, 3000));

    // K=3 warmup + extra request.
    for (int i = 0; i < 4; ++i)
    {
        pair.cli_msg->request()->msgcast<CountingRequest>()->value = i;
        pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "shm_only";
        ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500)) << "request " << i;
    }

    pair.req_stats->reset();
    pair.cli_msg->request()->msgcast<CountingRequest>()->value = 100;
    pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "verify_shm";
    ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500));

    // Normal SHM path always serializes.
    EXPECT_GE(pair.req_stats->serialize.load(), 1)
        << "SHM path must serialize request";
    auto rsp = pair.cli_msg->response()->msgcast<CountingResponse>();
    EXPECT_EQ(rsp->result, 101);
}

/* ============================================================================
 * SC-03: No local server — fallback to SHM with warning.
 * ============================================================================ */

TEST(ShmSerCliNodelet, NoLocalServerWarnsAndFallsBack)
{
    dzIPC::EnableNodelet(true);

    // Redirect stderr to capture the warning.
    std::stringstream captured;
    auto* old_stderr = std::cerr.rdbuf(captured.rdbuf());

    // Create only a client, no server → no registry entry.
    auto stats = std::make_shared<SerCliCountStats>();
    auto cli_msg = std::make_shared<ServiceData>(
        std::make_shared<CountingRequest>(stats),
        std::make_shared<CountingResponse>(stats));
    auto client = std::make_unique<shm::shm_cli_ipc>("sc_no_server", cli_msg, 3, false);
    client->InitChannel();

    // No handshake completes (no server).
    ASSERT_FALSE(wait_until([&]() { return client->handshake_completed(); }, 500))
        << "handshake should not complete without server";

    // Multiple send_request calls — warning must appear exactly once.
    stats->reset();
    cli_msg->request()->msgcast<CountingRequest>()->value = 1;
    cli_msg->request()->msgcast<CountingRequest>()->marker = "no_srv";

    EXPECT_FALSE(client->send_request(cli_msg, 100))
        << "send_request should fail when no server is connected";

    EXPECT_FALSE(client->send_request(cli_msg, 100))
        << "second send_request should also fail";

    // Restore stderr.
    std::cerr.rdbuf(old_stderr);

    const std::string output = captured.str();
    const std::string needle = "nodelet requested but unavailable; falling back";

    // Count occurrences: must be exactly 1 (one-shot warning).
    size_t pos = 0;
    int count = 0;
    while ((pos = output.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    EXPECT_EQ(count, 1) << "one-shot warning must appear exactly once, got " << count
                         << ". Captured stderr:\n" << output;
}

/* ============================================================================
 * SC-04: Request-response correctness under fast path.
 *
 * Verify that the response value is exactly request.value + 1 for
 * multiple consecutive fast-path calls.
 * ============================================================================ */

TEST(ShmSerCliNodelet, RequestResponseCorrectness)
{
    dzIPC::EnableNodelet(true);

    auto pair = make_ser_cli_pair("sc_correctness", 4);
    pair.server->InitChannel();
    pair.client->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.client->handshake_completed(); }, 3000));

    // K=3 warmup.
    for (int i = 0; i < 2; ++i)
    {
        pair.cli_msg->request()->msgcast<CountingRequest>()->value = -1;
        pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "warmup";
        ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500));
    }

    // Multiple fast-path calls.
    for (int i = 0; i < 5; ++i)
    {
        pair.cli_msg->request()->msgcast<CountingRequest>()->value = i * 10;
        pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "fp_" + std::to_string(i);
        ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500))
            << "fast-path request " << i << " failed";

        auto rsp = pair.cli_msg->response()->msgcast<CountingResponse>();
        EXPECT_EQ(rsp->result, i * 10 + 1) << "response value mismatch at call " << i;
        EXPECT_EQ(rsp->marker, "response_to_fp_" + std::to_string(i));
    }
}

/* ============================================================================
 * SC-05: server_count > 1 anomaly — inject dummy queue to make snapshot
 * size == 2, verify one-shot anomaly warning, then SHM fallback still works.
 * ============================================================================ */

TEST(ShmSerCliNodelet, AnomalyServerCountFallback)
{
    dzIPC::EnableNodelet(true);

    auto pair = make_ser_cli_pair("sc_anomaly_multi", 5);
    pair.server->InitChannel();
    pair.client->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.client->handshake_completed(); }, 3000));

    // Inject a dummy queue under the same ChannelKey to force server_count == 2.
    const uint32_t req_msg_id = pair.cli_msg->request()->msg_id();
    ChannelKey key{"sc_anomaly_multi", 5, req_msg_id, ChannelKind::ShmService};
    auto dummy_queue = std::make_shared<CircularQueue<IpcMsgBase>>(1);
    LocalPubSubRegistry::instance().register_subscriber(key, dummy_queue);

    // Redirect stderr to capture the anomaly warning.
    std::stringstream captured;
    auto* old_stderr = std::cerr.rdbuf(captured.rdbuf());

    // Multiple requests — fall back to SHM (handshake is up), serialize > 0.
    pair.req_stats->reset();
    for (int i = 0; i < 4; ++i)
    {
        pair.cli_msg->request()->msgcast<CountingRequest>()->value = i;
        pair.cli_msg->request()->msgcast<CountingRequest>()->marker = "anomaly_fb";
        ASSERT_TRUE(pair.client->send_request(pair.cli_msg, 500))
            << "SHM fallback request " << i << " failed";
        auto rsp = pair.cli_msg->response()->msgcast<CountingResponse>();
        EXPECT_EQ(rsp->result, i + 1);
    }

    // SHM path always serializes.
    EXPECT_GE(pair.req_stats->serialize.load(), 1)
        << "SHM fallback must serialize request";

    // Restore stderr.
    std::cerr.rdbuf(old_stderr);

    // Anomaly warning must appear exactly once.
    const std::string output = captured.str();
    const std::string needle = "nodelet registry anomaly";
    size_t pos = 0;
    int count = 0;
    while ((pos = output.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    EXPECT_EQ(count, 1) << "anomaly warning must appear exactly once, got " << count
                        << ". Captured stderr:\n" << output;

    // Cleanup: unregister the dummy so it doesn't leak into other tests.
    LocalPubSubRegistry::instance().unregister_subscriber(key, dummy_queue);
}

}   // namespace
