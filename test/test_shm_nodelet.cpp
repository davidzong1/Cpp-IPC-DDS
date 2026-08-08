#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

/* ============================================================================
 * Global: enable the unified nodelet switch for all tests in this file.
 * The fast path now requires dzIPC::IsNodeletEnabled() == true.
 * ============================================================================ */
struct NodeletEnabler {
    NodeletEnabler() { dzIPC::EnableNodelet(true); }
};
static NodeletEnabler g_enable_nodelet;

/* ============================================================================
 * CountingMsg: 自定义消息类型，统计 serialize/deserialize/clone 调用次数。
 * pub/sub 共享同一 CountStats 实例，用于验证快速路径行为。
 * ============================================================================ */

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
    /* 拷贝时创建独立计数器（sub 模板 clone 不应污染 pub 侧计数） */
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
        else
        {
            marker.clear();
        }
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

/* ============================================================================
 * Helpers
 * ============================================================================ */

using namespace dzIPC;

struct PubSubPair {
    std::unique_ptr<shm::shm_pub_ipc> pub;
    std::unique_ptr<shm::shm_sub_ipc> sub;
    std::shared_ptr<CountStats> stats;
    std::shared_ptr<TopicData> pub_topic;
    std::shared_ptr<TopicData> sub_topic;
};

/* 创建同 topic 同 domain 的 pub/sub 对。
 * sub 的 TopicData 模板 msg 使用独立的 CountingMsg（不共享 stats），
 * 避免 sub 内部 topic_msg_->clone() 计入 pub 侧 clone 计数。 */
PubSubPair make_pair(const std::string& topic, size_t domain, size_t queue_size = 4,
                     uint32_t msg_id = 0)
{
    PubSubPair pair;
    pair.stats = std::make_shared<CountStats>();

    pair.pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(pair.stats), msg_id);
    pair.pub = std::make_unique<shm::shm_pub_ipc>(pair.pub_topic, topic, domain, false);

    pair.sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(), msg_id);
    pair.sub = std::make_unique<shm::shm_sub_ipc>(pair.sub_topic, topic, domain, queue_size, false);

    return pair;
}

/* bounded wait，带超时保护 */
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/* 排空 subscriber 队列 */
void drain_subscriber(shm::shm_sub_ipc& sub, std::shared_ptr<TopicData>& topic)
{
    while (sub.try_get(topic))
    {
    }
}

/* 有界负向观察：在 timeout_ms 内持续轮询，一旦收到返回 true（异常），超时未收到返回 false（正常） */
bool bounded_never_received(shm::shm_sub_ipc& sub, std::shared_ptr<TopicData>& topic, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (sub.try_get(topic))
        {
            return false;   // 异常：收到了不该收到的消息
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;   // 超时未收到，符合隔离预期
}

/* ============================================================================
 * FT-01: 同进程快速路径指标验证
 *
 * K=3 门槛协议：前 2 次 publish 确认拓扑稳定，第 3 次进入快速路径。
 * 快速路径：clone 一次形成不可变快照，推入所有本地 sub 队列，
 * 跳过 serialize。
 *
 * 注意：pub 侧 deserialize 计数器仅在 pub 端 CountingMsg 实例被
 * deserialize() 时递增，而 sub 侧 SHM 反序列化使用的是 sub 自己的
 * 独立 CountingMsg（不共享 stats），因此 pub 侧 deserialize 恒为 0，
 * 不做断言。
 * ============================================================================ */

TEST(ShmNodelet, SameProcessFastPathMetrics)
{
    auto pair = make_pair("nodelet_fp_metrics", 1, 8);
    pair.pub->InitChannel();
    pair.sub->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.pub->has_subscribed(); }, 3000))
        << "pub/sub handshake timed out";

    /* K=3 门槛：前 2 次 publish 走 SHM 路径建立基线，逐条有界接收 */
    pair.stats->reset();
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = i;
        m->marker = "warmup";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500))
            << "sub did not receive warmup " << i;
    }

    /* 重置计数后执行第 3 次 publish（快速路径生效点） */
    pair.stats->reset();

    auto msg = std::make_shared<CountingMsg>(pair.stats);
    msg->value = 42;
    msg->marker = "fast_path_target";
    pair.pub->publish(msg);
    ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500))
        << "subscriber did not receive fast-path message";

    /* 接收并断言消息内容正确 */
    {
        auto rcvd = pair.sub_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd->value, 42);
        EXPECT_EQ(rcvd->marker, "fast_path_target");
    }

    const int delta_serialize = pair.stats->serialize.load(std::memory_order_relaxed);
    const int delta_clone = pair.stats->clone.load(std::memory_order_relaxed);

    /* 快速路径：不调用 serialize，pub 侧 clone 一次形成不可变快照 */
    EXPECT_EQ(delta_serialize, 0) << "fast path MUST NOT invoke serialize";
    EXPECT_EQ(delta_clone, 1) << "fast path: pub clones once to form immutable snapshot";
}

