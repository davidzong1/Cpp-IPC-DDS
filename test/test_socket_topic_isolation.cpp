/* 同机不同 topic 的 UDP 组播不得互相串扰 (docs/shm_defect_fixes.md 第 3 条)
 *
 * 缺陷形态: 端口公式是 domain_id * hash(topic), 默认 domain_id=0 时第二项恒为 0, 端口对
 * **所有 topic** 都退化成常数 11451; 接收端又绑 INADDR_ANY, 于是同机所有 topic 的 socket
 * 挤在同一端口互相收包。实测(修复前): 只往 A 发 593 条, **B 收到了全部 593 条**。
 *
 * 为什么既有 socket 测试没抓到: 它们每个 topic 用**不同的 msg_id**, 收到对方的包也会被
 * check_id 挡掉 —— 串扰真实存在但被掩盖。而 msg_id 的默认值是 0, 生产里大量 topic 并不设它。
 * 所以本文件刻意让两个 topic **都用默认 msg_id = 0**, 这才是缺陷的真实形态。
 *
 * 修复: 绑组播**组地址**而非 INADDR_ANY, 由内核按组过滤。寻址方案不变。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "dzIPC/dzipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

namespace {

using namespace std::chrono_literals;

/* 两个 topic 都用默认 msg_id = 0 —— 这是缺陷的真实形态, 见文件头。 */
constexpr std::uint32_t kMsgId = 0;

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

struct Counts
{
    std::atomic<int> a{0};
    std::atomic<int> b{0};
};

}   // namespace

TEST(SocketTopicIsolation, SameDomainDifferentTopicsDoNotCrossTalk)
{
    const std::string ta = "iso_udp_topic_A";
    const std::string tb = "iso_udp_topic_B";

    auto pub_a_td = make_td();
    auto sub_a_td = make_td();
    auto sub_b_td = make_td();
    /* 只给 A 建发布者; B 一条都不发。 */
    auto pub_a = dzIPC::PublisherIPCPtrMake(pub_a_td, ta, 0, dzIPC::IPC_SOCKET, false);
    auto sub_a = dzIPC::SubscriberIPCPtrMake(sub_a_td, ta, 0, 32, dzIPC::IPC_SOCKET, false);
    auto sub_b = dzIPC::SubscriberIPCPtrMake(sub_b_td, tb, 0, 32, dzIPC::IPC_SOCKET, false);
    pub_a->InitChannel("iso");
    sub_a->InitChannel("iso");
    sub_b->InitChannel("iso");
    std::this_thread::sleep_for(1500ms);

    Counts counts;
    std::atomic<bool> run{true};
    std::thread ra([&] {
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::StdString>(kMsgId);
        while (run.load(std::memory_order_acquire))
        {
            if (sub_a->try_get_clone(rcv)) counts.a.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
    });
    std::thread rb([&] {
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::StdString>(kMsgId);
        while (run.load(std::memory_order_acquire))
        {
            if (sub_b->try_get_clone(rcv)) counts.b.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
    });

    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "payload_of_A";
    msg->set_msg_id(kMsgId);
    int sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        pub_a->publish(msg);
        ++sent;
        std::this_thread::sleep_for(4ms);
    }
    run.store(false, std::memory_order_release);
    ra.join();
    rb.join();

    const int a = counts.a.load();
    const int b = counts.b.load();

    /* 正向: A 自己必须收得到 —— 否则"B 收不到"可能只是因为整条链路不通(假绿)。 */
    EXPECT_GT(a, 0) << "topic A 自己一条都没收到(发了 " << sent << " 条) —— 链路不通, "
                    << "本用例对串扰没有判据力";
    /* 反向: B 必须一条都收不到。修复前这里是 593/593 全收。 */
    EXPECT_EQ(b, 0) << "topic B 收到了 " << b << " 条本不属于它的数据 —— 跨 topic 串扰";
}
