/* 被套圈(lapped)接收方的安全性回归 —— docs/dzflat_known_issues.md 第 3/4/5 条
 *
 * 三条问题同根: 接收方无法察觉自己被套圈(游标落后写指针超过环长)。
 *
 *   ③ chunk 池耗尽 → 大消息退化成 64 字节分片 → 套圈重组拼出错乱缓冲 →
 *      check_id 偶然通过 → TLV deserialize 按垃圾长度走偏 → **段错误**;
 *   ④ 套圈时 cur 与 cur+256 映射同一环槽位 → 同一 storage_id 产生两个 buff_t →
 *      recycle_storage 走两次 → id_pool 空闲链表被重复入池接成自环, 丢掉其后全部 id;
 *   ⑤ pop() 先拷出数据、之后才清自己的 rc 位, 所以拷贝期间本格可被 force_push 覆写
 *      → 数据撕裂。
 *
 * ③ 会杀进程, 所以用 fork(): 子进程跑洪泛, 父进程只看它是否被信号打死。
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"

namespace {

using namespace std::chrono_literals;

constexpr int kChunkPoolSize = 32;
constexpr int kRingSlots = 256;

std::string uniq(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/lap/") + tag + "_" + std::to_string(n.fetch_add(1));
}

}   // namespace

/* ③ 内存安全: 发布方快发大消息而订阅方不取 → 不得让订阅进程崩掉。
 *
 * 机制: chunk 池(32 块/尺寸档位)耗尽后 send() 退化成 64 字节分片, 一条 0.88 MB 消息
 * 变成 14000+ 分片打进 256 槽的环, force_push 覆写 → 接收侧按 msg.id_ 索引的重组缓存
 * 拼出错乱缓冲 → deserialize 按缓冲内容算出的偏移越界读。
 *
 * 这不是 DZFlat 引入的 —— 在不含 DZFlat 的基线上同样复现(见 known_issues 第 3 条)。 */
TEST(LapSafety, FloodedSubscriberMustNotCrash)
{
    const std::string topic = uniq("flood");
    pid_t child = ::fork();
    ASSERT_GE(child, 0) << "fork 失败";
    if (child == 0)
    {
        /* 子进程: 发布方 + 一个故意不读的订阅方(订阅线程在库内部自己跑, 会 deserialize) */
        auto pub_td =
            std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 90);
        auto sub_td =
            std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 90);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0};
        dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 64};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(400ms);

        dzIPC::Msg::StdImage img;
        img.header.frame_id = "cam";
        img.header.stamp = 1.0;
        img.width = 640;
        img.height = 480;
        img.step = 1920;
        img.encoding = "rgb8";
        img.data.assign(640 * 480 * 3, 0x5A);

        /* 连发不取: 迫使池耗尽 → 分片 → 套圈 */
        for (int i = 0; i < 200; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdImage>(img);
            m->set_msg_id(90);
            pub.publish(m);
        }
        std::this_thread::sleep_for(1500ms);   /* 让订阅线程把错乱缓冲都啃一遍 */
        ::_exit(0);
    }

    int status = 0;
    ::waitpid(child, &status, 0);
    EXPECT_FALSE(WIFSIGNALED(status))
        << "订阅进程被信号 " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
        << " 打死 —— 套圈重组产出的错乱缓冲让 deserialize 越界读了";
    if (WIFEXITED(status))
    {
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
}

/* ④ 套圈接收方读空后, chunk 池不得"变小"。
 *
 * 判据用分片悬崖: 池子健康时一条大消息占 1 个槽位, 池子被自环吃掉后退化成
 * kPayload/64 个槽位。取探针条数 < 32 让健康分支结果确定。
 *
 * 触发路径: 让接收方被套圈(连发不取), 然后**读空**它 —— 读空时 cur 会走过同一环槽位
 * 两次, 同一 storage_id 因此产生两个 buff_t, recycle_storage 重复入池。 */
TEST(LapSafety, LappedDrainMustNotShrinkChunkPool)
{
    constexpr std::size_t kPayload = 8192;   /* 独立尺寸档位, 不与其他用例抢池子 */
    constexpr int kProbe = 20;
    static_assert(kProbe < kChunkPoolSize, "探针条数须小于池容量");

    const std::string name = "lap_pool_probe";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    std::vector<std::uint8_t> payload(kPayload, 0xC3);

    auto probe = [&](ipc::route& rx) {
        for (int i = 0; i < kProbe; ++i)
        {
            if (!tx.send(payload.data(), payload.size(), 0)) return -1;
        }
        int n = 0;
        for (;;)
        {
            ipc::buff_t b = rx.recv(60);
            if (b.empty()) break;
            ++n;
        }
        return n;
    };

    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    const int baseline = probe(rx);
    if (baseline != kProbe)
    {
        GTEST_SKIP() << "chunk 池非初始状态(基线 " << baseline << "/" << kProbe
                     << ")。清理 /dev/shm 下的 __IPC_SHM__CHUNK_INFO__* 后重跑";
    }

    /* 把接收方套圈: 连发不取, 远超环长 */
    for (int i = 0; i < kRingSlots * 3; ++i)
    {
        tx.send(payload.data(), payload.size(), 0);
    }
    /* 读空 —— 这一步在旧实现里会让同一 storage_id 二次入池 */
    for (;;)
    {
        ipc::buff_t b = rx.recv(60);
        if (b.empty()) break;
    }

    const int after = probe(rx);
    EXPECT_EQ(after, kProbe)
        << "套圈读空之后 chunk 池缩水(" << after << "/" << kProbe
        << ") —— 同一 storage_id 被重复入池, id_pool 空闲链表接成自环";
}

/* ④ 的直接形态: 套圈读空时, 同一条大消息的内容不得被投递两次。 */
TEST(LapSafety, LappedDrainMustNotDeliverDuplicates)
{
    constexpr std::size_t kPayload = 4096;
    const std::string name = "lap_dup";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    /* 每条消息首字节写自己的序号(mod 251), 便于识别重复 */
    std::vector<std::uint8_t> payload(kPayload, 0);
    constexpr int kMsgs = kRingSlots + 64;   /* 超过环长 → 必然套圈 */
    for (int i = 0; i < kMsgs; ++i)
    {
        payload[0] = static_cast<std::uint8_t>(i % 251);
        tx.send(payload.data(), payload.size(), 0);
    }

    std::vector<int> seen(251, 0);
    int total = 0;
    for (;;)
    {
        ipc::buff_t b = rx.recv(60);
        if (b.empty()) break;
        ++total;
        seen[*static_cast<const std::uint8_t*>(b.data())]++;
    }

    int dup = 0;
    for (int c : seen)
    {
        if (c > 1) dup += c - 1;
    }
    EXPECT_EQ(dup, 0) << "套圈读空投递了 " << dup << " 条重复内容(共收到 " << total
                      << " 条) —— cur 与 cur+256 映射同一环槽位被读了两次";
}