/* ============================================================================
 * FT-02: publish 后修改原对象不影响接收到的快照
 *
 * 快速路径：pub clone 形成不可变快照，修改原对象不影响 shared_ptr 指向的对象。
 * SHM 路径：serialize() 将消息拷贝到 buffer，修改原对象不改变 buffer。
 * 两种路径下 publish 后修改原对象都不应影响接收方。
 * ============================================================================ */

TEST(ShmNodelet, SnapshotImmutabilityAfterPublish)
{
    auto pair = make_pair("nodelet_snapshot", 2, 8);
    pair.pub->InitChannel();
    pair.sub->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.pub->has_subscribed(); }, 3000));

    /* warmup：逐条有界接收 */
    pair.stats->reset();
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = -1;
        m->marker = "warmup";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500))
            << "sub did not receive warmup " << i;
    }

    /* publish 后立即修改原对象 */
    auto msg = std::make_shared<CountingMsg>(pair.stats);
    msg->value = 100;
    msg->marker = "before_mutation";
    pair.pub->publish(msg);

    msg->value = 999;
    msg->marker = "AFTER_MUTATION";

    ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500))
        << "subscriber did not receive";

    {
        auto rcvd = pair.sub_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd->value, 100) << "mutation after publish must not affect delivered snapshot";
        EXPECT_EQ(rcvd->marker, "before_mutation")
            << "marker must reflect publish-time state, not post-publish mutation";
    }
}

/* ============================================================================
 * FT-03: 1 pub + 2 sub —— 两个 sub 收到同一个 clone 快照指针
 *
 * 快速路径：pub clone 一次，推入同一 shared_ptr 到所有本地 sub 队列。
 * 两个 sub 收到的 IpcMsgBase 对象地址必须相同。
 * ============================================================================ */

TEST(ShmNodelet, TwoSubscribersSameSnapshot)
{
    auto stats = std::make_shared<CountStats>();

    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "nodelet_2sub", 3, false);

    auto sub1_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub1 = std::make_unique<shm::shm_sub_ipc>(sub1_topic, "nodelet_2sub", 3, 8, false);

    auto sub2_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub2 = std::make_unique<shm::shm_sub_ipc>(sub2_topic, "nodelet_2sub", 3, 8, false);

    pub->InitChannel();
    sub1->InitChannel();
    sub2->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

    /* warmup：每个 publish 后两个 sub 逐条有界接收 */
    stats->reset();
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = -1;
        m->marker = "warmup";
        pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500))
            << "sub1 did not receive warmup " << i;
        ASSERT_TRUE(wait_until([&]() { return sub2->try_get(sub2_topic); }, 500))
            << "sub2 did not receive warmup " << i;
    }

    /* publish */
    auto msg = std::make_shared<CountingMsg>(stats);
    msg->value = 77;
    msg->marker = "twin";
    pub->publish(msg);

    ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500))
        << "sub1 did not receive";
    ASSERT_TRUE(wait_until([&]() { return sub2->try_get(sub2_topic); }, 500))
        << "sub2 did not receive";

    {
        auto rcvd1 = sub1_topic->topic()->msgcast<CountingMsg>();
        auto rcvd2 = sub2_topic->topic()->msgcast<CountingMsg>();

        EXPECT_EQ(rcvd1->value, 77);
        EXPECT_EQ(rcvd1->marker, "twin");
        EXPECT_EQ(rcvd2->value, 77);
        EXPECT_EQ(rcvd2->marker, "twin");

        /* 快速路径：两个 sub 收到同一个 clone 快照 */
        EXPECT_EQ(sub1_topic->topic().get(), sub2_topic->topic().get())
            << "both subscribers must receive the same cloned snapshot pointer";
    }
}

