#include "dzIPC/dzipc.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/srv_data.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

/* ============================================================================
 * test_nodelet_switch.cpp — Nodelet 开关行为独立验收测试
 *
 * 接口契约（arch_analyst 定稿）：
 *   dzIPC::EnableNodelet(bool)   — 进程级单一开关，atomic，默认 false
 *   dzIPC::IsNodeletEnabled()    — 读取开关状态
 *   位于 include/dzIPC/dzipc.h，不改任何现有工厂/构造函数签名
 *   SHM pub/sub、SHM ser/cli、UDP pub/sub 均读取该统一开关
 *
 * 测试范围：
 *   NS-01  SHM 工厂调用默认关闭 → 序列化证据
 *   NS-02  SHM 显式 EnableNodelet(true) → 快路径证据
 *   NS-03  SHM 开启但无本地 sub → 回退 SHM + warning
 *   NS-04  SHM 动态关闭 → 快路径停止
 *   NS-05  SHM publish_for_sniffer 永远 serialize
 *   NS-06  SHM warning 节流 — 至少出现一次且不刷屏
 *   NS-07  UDP 默认关闭/显式开启/条件不足回退 证据
 *   NS-08  SHM ser/cli 请求响应功能回归（双态）
 * ============================================================================ */

namespace {

/* ============================================================================
 * Test fixture: ensures IsNodeletEnabled() is restored to false after every
 * test, preventing cross-test pollution via the process-wide switch.
 * ============================================================================ */
class NodeletSwitchFixture : public ::testing::Test
{
protected:
    void TearDown() override
    {
        dzIPC::EnableNodelet(false);
    }
};

/* ############################################################################
 * Test infrastructure (self-contained; cannot share with test_shm_nodelet.cpp
 * due to anonymous namespace).
 * ############################################################################ */

struct CountStats {
    std::atomic<int> serialize{0};
    std::atomic<int> deserialize{0};
    std::atomic<int> clone{0};

    void reset()
    {
        serialize.store(0, std::memory_order_relaxed);
        deserialize.store(0, std::memory_order_relaxed);
        clone.store(0, std::memory_order_relaxed);
    }
    CountStats() = default;
    CountStats(const CountStats&) {}
    CountStats& operator=(const CountStats&) { return *this; }
};

class CountingMsg : public IpcMsgBase {
public:
    std::shared_ptr<CountStats> stats;
    int32_t value{0};
    std::string marker;

    CountingMsg() : stats(std::make_shared<CountStats>()) {}
    explicit CountingMsg(std::shared_ptr<CountStats> s) : stats(std::move(s)) {}

    ipc::buffer serialize() override
    {
        stats->serialize.fetch_add(1, std::memory_order_relaxed);
        const int32_t marker_len = static_cast<int32_t>(marker.size());
        const uint32_t total_data = sizeof(value) + sizeof(marker_len) + static_cast<uint32_t>(marker_len);
        ipc::buffer buf = serialize_data_cut(total_data);
        uint32_t offset = 0;
        uint16_t page = 1;
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()), reinterpret_cast<const uint8_t*>(&value), page, offset,
                         sizeof(value));
        adapt_memcpy_tos(static_cast<uint8_t*>(buf.data()), reinterpret_cast<const uint8_t*>(&marker_len), page, offset,
                         sizeof(marker_len));
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
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&value), static_cast<const uint8_t*>(buf.data()), offset,
                          sizeof(value));
        adapt_memcpy_tods(reinterpret_cast<uint8_t*>(&marker_len), static_cast<const uint8_t*>(buf.data()), offset,
                          sizeof(marker_len));
        if (marker_len > 0)
        {
            marker.resize(static_cast<size_t>(marker_len));
            adapt_memcpy_tods(reinterpret_cast<uint8_t*>(marker.data()), static_cast<const uint8_t*>(buf.data()), offset,
                              static_cast<uint32_t>(marker_len));
        }
        else { marker.clear(); }
    }

    CountingMsg* clone() const override
    {
        stats->clone.fetch_add(1, std::memory_order_relaxed);
        auto* c = new CountingMsg(stats);
        c->value = value;
        c->marker = marker;
        c->set_msg_id(dz_ipc_msg_id);
        return c;
    }
};

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

