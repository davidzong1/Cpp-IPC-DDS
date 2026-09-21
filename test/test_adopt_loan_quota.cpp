/* UF-012 回归: adopt 借样配额(docs/adopt_loan_quota_fix.md)
 *
 * 被测不变量: **每订阅者最多借 ViewQueueCap()(= large_msg_cache/4 = 10, 对齐
 * ROS 2 默认 QoS depth)块 chunk** —— 借样进 msg_queue_ 的 schema-less DZFlat
 * 消息每条钉一块池 chunk, 队列深度是用户配置的 queue_size(可远大于池), 不设
 * 配额一个慢消费者就能把整档池钉干(UF-012)。配额满即物化拷贝(kDzFlatAdoptSpilled
 * 计数), 降级是每消息一次拷贝, 不是系统级池饿死。
 *
 * 四条判据, 各自打在计数的一条腿上:
 *   QuotaCapsBorrowAndSpillsMaterialize —— 配额封顶: 前 10 条借样、其后物化,
 *       spilled 恰好 = N - 10; 内容逐字节一致(借样与物化同源)。顺带验证
 *       queue_size > 钉上限时的构造期警告(用户拍板的补充项)。
 *   DrainRestoresZeroCopy              —— pop 递减: 配额打满后排空, 下一条必须
 *       回到借样。pop 递减缺失 ⇒ 计数停在 10 ⇒ 本条必红。
 *   EvictionKeepsQuotaAccurate         —— 满队驱逐递减: queue_size=4 灌 12 条,
 *       驱逐回调若缺失, 计数只涨不跌 ⇒ 第 11/12 条会物化 ⇒ spilled==0 必红。
 *   SteadyStateStaysZeroCopy           —— 常态不受扰: 消费跟得上时 spilled 恒 0。
 *
 * 尺寸档: StdImage 60×45 / 61×45(rgb8) ⇒ dzflat 段 ~8.2KB ⇒ chunk 类 10240,
 * 避开其他用例占用的 3072/4096/5120/7168/9216/13312。两种宽度落同一档、段长不同,
 * 交替发布 ⇒ 收到的段长必须交替 —— 这是"没有串档"的内容判据。
 */
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/def.h"

namespace {

using namespace std::chrono_literals;

/* 开关 RAII(同 test_dzflat_transport): 用例间不互相污染。 */
class DzFlatSwitch
{
public:
    explicit DzFlatSwitch(bool on) : prev_(dzIPC::IsDzFlatEnabled())
    {
        dzIPC::EnableDzFlat(on);
        dzIPC::ResetDzFlatCounters();
    }
    ~DzFlatSwitch() { dzIPC::EnableDzFlat(prev_); }

private:
    bool prev_;
};

std::string unique_topic(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/adopt_quota/") + tag + "_" + std::to_string(n.fetch_add(1));
}

dzIPC::Msg::StdImage make_image(std::size_t w, std::size_t h, std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "adopt_quota_cam";
    img.header.stamp = 7.5;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.step = static_cast<std::uint32_t>(w * 3);
    img.encoding = "rgb8";
    img.data.resize(w * h * 3);
    for (std::size_t i = 0; i < img.data.size(); ++i)
        img.data[i] = static_cast<std::uint8_t>((i * 7 + seed) & 0xFF);
    return img;
}

/* GenericMessage 订阅者(话题模板无 schema ⇒ DZFlat 段必走 adopt 分支)。 */
struct SubOnGeneric
{
    std::shared_ptr<dzIPC::TopicData> td;
    std::unique_ptr<dzIPC::shm::shm_sub_ipc> sub;

    SubOnGeneric(const std::string& topic, std::uint32_t msg_id, std::size_t queue_size)
        : td(std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(),
                                                msg_id)),
          sub(new dzIPC::shm::shm_sub_ipc(td, topic, 0, queue_size))
    {
        sub->InitChannel();
    }
};

/* 反复发布直到订阅侧收到第一条(= SHM 握手完成), 并把途中的积压**排干**。
 *
 * ⛔ "收到一条"只证明握手已通 —— 握手期多发的 warm 消息还躺在队列里, 不排干
 *    会混进后续判据的计数(实测把 total 抬成 24/20)。排干判据: 连续两轮
 *    (间隔 30ms)排空 —— 单轮排空不夠, 订阅线程可能还在把环里的余量搬进队列。 */