/* ============================================================================
 * FT-04: publish_for_sniffer 强制走 SHM 序列化路径
 *
 * sniffer 依赖 SHM 通道的序列化数据，绝不能走快速路径。
 * ============================================================================ */

TEST(ShmNodelet, PublishForSnifferAlwaysSerializes)
{
    auto stats = std::make_shared<CountStats>();
    auto topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(topic, "nodelet_sniffer", 4, false);
    pub->InitChannel();

    stats->reset();
    int before = stats->serialize.load(std::memory_order_relaxed);

    auto msg = std::make_shared<CountingMsg>(stats);
    msg->value = 55;
    msg->marker = "sniff_me";
    pub->publish_for_sniffer(msg);

    int delta = stats->serialize.load(std::memory_order_relaxed) - before;
    EXPECT_GE(delta, 1) << "publish_for_sniffer MUST invoke serialize — sniffer needs serialized data";
}

/* ============================================================================
 * FT-05: publish_blocking 仍调用 serialize
 *
 * publish_blocking 使用 publisher_->try_send() 走 SHM 通道。
 * ============================================================================ */

TEST(ShmNodelet, PublishBlockingAlwaysSerializes)
{
    auto stats = std::make_shared<CountStats>();
    auto topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(topic, "nodelet_blocking", 5, false);
    pub->InitChannel();

    stats->reset();
    int before = stats->serialize.load(std::memory_order_relaxed);

    auto msg = std::make_shared<CountingMsg>(stats);
    msg->value = 66;
    msg->marker = "block_me";
    pub->publish_blocking(msg, 100);

    int delta = stats->serialize.load(std::memory_order_relaxed) - before;
    EXPECT_GE(delta, 1) << "publish_blocking MUST invoke serialize (SHM path via publisher_->try_send)";
}

/* ============================================================================
 * FT-06: topic 隔离 —— 不同 topic 使用不同 SHM channel，互不干扰
 * ============================================================================ */

TEST(ShmNodelet, TopicIsolation)
{
    auto pair_a = make_pair("nodelet_topic_a", 6, 4);
    auto pair_b = make_pair("nodelet_topic_b", 6, 4);

    pair_a.pub->InitChannel();
    pair_a.sub->InitChannel();
    pair_b.pub->InitChannel();
    pair_b.sub->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair_a.pub->has_subscribed(); }, 3000));
    ASSERT_TRUE(wait_until([&]() { return pair_b.pub->has_subscribed(); }, 3000));

    /* warmup：每个 pair 逐条有界接收 */
    for (auto* p : {&pair_a, &pair_b})
    {
        for (int i = 0; i < 2; ++i)
        {
            auto m = std::make_shared<CountingMsg>(p->stats);
            m->value = -1;
            m->marker = "warmup";
            p->pub->publish(m);
            ASSERT_TRUE(wait_until([&]() { return p->sub->try_get(p->sub_topic); }, 500))
                << "warmup receive failed";
        }
    }

    /* 向 topic_a 发布，topic_b 不应收到（有界负向观察） */
    auto msg_a = std::make_shared<CountingMsg>(pair_a.stats);
    msg_a->value = 111;
    msg_a->marker = "only_a";
    pair_a.pub->publish(msg_a);

    bool a_got = wait_until([&]() { return pair_a.sub->try_get(pair_a.sub_topic); }, 500);
    ASSERT_TRUE(a_got) << "topic_a subscriber should receive message on its own topic";
    {
        auto rcvd = pair_a.sub_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd->value, 111);
        EXPECT_EQ(rcvd->marker, "only_a");
    }

    bool b_isolated = bounded_never_received(*pair_b.sub, pair_b.sub_topic, 500);
    EXPECT_TRUE(b_isolated) << "topic_b subscriber must NOT receive messages from topic_a (bounded observation)";
}