/* RAII stderr captor — POSIX only (Linux/macOS).
 * On Windows, test cases that need stderr capture are conditionally compiled out. */
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
class ScopedStderrCapture {
public:
    ScopedStderrCapture() : buf_()
    {
        fflush(stderr);
        saved_ = dup(fileno(stderr));
        int pipefd[2];
        if (pipe(pipefd) != 0)
        {
            /* pipe() failed — release saved_ fd, leave stderr untouched, mark no-op */
            close(saved_);
            saved_ = -1;
            read_fd_ = -1;
            return;
        }
        dup2(pipefd[1], fileno(stderr));
        close(pipefd[1]);
        read_fd_ = pipefd[0];
        int flags = fcntl(read_fd_, F_GETFL, 0);
        fcntl(read_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    ~ScopedStderrCapture()
    {
        if (read_fd_ == -1) return;   /* pipe failed in ctor, nothing to restore */
        fflush(stderr);
        dup2(saved_, fileno(stderr));
        close(saved_);
        close(read_fd_);
    }
    std::string drain()
    {
        if (read_fd_ == -1) return {};
        char tmp[4096];
        ssize_t n;
        while ((n = read(read_fd_, tmp, sizeof(tmp) - 1)) > 0)
        {
            tmp[n] = '\0';
            buf_ << tmp;
        }
        return buf_.str();
    }
    void reset() { buf_.str(""); buf_.clear(); }

private:
    int saved_{0};
    int read_fd_{-1};
    std::ostringstream buf_;
};
#else
/* Stub: no-op on unsupported platforms.  Tests depending on stderr capture
 * will compile but the warning assertions won't fire — acceptable for
 * non-Linux CI. */
class ScopedStderrCapture {
public:
    std::string drain() { return {}; }
    void reset() {}
};
#endif

/* warmup helper: publish 2× to approach K=3 threshold (SHM) */
void warmup_shm_fp(shm::shm_pub_ipc& pub, shm::shm_sub_ipc& sub,
                   std::shared_ptr<TopicData>& sub_topic,
                   std::shared_ptr<CountStats> stats)
{
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = i;
        m->marker = "warmup";
        pub.publish(m);
        ASSERT_TRUE(wait_until([&]() { return sub.try_get(sub_topic); }, 500))
            << "warmup receive failed at i=" << i;
    }
}

/* ===========================================================================
 * NS-01: SHM 工厂调用默认关闭 —— serialize 证据
 *
 * 分别通过 raw shm_pub_ipc 构造函数和 PublisherIPCPtrMake 工厂两种方式
 * 创建 publisher，默认状态下 nodelet 关闭，即使 K=3 满足也永远走 SHM
 * 序列化路径（每条消息 serialize ≥ 1）。
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmDefaultOffSerializeEvidence)
{
    EXPECT_FALSE(IsNodeletEnabled()) << "nodelet must be disabled by default";

    /* ----- Sub-test A: raw shm_pub_ipc ----- */
    {
        auto stats = std::make_shared<CountStats>();
        auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
        auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_dfl_raw", 20, false);

        auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
        auto sub = std::make_unique<shm::shm_sub_ipc>(sub_topic, "ns_dfl_raw", 20, 8, false);

        pub->InitChannel();
        sub->InitChannel();
        ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

        for (int i = 0; i < 10; ++i)
        {
            stats->reset();
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = i;
            m->marker = "raw";
            pub->publish(m);
            ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 500))
                << "raw pub: sub did not receive message " << i;

            EXPECT_GE(stats->serialize.load(), 1)
                << "Raw pub msg " << i << ": nodelet off MUST serialize every time";
            auto rcvd = sub_topic->topic()->msgcast<CountingMsg>();
            EXPECT_EQ(rcvd->value, i);
        }
    }

    /* ----- Sub-test B: PublisherIPCPtrMake factory ----- */
    {
        auto stats = std::make_shared<CountStats>();
        auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
        auto pub = PublisherIPCPtrMake(pub_topic, "ns_dfl_factory", 21, IPC_SHM, false);
        pub->InitChannel();

        auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
        auto sub = SubscriberIPCPtrMake(sub_topic, "ns_dfl_factory", 21, 8, IPC_SHM, false);
        sub->InitChannel();

        ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

        for (int i = 0; i < 6; ++i)
        {
            stats->reset();
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = i;
            m->marker = "factory";
            pub->publish(m);

            std::shared_ptr<TopicData> recv = TopicDataPtrMake<CountingMsg>();
            ASSERT_TRUE(wait_until([&]() { return sub->try_get(recv); }, 500))
                << "factory sub did not receive message " << i;

            EXPECT_GE(stats->serialize.load(), 1)
                << "Factory pub msg " << i << ": nodelet off MUST serialize every time";
        }
    }
}