void warm_up(dzIPC::shm::shm_pub_ipc& pub, dzIPC::shm::shm_sub_ipc& sub,
             const dzIPC::Msg::StdImage& warm, std::uint32_t msg_id,
             std::shared_ptr<dzIPC::TopicData>& sink)
{
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    bool got_first = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(warm);
        m->set_msg_id(msg_id);
        pub.publish(m);
        if (sub.try_get_clone(sink))
        {
            got_first = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    if (!got_first)
    {
        FAIL() << "4s 内没收到第一条(warm-up) —— SHM 握手未完成, 后续判据全部失效";
        return;
    }
    bool settled = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(30ms);
        bool any = false;
        while (sub.try_get_clone(sink))
        {
            any = true;
        }
        if (any)
        {
            settled = false;
        }
        else if (settled)
        {
            return;
        }
        else
        {
            settled = true;
        }
    }
    FAIL() << "warm-up 积压排不干 —— 订阅线程停摆";
}

/* 排干 msg_queue_, 按到达顺序收集 (是否借样, 段长, 段字节)。 */
struct RecvMsg
{
    bool borrowed = false;
    std::size_t seg_len = 0;
    std::vector<std::uint8_t> bytes;
};

std::vector<RecvMsg> drain(dzIPC::shm::shm_sub_ipc& sub,
                           std::shared_ptr<dzIPC::TopicData>& sink)
{
    std::vector<RecvMsg> out;
    while (sub.try_get_clone(sink))
    {
        auto* gm = dynamic_cast<dzIPC::GenericMessage*>(sink->topic().get());
        if (gm == nullptr)
        {
            ADD_FAILURE() << "schema-less 话题收到的必须是 GenericMessage";
            out.clear();
            return out;
        }
        if (!gm->has_dzflat())
        {
            ADD_FAILURE() << "收到一条不含 DZFlat 段的消息 —— wire 判别失效";
            continue;
        }
        RecvMsg r;
        r.borrowed = gm->dzflat_is_borrowed();
        r.seg_len = gm->dzflat_len();
        const auto* p = gm->dzflat_data();
        if (p == nullptr)
        {
            ADD_FAILURE() << "DZFlat 段基址为空";
            continue;
        }
        r.bytes.assign(p, p + r.seg_len);
        out.push_back(std::move(r));
    }
    return out;
}

/* 补充项①: queue_size > 钉上限 ⇒ 构造期打印警告(捕获 stderr 验证)。 */
TEST(AdoptLoanQuota, WarnsWhenQueueExceedsPinCap)
{
    /* 补充项③: 钉上限对齐 ROS 2 默认 depth —— 视图/adopt 共用 ViewQueueCap()。 */
    EXPECT_EQ(dzIPC::ViewQueueCap(), 10u)
        << "钉上限应为 large_msg_cache(40)/4 = 10(对齐 ROS 2 默认 QoS depth)";
    EXPECT_EQ(static_cast<std::size_t>(ipc::large_msg_cache), 40u);

    DzFlatSwitch on{true};
    const std::string topic = unique_topic("warn");
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), 90);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    std::ostringstream captured;
    auto* old_err = std::cerr.rdbuf(captured.rdbuf());
    {
        SubOnGeneric sub(topic, 90, 64);   // 64 > 10 ⇒ 必须警告
    }
    std::cerr.rdbuf(old_err);

    const std::string text = captured.str();
    EXPECT_NE(text.find("queue_size = 64"), std::string::npos)
        << "queue_size 超过钉上限必须警告, 实际输出:\n"
        << text;
    EXPECT_NE(text.find("adopt 借样"), std::string::npos)
        << "警告必须同时说明 adopt 借样配额面, 实际输出:\n"
        << text;
}