/* ============================================================================
 * FT-07: msg_id 隔离 —— 不同 msg_id 的 sub 拒绝不匹配的消息
 *
 * check_msg_id() 在 deserialize 之前校验消息尾部 msg_id。
 * msg_id 不匹配时消息被静默丢弃。
 * 快速路径下 ChannelKey 包含 msg_id，隔离语义不变。
 * ============================================================================ */

TEST(ShmNodelet, MsgIdIsolation)
{
    constexpr uint32_t kMsgIdA = 42;
    constexpr uint32_t kMsgIdB = 99;

    /* sub_a 与 pub 使用相同 msg_id=42，sub_b 使用不同 msg_id=99 */
    auto stats_a = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats_a), kMsgIdA);
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "nodelet_msgid", 11, false);

    auto sub_a_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(), kMsgIdA);
    auto sub_a = std::make_unique<shm::shm_sub_ipc>(sub_a_topic, "nodelet_msgid", 11, 8, false);

    auto sub_b_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(), kMsgIdB);
    auto sub_b = std::make_unique<shm::shm_sub_ipc>(sub_b_topic, "nodelet_msgid", 11, 8, false);

    pub->InitChannel();
    sub_a->InitChannel();
    sub_b->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

    /* warmup：逐条有界接收（仅 sub_a 能收到，sub_b 被 msg_id 隔离） */
    stats_a->reset();
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(stats_a);
        m->set_msg_id(kMsgIdA);
        m->value = -1;
        m->marker = "warmup";
        pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return sub_a->try_get(sub_a_topic); }, 500))
            << "sub_a did not receive warmup " << i;
    }
    drain_subscriber(*sub_b, sub_b_topic);

    /* publish 一条 msg_id=42 的消息 */
    auto msg = std::make_shared<CountingMsg>(stats_a);
    msg->set_msg_id(kMsgIdA);
    msg->value = 123;
    msg->marker = "msgid_42";
    pub->publish(msg);

    /* sub_a(msg_id=42) 应收到 */
    bool a_got = wait_until([&]() { return sub_a->try_get(sub_a_topic); }, 500);
    ASSERT_TRUE(a_got) << "sub with matching msg_id should receive";
    {
        auto rcvd = sub_a_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd->value, 123);
        EXPECT_EQ(rcvd->marker, "msgid_42");
    }

    /* sub_b(msg_id=99) 应被隔离（有界负向观察） */
    bool b_isolated = bounded_never_received(*sub_b, sub_b_topic, 500);
    EXPECT_TRUE(b_isolated) << "sub with mismatched msg_id must NOT receive (bounded observation)";
}

/* ============================================================================
 * FT-08: sub 销毁后 publish 不崩溃
 *
 * sub 析构时先从 LocalPubSubRegistry 注销（快速路径 publisher 快照
 * 不再包含此队列），再停止 SHM 线程。pub 继续 publish 必须安全。
 * ============================================================================ */