/* ===========================================================================
 * NS-02: 显式 EnableNodelet(true) → SHM 快路径证据
 *
 * K=3 满足时 serialize=0, clone=1。
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmEnableFastPathEvidence)
{
    EnableNodelet(true);
    EXPECT_TRUE(IsNodeletEnabled());

    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_enable_fp", 22, false);

    auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub = std::make_unique<shm::shm_sub_ipc>(sub_topic, "ns_enable_fp", 22, 8, false);

    pub->InitChannel();
    sub->InitChannel();
    ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

    warmup_shm_fp(*pub, *sub, sub_topic, stats);

    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 42;
        m->marker = "fast_path";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 500));

    EXPECT_EQ(stats->serialize.load(), 0) << "fast path MUST NOT invoke serialize";
    EXPECT_EQ(stats->clone.load(), 1) << "fast path clones once";

    auto rcvd = sub_topic->topic()->msgcast<CountingMsg>();
    EXPECT_EQ(rcvd->value, 42);
    EXPECT_EQ(rcvd->marker, "fast_path");
}

/* ===========================================================================
 * NS-03: SHM 开启但条件不满足 → 回退 + warning
 *
 * EnableNodelet(true) 但无本地 subscriber → snapshot 为空 → 回退 SHM。
 * 每条消息必须 serialize，warning 至少出现一次。
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmEnableButFallbackWithWarning)
{
    EnableNodelet(true);

    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_fallback", 23, false);
    pub->InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ScopedStderrCapture cap;

    for (int i = 0; i < 5; ++i)
    {
        stats->reset();
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = i;
        m->marker = "fallback";
        pub->publish(m);
        EXPECT_GE(stats->serialize.load(), 1)
            << "Message " << i << ": must serialize (no local sub, fallback to SHM)";
    }

    std::string output = cap.drain();
    bool has_warning = output.find("nodelet requested but unavailable") != std::string::npos
                       || output.find("falling back") != std::string::npos;
    EXPECT_TRUE(has_warning)
        << "Expected warning 'nodelet requested but unavailable; falling back' at least once.\n"
        << "Captured stderr: " << output;
}

/* ===========================================================================
 * NS-04: 动态关闭后快速路径立即停止
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmToggleOffDisablesFastPath)
{
    EnableNodelet(true);

    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_toggle", 24, false);

    auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub = std::make_unique<shm::shm_sub_ipc>(sub_topic, "ns_toggle", 24, 8, false);

    pub->InitChannel();
    sub->InitChannel();
    ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

    warmup_shm_fp(*pub, *sub, sub_topic, stats);

    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 1;
        m->marker = "before_toggle";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 500));
    EXPECT_EQ(stats->serialize.load(), 0) << "fast path should be active before toggle off";

    EnableNodelet(false);

    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 2;
        m->marker = "after_toggle_off";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 500));
    EXPECT_GE(stats->serialize.load(), 1)
        << "after EnableNodelet(false), publish MUST invoke serialize";

    auto rcvd = sub_topic->topic()->msgcast<CountingMsg>();
    EXPECT_EQ(rcvd->value, 2);
    EXPECT_EQ(rcvd->marker, "after_toggle_off");
}

/* ===========================================================================
 * NS-05: SHM publish_for_sniffer 永远 serialize，不受开关影响
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmSnifferAlwaysSerializes)
{
    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_sniffer", 25, false);
    pub->InitChannel();

    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 10;
        m->marker = "sniff_off";
        pub->publish_for_sniffer(m);
    }
    EXPECT_GE(stats->serialize.load(), 1) << "sniffer must serialize when nodelet disabled";

    EnableNodelet(true);
    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 20;
        m->marker = "sniff_on";
        pub->publish_for_sniffer(m);
    }
    EXPECT_GE(stats->serialize.load(), 1) << "sniffer must serialize even when nodelet enabled";
}

/* ===========================================================================
 * NS-06: SHM warning 节流 —— 至少出现一次，但绝不刷屏
 *
 * EnableNodelet(true) 但无本地 sub → 连续 publish 50 条。
 * warning 出现 ≥1 次（需求明确要求条件不足打印 warning），
 * 且 ≤3 次（允许 stderr 缓冲合并，绝不出现 50 次）。
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmWarningThrottled)
{
    EnableNodelet(true);

    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "ns_throttle", 26, false);
    pub->InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ScopedStderrCapture cap;

    constexpr int kPublishCount = 50;
    for (int i = 0; i < kPublishCount; ++i)
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = i;
        m->marker = "throttle";
        pub->publish(m);
    }

    std::string output = cap.drain();

    size_t pos = 0;
    int warning_count = 0;
    const std::string needle = "nodelet requested but unavailable";
    while ((pos = output.find(needle, pos)) != std::string::npos)
    {
        ++warning_count;
        pos += needle.size();
    }

    EXPECT_GE(warning_count, 1)
        << "Warning 'nodelet requested but unavailable' MUST appear at least once (requirement mandates "
        << "warning when nodelet enabled but conditions not met). Got 0 in " << kPublishCount
        << " publishes.\nCaptured stderr: " << output;

    EXPECT_LE(warning_count, 3)
        << "Warning MUST be throttled (≤3 for " << kPublishCount << " publishes), got "
        << warning_count << ".\nCaptured stderr: " << output;
}

/* ===========================================================================
 * NS-07: UDP nodelet —— 默认关闭 / 显式开启 / 条件不足回退 证据
 *
 * UDP (socket) pub/sub 同样读取统一开关 dzIPC::IsNodeletEnabled()。
 *   7a: 默认关闭 → 始终走 UDP serialize
 *   7b: 显式开启 → K=3 快路径生效
 *   7c: 开启但无本地 sub → 回退 UDP serialize + warning
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, UdpNodeletEvidence)
{
    /* ----- 7a: UDP 默认关闭 → serialize 证据 ----- */
    {
        EXPECT_FALSE(IsNodeletEnabled());

        auto stats = std::make_shared<CountStats>();
        auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
        auto pub = std::make_unique<socket::socket_pub_ipc>(pub_topic, "ns_udp_off", 0, false);

        auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
        auto sub = std::make_unique<socket::socket_sub_ipc>(sub_topic, "ns_udp_off", 0, 8, false);

        pub->InitChannel();
        sub->InitChannel();

        /* socket_pub_ipc::has_subscribed() is a stub (subscribed_ never updated);
         * confirm readiness by bounded publish + try_get retry loop. */
        {
            bool synced = false;
            for (int retry = 0; retry < 20 && !synced; ++retry)
            {
                auto m = std::make_shared<CountingMsg>(stats);
                m->value = -1;
                m->marker = "sync";
                pub->publish(m);
                synced = wait_until([&]() { return sub->try_get(sub_topic); }, 300);
            }
            ASSERT_TRUE(synced) << "UDP pub/sub sync timed out (default off)";
        }

        /* Publish 6 messages (>K=3). Every one must serialize. */
        for (int i = 0; i < 6; ++i)
        {
            stats->reset();
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = i;
            m->marker = "udp_off";
            pub->publish(m);
            ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 1000))
                << "UDP sub did not receive message " << i << " (default off)";

            EXPECT_GE(stats->serialize.load(), 1)
                << "UDP msg " << i << ": nodelet off MUST serialize every time";
        }
    }

    /* ----- 7b: UDP 显式开启 → 快路径证据 ----- */
    {
        EnableNodelet(true);
        EXPECT_TRUE(IsNodeletEnabled());

        auto stats = std::make_shared<CountStats>();
        auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
        auto pub = std::make_unique<socket::socket_pub_ipc>(pub_topic, "ns_udp_on", 1, false);

        auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
        auto sub = std::make_unique<socket::socket_sub_ipc>(sub_topic, "ns_udp_on", 1, 8, false);

        pub->InitChannel();
        sub->InitChannel();

        /* bounded publish+try_get to confirm UDP path is ready */
        {
            bool synced = false;
            for (int retry = 0; retry < 20 && !synced; ++retry)
            {
                auto m = std::make_shared<CountingMsg>(stats);
                m->value = -1;
                m->marker = "sync";
                pub->publish(m);
                synced = wait_until([&]() { return sub->try_get(sub_topic); }, 300);
            }
            ASSERT_TRUE(synced) << "UDP pub/sub sync timed out (enabled)";
        }

        /* Warmup 2× → K=2 */
        for (int i = 0; i < 2; ++i)
        {
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = -1;
            m->marker = "warmup";
            pub->publish(m);
            ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 1000))
                << "UDP warmup receive failed at i=" << i;
        }

        /* 3rd publish → fast path */
        stats->reset();
        {
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = 77;
            m->marker = "udp_fast";
            pub->publish(m);
        }
        ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 1000));

        EXPECT_EQ(stats->serialize.load(), 0)
            << "UDP fast path: serialize MUST be 0";
        EXPECT_EQ(stats->clone.load(), 1)
            << "UDP fast path: clone must be 1";

        auto rcvd = sub_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd->value, 77);
        EXPECT_EQ(rcvd->marker, "udp_fast");
    }

    /* ----- 7c: UDP 开启但无本地 sub → 回退 + warning ----- */
    {
        EnableNodelet(true);

        auto stats = std::make_shared<CountStats>();
        auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
        auto pub = std::make_unique<socket::socket_pub_ipc>(pub_topic, "ns_udp_nosub", 2, false);
        pub->InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ScopedStderrCapture cap;

        for (int i = 0; i < 5; ++i)
        {
            stats->reset();
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = i;
            m->marker = "udp_fb";
            pub->publish(m);
            EXPECT_GE(stats->serialize.load(), 1)
                << "UDP no-sub msg " << i << ": must serialize (fallback)";
        }

        std::string output = cap.drain();
        bool has_warning = output.find("nodelet requested but unavailable") != std::string::npos
                           || output.find("falling back") != std::string::npos;
        EXPECT_TRUE(has_warning)
            << "UDP: expected warning 'nodelet requested but unavailable; falling back' at least once.\n"
            << "Captured stderr: " << output;
    }
}