/* 判据一: 配额封顶 —— 前 10 条借样、后 2 条物化, spilled == N - cap; 内容逐字节一致。 */
TEST(AdoptLoanQuota, QuotaCapsBorrowAndSpillsMaterialize)
{
    DzFlatSwitch on{true};
    dzIPC::ResetDzFlatRxCounters();

    const std::string topic = unique_topic("cap");
    constexpr std::uint32_t kMsgId = 91;
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    /* queue_size = 64 ≫ 钉上限 10: 慢消费者场景的原型。 */
    SubOnGeneric sub(topic, kMsgId, 64);

    const dzIPC::Msg::StdImage warm = make_image(60, 45, 0x01);
    /* sink 用订阅者自己的 td(既定习语, 同 test_dzflat_transport): try_get_clone
     * 会把出队消息 update 进它, 空指针会解引用段错误。 */
    std::shared_ptr<dzIPC::TopicData>& sink = sub.td;
    warm_up(pub, *sub.sub, warm, kMsgId, sink);

    /* 交替发布两种宽度(同档不同长)各 6 条, 共 12; 期间**完全不消费**。 */
    constexpr int kN = 12;
    for (int i = 0; i < kN; ++i)
    {
        const auto src = make_image(i % 2 == 0 ? 60 : 61, 45, 0x10);
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }

    /* ⛔ 等订阅线程把环里的 12 条**全部吞完**才许 pop: kDzFlatAccepted 在订阅线程
     * "接受"时自增(先于 adopt/spill 决策), 连续两读稳定 ⇒ 环空。并发排干会提前
     * 释放配额, 把本该物化的尾部消息变成借样 —— 第一轮跑出的真竞态。 */
    std::uint64_t last_acc = 0;
    int stable = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (stable < 2 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(100ms);
        const auto acc = dzIPC::DzFlatRxCounters().dzflat_accepted;
        if (acc == last_acc)
        {
            ++stable;
        }
        else
        {
            stable = 0;
            last_acc = acc;
        }
    }
    ASSERT_GE(last_acc, static_cast<std::uint64_t>(kN))
        << "接受的段数少于发布数 —— 订阅线程没吞完";
    std::this_thread::sleep_for(100ms);   /* 余量: accept 与 adopt/spill 决策间的微秒级间隔 */

    std::vector<RecvMsg> got = drain(*sub.sub, sink);
    ASSERT_EQ(got.size(), static_cast<std::size_t>(kN)) << "12 条必须全部送达";

    /* 配额分割: 前 10 借样、后 2 物化(FIFO ⇒ 顺序即发布序)。 */
    const std::size_t cap = dzIPC::ViewQueueCap();
    for (std::size_t i = 0; i < got.size(); ++i)
    {
        EXPECT_EQ(got[i].borrowed, i < cap)
            << "第 " << i << " 条的借样态与配额分割不符";
    }
    EXPECT_EQ(dzIPC::DzFlatRxCounters().dzflat_adopt_spilled, kN - cap)
        << "spilled 计数必须恰为 N - 配额";

    /* 段长交替(60/61 两种宽度 ⇒ 两种段长), 证明没有串档。 */
    ASSERT_NE(got[0].seg_len, got[1].seg_len) << "两种宽度的段长不应相同";
    for (std::size_t i = 2; i < got.size(); ++i)
    {
        EXPECT_EQ(got[i].seg_len, got[i % 2].seg_len)
            << "第 " << i << " 条段长与同宽度首条不一致 —— 串档";
    }
    /* 内容逐字节: 同宽度首末两条(借样 vs 物化)同源, 必须一致。 */
    EXPECT_EQ(got[0].bytes, got[10].bytes) << "借样与物化的同源内容不一致";
    EXPECT_EQ(got[1].bytes, got[11].bytes) << "借样与物化的同源内容不一致";

    EXPECT_GT(dzIPC::DzFlatPublishCount(), 0u) << "全程没走 DZFlat —— 测的是空气";
}