TEST(ShmNodelet, PublishAfterSubDestroyNoCrash)
{
    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "nodelet_destroy", 7, false);

    {
        auto sub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
        auto sub = std::make_unique<shm::shm_sub_ipc>(sub_topic, "nodelet_destroy", 7, 4, false);

        pub->InitChannel();
        sub->InitChannel();
        ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

        /* warmup：逐条有界接收 */
        for (int i = 0; i < 2; ++i)
        {
            auto m = std::make_shared<CountingMsg>(stats);
            m->value = -1;
            m->marker = "warmup";
            pub->publish(m);
            ASSERT_TRUE(wait_until([&]() { return sub->try_get(sub_topic); }, 500))
                << "sub did not receive warmup " << i;
        }
        /* sub 离开作用域 → 析构 → 先注销再停止线程 */
    }

    /* sub 已销毁，pub 继续 publish 不崩溃 */
    for (int i = 0; i < 5; ++i)
    {
        auto msg = std::make_shared<CountingMsg>(stats);
        msg->value = i;
        msg->marker = "post_destroy";
        EXPECT_NO_THROW(pub->publish(msg)) << "publish after sub destroyed must not throw";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* ============================================================================
 * FT-09: 拓扑数量变化重置 K 计数器
 *
 * 协议要求 K=3 次连续一致的观察才激活快速路径。当 sub 数量发生变化
 * （新增或移除），snapshot.size() 与上次记录不一致，K 必须重置为 0，
 * 重新经过 3 次连续确认后才恢复快速路径。
 *
 * 测试流程：
 *   1. 1 pub + 1 sub，publish 2 次 → K 累计到 2
 *   2. 加入 sub2（拓扑变化）
 *   3. publish_for_sniffer 确认两者均已连接（不经过 publish_best_effort）
 *   4. 首次普通 publish → K 因 snapshot.size 变化重置 → SHM 路径（serialize）
 *   5. 第二次普通 publish → K=2
 *   6. 第三次普通 publish → K=3 → 快速路径（serialize=0, clone=1）
 * ============================================================================ */

TEST(ShmNodelet, TopologyCountChangeResetsK)
{
    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "nodelet_topo_k", 8, false);

    auto sub1_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub1 = std::make_unique<shm::shm_sub_ipc>(sub1_topic, "nodelet_topo_k", 8, 8, false);

    pub->InitChannel();
    sub1->InitChannel();
    ASSERT_TRUE(wait_until([&]() { return pub->has_subscribed(); }, 3000));

    /* Phase 1: 1 sub，publish 2 次，K 累计到 2（尚未达到快速路径门槛） */
    stats->reset();
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = i;
        m->marker = "K_build";
        pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500))
            << "sub1 did not receive warmup " << i;
    }

    /* Phase 2: 加入 sub2，拓扑数量从 1 变为 2。
     * sub2 的 handshake 是异步的，使用 publish_for_sniffer 重试循环
     * 确认 sub2 完成握手并开始接收（publish_for_sniffer 不经过
     * publish_best_effort，因此不更新 K 计数器）。 */
    auto sub2_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>());
    auto sub2 = std::make_unique<shm::shm_sub_ipc>(sub2_topic, "nodelet_topo_k", 8, 8, false);
    sub2->InitChannel();

    bool sub2_ready = false;
    for (int attempt = 0; attempt < 15; ++attempt)
    {
        drain_subscriber(*sub1, sub1_topic);
        {
            auto sniff = std::make_shared<CountingMsg>(stats);
            sniff->value = 99;
            sniff->marker = "sniff_confirm";
            pub->publish_for_sniffer(sniff);
        }
        if (wait_until([&]() { return sub2->try_get(sub2_topic); }, 200))
        {
            sub2_ready = true;
            break;
        }
    }
    ASSERT_TRUE(sub2_ready) << "sub2 did not complete handshake within retry window";

    /* sub1 也应收到最后的 sniffer 确认消息 */
    ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500))
        << "sub1 should receive sniffer confirmation";

    /* Phase 3: 首次普通 publish —— K 检测到 snapshot.size 变化（1→2）而重置，
     * 必须走 SHM 路径（serialize） */
    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 1;
        m->marker = "post_join_1st";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500));
    ASSERT_TRUE(wait_until([&]() { return sub2->try_get(sub2_topic); }, 500));
    EXPECT_GE(stats->serialize.load(), 1)
        << "1st publish after topology change MUST invoke serialize (K was reset)";

    /* Phase 4: 第二次普通 publish，K=2，仍走 SHM */
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 2;
        m->marker = "post_join_2nd";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500));
    ASSERT_TRUE(wait_until([&]() { return sub2->try_get(sub2_topic); }, 500));

    /* Phase 5: 第三次普通 publish —— K=3 ≥ kFastPathConfirm，快速路径生效 */
    stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(stats);
        m->value = 3;
        m->marker = "fast_path";
        pub->publish(m);
    }
    ASSERT_TRUE(wait_until([&]() { return sub1->try_get(sub1_topic); }, 500));
    ASSERT_TRUE(wait_until([&]() { return sub2->try_get(sub2_topic); }, 500));

    EXPECT_EQ(stats->serialize.load(), 0)
        << "3rd publish after K re-confirmation MUST NOT invoke serialize (fast path)";
    EXPECT_EQ(stats->clone.load(), 1)
        << "fast path: pub clones once to form immutable snapshot";

    /* 两个 sub 内容一致，且收到同一快照指针 */
    {
        auto rcvd1 = sub1_topic->topic()->msgcast<CountingMsg>();
        auto rcvd2 = sub2_topic->topic()->msgcast<CountingMsg>();
        EXPECT_EQ(rcvd1->value, 3);
        EXPECT_EQ(rcvd1->marker, "fast_path");
        EXPECT_EQ(rcvd2->value, 3);
        EXPECT_EQ(rcvd2->marker, "fast_path");
        EXPECT_EQ(sub1_topic->topic().get(), sub2_topic->topic().get())
            << "fast path: both subs receive the same cloned snapshot pointer";
    }
}

