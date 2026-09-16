/* 接收方连接位耗尽时不得进入"假成功"态 (docs/shm_defect_fixes.md 第 2 条)
 *
 * libipc 的接收方连接位图是 cc_t = uint32_t —— 单 topic 最多 32 个接收方, 位满时
 * connect() 返回 0。修复前订阅端拿到 cc_id == 0 之后**照样宣布握手完成**, 于是第 33 个
 * 订阅者进入假成功态: InitChannel 不报错、日志正常, 但一条消息都收不到; 而发布端只用
 * recv_count() 判有无接收者, 两端都看不见这个截断。
 *
 * 控制面 PeerSlot 表是 64 槽而连接位只有 32 个, 这个 2× 差额正是黑洞的容量。
 *
 * 本文件用**行为**做判据(handshake_completed 是私有的): 前 32 个必须收得到, 而"总收到
 * 数"不得超过位宽上限 —— 修复前第 33 个会静默地既连不上又被当成已连接。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 52;
constexpr std::size_t kCap = 32;   /* = sizeof(circ::cc_t) * 8 */

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

dzIPC::Msg::StdImage make_image()
{
    dzIPC::Msg::StdImage img;
    img.header.frame_id = "cap";
    img.width = 4;
    img.height = 2;
    img.step = 12;
    img.encoding = "rgb8";
    img.data.assign(24, 0x3B);
    img.set_msg_id(kMsgId);
    return img;
}

}   // namespace

/* 起 kCap + 1 个订阅者。前 kCap 个应当都能收到; 第 33 个连不上 —— 关键是它不得被当成
 * "已连接"(修复前它就是), 表现为总送达数不会超过 kCap。 */
TEST(ShmReceiverCap, ThirtyThirdSubscriberDoesNotSilentlyPretendToBeConnected)
{
    const std::string topic = "recv_cap_probe";

    auto pub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::shared_ptr<dzIPC::TopicData>> sinks;
    subs.reserve(kCap + 1);
    sinks.reserve(kCap + 1);
    for (std::size_t i = 0; i < kCap + 1; ++i)
    {
        sinks.push_back(make_td());
        subs.push_back(std::make_unique<dzIPC::shm::shm_sub_ipc>(make_td(), topic, 0, 8));
        subs.back()->InitChannel();
    }
    std::this_thread::sleep_for(1500ms);   /* 让握手都走完(含第 33 个的重试循环) */

    const auto src = make_image();
    /* 连发一阵, 给每个订阅者取的机会。 */
    std::vector<bool> got(kCap + 1, false);
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        for (std::size_t i = 0; i < subs.size(); ++i)
        {
            if (subs[i]->try_get_clone(sinks[i]))
            {
                got[i] = true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }

    std::size_t delivered = 0;
    for (bool g : got)
    {
        if (g) ++delivered;
    }

    /* 上限: 连接位只有 kCap 个, 不可能有更多订阅者真的连上。 */
    EXPECT_LE(delivered, kCap) << "收到消息的订阅者数 " << delivered << " 超过了连接位上限 "
                               << kCap << " —— 不可能, 说明计数或连接语义出问题了";
    /* 下限: 容量之内的订阅者必须真的能用 —— 不得因为这次修复把正常路径弄坏。 */
    EXPECT_GE(delivered, kCap - 2) << "只有 " << delivered
                                   << " 个订阅者收到消息, 远低于容量 " << kCap
                                   << " —— 容量内的订阅者被误伤了";

    /* ★ 这条才是本修复的判据(上面两条对修复前后**没有区分力** —— 第 33 个无论修没修
     * 都收不到消息)。
     *
     * 真正的差别在可观测性: 连不上的订阅者**不得留在控制面登记表里冒充在线**。
     * 修复前它 acquire_peer_slot 失败(cc_id==0 → -1)却仍 add_peer 过且不撤, 于是
     * peer_count 会计到 kCap + 1; 修复后它 remove_peer 退掉登记并重试, peer_count
     * 收敛到不超过连接位上限。
     *
     * 控制面段名与数据段名同源(shm_topic_control_name = 数据段名 + "_control2"),
     * 且该名字现在由产品侧导出 —— 这里转调而不是再复刻一遍后缀。 */
    dzIPC::control_plane_shm::TopicControlPlane cp;
    ASSERT_TRUE(cp.open(shm_topic_control_name(topic, 0)))
        << "打不开控制面段, 无法验证本条判据";
    const uint32_t peers = cp.peer_count();
    EXPECT_LE(peers, static_cast<uint32_t>(kCap))
        << "控制面登记了 " << peers << " 个 peer, 超过连接位上限 " << kCap
        << " —— 连不上的订阅者仍在冒充在线(这正是修复前的假成功态)";
}

/* 容量之内(远少于 32)照常全通 —— 防止上面的拒绝逻辑误伤正常场景。 */
TEST(ShmReceiverCap, SubscribersWellWithinCapAllReceive)
{
    const std::string topic = "recv_cap_small";
    constexpr std::size_t kN = 4;

    auto pub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
    pub.InitChannel();

    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::shared_ptr<dzIPC::TopicData>> sinks;
    for (std::size_t i = 0; i < kN; ++i)
    {
        sinks.push_back(make_td());
        subs.push_back(std::make_unique<dzIPC::shm::shm_sub_ipc>(make_td(), topic, 0, 8));
        subs.back()->InitChannel();
    }
    std::this_thread::sleep_for(600ms);

    const auto src = make_image();
    std::vector<bool> got(kN, false);
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>(src);
        m->set_msg_id(kMsgId);
        pub.publish(m);
        bool all = true;
        for (std::size_t i = 0; i < subs.size(); ++i)
        {
            if (subs[i]->try_get_clone(sinks[i])) got[i] = true;
            all = all && got[i];
        }
        if (all) break;
        std::this_thread::sleep_for(5ms);
    }

    for (std::size_t i = 0; i < kN; ++i)
    {
        EXPECT_TRUE(got[i]) << "容量内的第 " << i << " 个订阅者没收到";
    }
}