/* 判据二: pop 递减 —— 配额打满后排空, 下一条必须回到借样。 */
TEST(AdoptLoanQuota, DrainRestoresZeroCopy)
{
    DzFlatSwitch on{true};
    dzIPC::ResetDzFlatRxCounters();

    const std::string topic = unique_topic("drain");
    constexpr std::uint32_t kMsgId = 92;
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    SubOnGeneric sub(topic, kMsgId, 64);
    const dzIPC::Msg::StdImage warm = make_image(60, 45, 0x02);
    std::shared_ptr<dzIPC::TopicData>& sink = sub.td;   /* 习语同上 */
    warm_up(pub, *sub.sub, warm, kMsgId, sink);

    /* 打满配额: 恰好发布 cap 条且不消费。 */
    const std::size_t cap = dzIPC::ViewQueueCap();
    for (std::size_t i = 0; i < cap; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(60, 45, 0x20));
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }
    std::vector<RecvMsg> got;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (got.size() < cap && std::chrono::steady_clock::now() < deadline)
    {
        auto more = drain(*sub.sub, sink);
        got.insert(got.end(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
        if (got.size() < cap)
        {
            std::this_thread::sleep_for(5ms);
        }
    }
    ASSERT_EQ(got.size(), cap);
    for (const auto& r : got)
    {
        EXPECT_TRUE(r.borrowed) << "配额内的消息必须全部借样";
    }

    /* 排空后配额必须归零(pop 递减): 再发一条必须仍是借样。
     * pop 递减缺失 ⇒ 计数停在 cap ⇒ 这一条会被物化 ⇒ 红。 */
    auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(60, 45, 0x21));
    m->set_msg_id(kMsgId);
    pub.publish(m);

    bool borrowed_again = false;
    const auto deadline2 = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline2)
    {
        auto more = drain(*sub.sub, sink);
        for (const auto& r : more)
        {
            borrowed_again = r.borrowed;
        }
        if (borrowed_again)
        {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(borrowed_again) << "排空后新消息应回到借样 —— pop 递减失效则此处物化";
    EXPECT_EQ(dzIPC::DzFlatRxCounters().dzflat_adopt_spilled, 0u)
        << "本用例全程不应有物化";
}

/* 判据三: 满队驱逐递减 —— queue_size=4 灌 12 条, 驱逐回调缺失 ⇒ 后两条物化。 */
TEST(AdoptLoanQuota, EvictionKeepsQuotaAccurate)
{
    DzFlatSwitch on{true};
    dzIPC::ResetDzFlatRxCounters();

    const std::string topic = unique_topic("evict");
    constexpr std::uint32_t kMsgId = 93;
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    SubOnGeneric sub(topic, kMsgId, 4);   // 深度 4 < 配额 10
    const dzIPC::Msg::StdImage warm = make_image(60, 45, 0x03);
    std::shared_ptr<dzIPC::TopicData>& sink = sub.td;   /* 习语同上 */
    warm_up(pub, *sub.sub, warm, kMsgId, sink);

    /* 灌 12 条不消费: 每条 adopt 入队都挤掉最老一条 ⇒ 计数在 3..4 振荡,
     * 永远到不了配额 ⇒ 全部借样、零物化。驱逐回调缺失 ⇒ 计数涨到 10 ⇒
     * 第 11/12 条物化 ⇒ spilled==0 必红。 */
    for (int i = 0; i < 12; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(60, 45, 0x30));
        m->set_msg_id(kMsgId);
        pub.publish(m);
    }
    std::this_thread::sleep_for(800ms);   // 等订阅线程吞完环里 12 条并完成驱逐

    auto got = drain(*sub.sub, sink);
    EXPECT_LE(got.size(), 4u);
    EXPECT_EQ(dzIPC::DzFlatRxCounters().dzflat_adopt_spilled, 0u)
        << "深 4 的队列靠驱逐自限, 不应触碰物化 —— 驱逐递减失效则此处出现溢出";

    /* 驱逐后计数归零: 排空再发一条, 必须仍是借样。 */
    auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(60, 45, 0x31));
    m->set_msg_id(kMsgId);
    pub.publish(m);
    bool borrowed = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto more = drain(*sub.sub, sink);
        for (const auto& r : more)
        {
            borrowed = r.borrowed;
        }
        if (borrowed)
        {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(borrowed) << "驱逐归还后配额应为 0 —— 计数漂移则此处物化";
}

/* 判据四: 常态不受扰 —— 消费跟得上时零拷贝恒生效。 */
TEST(AdoptLoanQuota, SteadyStateStaysZeroCopy)
{
    DzFlatSwitch on{true};
    dzIPC::ResetDzFlatRxCounters();

    const std::string topic = unique_topic("steady");
    constexpr std::uint32_t kMsgId = 94;
    auto pub_td = std::make_shared<dzIPC::TopicData>(
        std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    SubOnGeneric sub(topic, kMsgId, 64);
    const dzIPC::Msg::StdImage warm = make_image(60, 45, 0x04);
    std::shared_ptr<dzIPC::TopicData>& sink = sub.td;   /* 习语同上 */
    warm_up(pub, *sub.sub, warm, kMsgId, sink);

    std::size_t borrowed_n = 0, total = 0;
    for (int i = 0; i < 20; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(make_image(60, 45, 0x40));
        m->set_msg_id(kMsgId);
        pub.publish(m);
        std::this_thread::sleep_for(15ms);   // 消费节奏跟得上 ⇒ 队列不积压
        for (const auto& r : drain(*sub.sub, sink))
        {
            ++total;
            if (r.borrowed)
            {
                ++borrowed_n;
            }
        }
    }
    EXPECT_EQ(total, 20u);
    EXPECT_EQ(borrowed_n, 20u) << "常态下每条都应借样(零拷贝)";
    EXPECT_EQ(dzIPC::DzFlatRxCounters().dzflat_adopt_spilled, 0u);
}

}   // namespace