/* ===========================================================================
 * NS-08: SHM ser/cli 请求响应功能回归 —— 开关双态下均正常完成请求响应
 *
 * SHM service/client 使用 ipc::server 双向通道。本测试仅做功能回归验证：
 * 在 nodelet=false 和 nodelet=true 两种状态下，ser/cli 对均能正常
 * 完成握手、发送请求并收到正确响应。快速路径语义证据由
 * test_shm_ser_cli_nodelet 覆盖。
 * =========================================================================== */
TEST_F(NodeletSwitchFixture, ShmServiceClientFunctionalRegression)
{
    for (bool enabled : {false, true})
    {
        EnableNodelet(enabled);

        auto srv_msg = std::make_shared<ServiceData>(
            std::make_shared<Srv::RequestResponseTestRequest>(),
            std::make_shared<Srv::RequestResponseTestResponse>());

        std::atomic<int> serve_count{0};

        shm::shm_ser_ipc server("ns_sercli_test", srv_msg,
            [&](std::shared_ptr<ServiceData>& msg)
            {
                auto req = std::static_pointer_cast<Srv::RequestResponseTestRequest>(msg->request());
                auto res = std::static_pointer_cast<Srv::RequestResponseTestResponse>(msg->response());
                res->response.resize(req->request.size());
                for (size_t j = 0; j < req->request.size(); ++j)
                    res->response[j] = req->request[j] + 1.0;
                ++serve_count;
            },
            30, false);

        auto cli_msg = std::make_shared<ServiceData>(
            std::make_shared<Srv::RequestResponseTestRequest>(),
            std::make_shared<Srv::RequestResponseTestResponse>());
        shm::shm_cli_ipc client("ns_sercli_test", cli_msg, 30, false);

        /* Must Init both ends before waiting for either handshake,
         * otherwise server may never see a peer and handshake times out. */
        server.InitChannel();
        client.InitChannel();

        EXPECT_TRUE(wait_until([&]() { return server.handshake_completed(); }, 5000))
            << "Server handshake timed out with nodelet=" << enabled;
        EXPECT_TRUE(wait_until([&]() { return client.handshake_completed(); }, 5000))
            << "Client handshake timed out with nodelet=" << enabled;

        auto req_ptr = std::make_shared<ServiceData>(
            std::make_shared<Srv::RequestResponseTestRequest>(),
            std::make_shared<Srv::RequestResponseTestResponse>());
        {
            auto req = std::static_pointer_cast<Srv::RequestResponseTestRequest>(req_ptr->request());
            req->request = {1.0, 2.0, 3.0};
        }

        bool sent = client.send_request(req_ptr, 1000);
        EXPECT_TRUE(sent) << "send_request failed with nodelet=" << enabled;

        EXPECT_GE(serve_count.load(), 1) << "Server did not process request with nodelet=" << enabled;
        {
            auto res = std::static_pointer_cast<Srv::RequestResponseTestResponse>(req_ptr->response());
            ASSERT_EQ(res->response.size(), 3u);
            EXPECT_DOUBLE_EQ(res->response[0], 2.0);
            EXPECT_DOUBLE_EQ(res->response[1], 3.0);
            EXPECT_DOUBLE_EQ(res->response[2], 4.0);
        }
    }
}

}   // namespace