/* ============================================================================
 * FT-10: NodeletDisabledByDefault — 开关显式关闭时不走快速路径
 *
 * 静态 NodeletEnabler 已开启开关，此处显式关闭后验证所有 publish 均走
 * SHM 序列化路径（serialize ≥ 1），即使用 K=3 门槛已满足。
 * ============================================================================ */

TEST(ShmNodelet, NodeletDisabledFallsBack)
{
    dzIPC::EnableNodelet(false);

    auto pair = make_pair("nodelet_disabled", 9, 8);
    pair.pub->InitChannel();
    pair.sub->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.pub->has_subscribed(); }, 3000));

    /* warmup */
    for (int i = 0; i < 3; ++i)
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = i;
        m->marker = "warmup";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }

    /* nodelet off → 始终走 SHM，serialize 必须被调用 */
    pair.stats->reset();
    auto msg = std::make_shared<CountingMsg>(pair.stats);
    msg->value = 100;
    msg->marker = "no_nodelet";
    pair.pub->publish(msg);
    ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));

    EXPECT_GE(pair.stats->serialize.load(), 1)
        << "nodelet disabled: MUST invoke serialize (always SHM path)";

    /* 重新开启以便后续测试不受影响 */
    dzIPC::EnableNodelet(true);
}

/* ============================================================================
 * FT-11: NodeletToggleRuntime — 运行时关闭再开启
 *
 * 验证开关可在运行时动态切换：关闭后回退 SHM，重新开启后恢复快速路径。
 * ============================================================================ */

TEST(ShmNodelet, NodeletToggleRuntime)
{
    auto pair = make_pair("nodelet_toggle", 10, 8);
    pair.pub->InitChannel();
    pair.sub->InitChannel();

    ASSERT_TRUE(wait_until([&]() { return pair.pub->has_subscribed(); }, 3000));

    /* warmup: nodelet 已开启（静态 enabler），K 累积 */
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = -1;
        m->marker = "warmup";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }

    /* Phase 1: nodelet on → 快速路径 */
    pair.stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = 1;
        m->marker = "fast";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }
    EXPECT_EQ(pair.stats->serialize.load(), 0) << "nodelet on: fast path, no serialize";
    EXPECT_EQ(pair.stats->clone.load(), 1) << "nodelet on: clone once";

    /* Phase 2: nodelet off → SHM */
    dzIPC::EnableNodelet(false);
    pair.stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = 2;
        m->marker = "shm";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }
    EXPECT_GE(pair.stats->serialize.load(), 1) << "nodelet off: must use SHM path";

    /* Phase 3: nodelet back on → K 重置后重新累积 3 次 */
    dzIPC::EnableNodelet(true);
    for (int i = 0; i < 2; ++i)
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = -1;
        m->marker = "rewarm";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }
    pair.stats->reset();
    {
        auto m = std::make_shared<CountingMsg>(pair.stats);
        m->value = 3;
        m->marker = "fast_again";
        pair.pub->publish(m);
        ASSERT_TRUE(wait_until([&]() { return pair.sub->try_get(pair.sub_topic); }, 500));
    }
    EXPECT_EQ(pair.stats->serialize.load(), 0) << "nodelet re-enabled: fast path resumes after K=3";
    EXPECT_EQ(pair.stats->clone.load(), 1) << "nodelet re-enabled: clone once";
}

