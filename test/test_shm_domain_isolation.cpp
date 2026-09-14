/* SHM 的 domain 隔离 (docs/shm_defect_fixes.md 第 1 条)
 *
 * 修复前: SHM 段名只由 topic 名派生, domain_id 只用于进程内 nodelet 的 ChannelKey 与
 * info_pool 的诊断记录 —— 于是两个进程用 domain 0 和 domain 7 发**同名 topic** 会共用
 * 同一块共享内存通道, 本该互不可见的两个域互相收到了对方的消息。
 *
 * 这类缺陷不以报错形式出现, 而是**多收**, 所以既有测试全都测不到它 —— 它们从不断言
 * "跨 domain 应当收不到"。本文件补上这条判据, 同时钉住"同 domain 仍互通"不被改坏。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 41;

dzIPC::Msg::StdImage make_image(std::uint8_t seed)
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "cam";
    img.width = 8;
    img.height = 4;
    img.step = 24;
    img.encoding = "rgb8";
    img.data.assign(64, seed);
    img.set_msg_id(kMsgId);
    return img;
}

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

/* 反复发布 budget 时间, 期间尝试收取; 返回是否收到过任何一条。 */
bool pump_any(dzIPC::shm::shm_pub_ipc& pub, dzIPC::shm::shm_sub_ipc& sub,
              std::shared_ptr<dzIPC::TopicData>& sink, const dzIPC::Msg::StdImage& src,
              std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        if (sub.try_get_clone(sink))
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

}   // namespace

/* 同一 topic 名 + 不同 domain ⇒ 必须互不可见。 */
TEST(ShmDomainIsolation, SameTopicDifferentDomainsDoNotSeeEachOther)
{
    const std::string topic = "domain_iso_same_name";

    auto pub_td = make_td();
    auto sub_td = make_td();
    /* 发布方在 domain 0, 订阅方在 domain 7 —— 同名 topic, 但应当各走各的段。 */
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 7, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto sink = make_td();
    const auto src = make_image(0x5A);
    EXPECT_FALSE(pump_any(pub, sub, sink, src, 1200ms))
        << "domain 0 的发布被 domain 7 的订阅者收到了 —— SHM 段名没有隔离 domain";
}

/* 同一 topic 名 + 同一 domain ⇒ 照常互通(不得因隔离改动而回归)。 */
TEST(ShmDomainIsolation, SameTopicSameDomainStillCommunicates)
{
    const std::string topic = "domain_iso_same_domain";

    auto pub_td = make_td();
    auto sub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 3};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 3, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto sink = make_td();
    const auto src = make_image(0x77);
    ASSERT_TRUE(pump_any(pub, sub, sink, src, 4000ms)) << "同 domain 同 topic 不通了";

    auto got = sink->topic()->msgcast<dzIPC::Msg::StdImage>();
    ASSERT_TRUE(got != nullptr);
    EXPECT_EQ(got->data, src.data);
}

/* 两个 domain 各自成对 ⇒ 各自通, 且互不串。 */
TEST(ShmDomainIsolation, TwoDomainsEachDeliverOwnPayload)
{
    const std::string topic = "domain_iso_pairs";

    auto a_pub_td = make_td(), a_sub_td = make_td();
    auto b_pub_td = make_td(), b_sub_td = make_td();
    dzIPC::shm::shm_pub_ipc a_pub{a_pub_td, topic, 11};
    dzIPC::shm::shm_sub_ipc a_sub{a_sub_td, topic, 11, 8};
    dzIPC::shm::shm_pub_ipc b_pub{b_pub_td, topic, 22};
    dzIPC::shm::shm_sub_ipc b_sub{b_sub_td, topic, 22, 8};
    a_pub.InitChannel();
    a_sub.InitChannel();
    b_pub.InitChannel();
    b_sub.InitChannel();
    std::this_thread::sleep_for(400ms);

    const auto a_src = make_image(0xA1);
    const auto b_src = make_image(0xB2);

    auto a_sink = make_td();
    ASSERT_TRUE(pump_any(a_pub, a_sub, a_sink, a_src, 4000ms)) << "domain 11 自身不通";
    auto a_got = a_sink->topic()->msgcast<dzIPC::Msg::StdImage>();
    ASSERT_TRUE(a_got != nullptr);
    EXPECT_EQ(a_got->data, a_src.data) << "domain 11 收到的不是自己的载荷";

    auto b_sink = make_td();
    ASSERT_TRUE(pump_any(b_pub, b_sub, b_sink, b_src, 4000ms)) << "domain 22 自身不通";
    auto b_got = b_sink->topic()->msgcast<dzIPC::Msg::StdImage>();
    ASSERT_TRUE(b_got != nullptr);
    EXPECT_EQ(b_got->data, b_src.data) << "domain 22 收到的不是自己的载荷";
}