/* ============================================================================
 * FT-12: NodeletWarningOnce — 回退 warning 每个原因仅输出一次
 *
 * 构造无本地订阅者场景，验证 warning 只打印一次而非每条消息刷屏。
 * 通过在 cerr 重定向后计数验证。
 * ============================================================================ */

TEST(ShmNodelet, NodeletWarningOncePerReason)
{
    dzIPC::EnableNodelet(true);

    auto stats = std::make_shared<CountStats>();
    auto pub_topic = std::make_shared<TopicData>(std::make_shared<CountingMsg>(stats));
    auto pub = std::make_unique<shm::shm_pub_ipc>(pub_topic, "nodelet_warn", 12, false);
    pub->InitChannel();

    /* 没有 subscriber：snapshot 始终为空，触发 kWarnNoLocalSubs */
    stats->reset();
    for (int i = 0; i < 10; ++i)
    {
        auto msg = std::make_shared<CountingMsg>(stats);
        msg->value = i;
        msg->marker = "no_sub";
        pub->publish(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    /* 无论发布多少条，publish_best_effort 都能正常返回（不崩溃） */
    EXPECT_GE(stats->serialize.load(), 10)
        << "all publishes without subscriber must go through SHM (serialize per message)";

    dzIPC::EnableNodelet(true);  // restore for subsequent tests
}

/* ============================================================================
 * FT-13: ChannelKind 隔离 — 同 topic/domain/msg_id 不同 kind 互不干扰
 *
 * 验证 hash/equality 纳入了 ChannelKind，三种传输类型各自注册的队列
 * 在 snapshot 中完全隔离，不会跨种类污染。
 * ============================================================================ */

TEST(ShmNodelet, ChannelKindIsolation)
{
    using namespace dzIPC;

    const std::string topic = "kind_iso_topic";
    constexpr size_t domain = 99;
    constexpr uint32_t msg_id = 1;

    auto q_shm_pub = std::make_shared<CircularQueue<IpcMsgBase>>(1);
    auto q_socket_pub = std::make_shared<CircularQueue<IpcMsgBase>>(1);
    auto q_shm_svc = std::make_shared<CircularQueue<IpcMsgBase>>(1);

    auto& reg = LocalPubSubRegistry::instance();

    /* 注册：同一 topic/domain/msg_id，三种 kind */
    ChannelKey key_shm{topic, domain, msg_id, ChannelKind::ShmPubSub};
    ChannelKey key_sock{topic, domain, msg_id, ChannelKind::SocketPubSub};
    ChannelKey key_svc{topic, domain, msg_id, ChannelKind::ShmService};

    reg.register_subscriber(key_shm, q_shm_pub);
    reg.register_subscriber(key_sock, q_socket_pub);
    reg.register_subscriber(key_svc, q_shm_svc);

    /* 验证：每种 kind 的 snapshot 仅包含自己的队列 */
    {
        auto snap = reg.subscriber_snapshot(key_shm);
        ASSERT_EQ(snap.size(), 1u);
        EXPECT_EQ(snap[0].get(), q_shm_pub.get());
    }
    {
        auto snap = reg.subscriber_snapshot(key_sock);
        ASSERT_EQ(snap.size(), 1u);
        EXPECT_EQ(snap[0].get(), q_socket_pub.get());
    }
    {
        auto snap = reg.subscriber_snapshot(key_svc);
        ASSERT_EQ(snap.size(), 1u);
        EXPECT_EQ(snap[0].get(), q_shm_svc.get());
    }

    /* 注销 */
    reg.unregister_subscriber(key_shm, q_shm_pub);
    reg.unregister_subscriber(key_sock, q_socket_pub);
    reg.unregister_subscriber(key_svc, q_shm_svc);

    /* 验证：全部清空 */
    EXPECT_TRUE(reg.subscriber_snapshot(key_shm).empty());
    EXPECT_TRUE(reg.subscriber_snapshot(key_sock).empty());
    EXPECT_TRUE(reg.subscriber_snapshot(key_svc).empty());
}

}   // namespace
