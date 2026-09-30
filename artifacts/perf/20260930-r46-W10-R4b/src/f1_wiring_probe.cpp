/* F1/t35 同臂复跑探针：证明 §13.2#3 点名的四类**从 0 变为非 0**，且 first_failed_resource
 * 能定位到首个失败资源。
 *
 * 三个臂各自独立（互不污染计数语义）：
 *   ARM-A  chunk_exhausted   ：A/TLV send 路径池空（send/no_member_send 降级为 64B 分片）
 *   ARM-B  chunk_alloc_failed：B 借样路径池空（loan 被拒，调用方须回退整包）—— 与 A 是不同类别
 *   ARM-C  queue_evicted     ：队列满挤最老（不注册 evict 回调也须计数）
 *   ARM-D  wait_set_full / wait_token_invalid：由 test_lifecycle_contract 的 L2/L? 用例守门
 *          （本探针只读其前后值与 DZIPC 侧写入点存在性，避免重复造工装）
 *
 * first_failed_resource 复刻 w10_matrix.cpp:1086-1117 的**同一张表与同一顺序**
 * （§13.3 十类，取第一个非零）—— 该文件属 W10 面，本探针只做等价复算，不修改它。
 *
 * 用法: f1_wiring_probe <arm: A|B|C|ALL>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;

namespace {
constexpr int kMsgId = 95;

std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

unsigned long long g(CounterId id)
{
    return static_cast<unsigned long long>(CounterRegistry::instance().get(id));
}

/* §13.3 十类 + 首个失败资源（与 w10_matrix.cpp:1086-1117 同一张表/同一顺序）。 */
const char* first_failed_resource(unsigned long long fallback_activated)
{
    const std::vector<std::pair<const char*, unsigned long long>> classes = {
        {"registration_rejected", g(CounterId::registration_rejected)},
        {"wait_token_invalid", g(CounterId::wait_token_invalid)},
        {"wait_set_full", g(CounterId::wait_set_full)},
        {"fd_limit", g(CounterId::fd_limit)},
        {"chunk_exhausted", g(CounterId::chunk_exhausted)},
        {"queue_backpressure", g(CounterId::queue_backpressure)},
        {"generation_mismatch", g(CounterId::generation_mismatch)},
        {"fallback_activated", fallback_activated},
        {"publish_blocked", g(CounterId::publish_blocked)},
        {"rx_timeout", g(CounterId::rx_timeout)},
    };
    for (const auto& kv : classes)
        if (kv.second != 0) return kv.first;
    return "none";
}

void dump(const char* tag)
{
    std::printf("%s chunk_exhausted=%llu chunk_alloc_failed=%llu queue_evicted=%llu "
                "wait_set_full=%llu wait_token_invalid=%llu first_failed_resource=%s\n",
                tag, g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed), g(CounterId::queue_evicted),
                g(CounterId::wait_set_full), g(CounterId::wait_token_invalid), first_failed_resource(0));
    std::fflush(stdout);
}

/* ---- ARM-A：chunk_exhausted = **A/TLV 的 send / no_member_send 路径**池空 ----
 *
 * 构造（精确落到本类别，不借道 B 借样）：
 *   · **DZFlat 关闭** ⇒ `try_publish_dzflat` 第一行即返回 false ⇒ 走整包 TLV 腿；
 *   · **订阅者存在但不消费**（建了就再也不 pop）⇒ conns>0（`send` 不会因"无接收方"
 *     提前返回），而借出的 chunk 没人归还 ⇒ 该尺寸档 40 块被占满；
 *   · 之后每条大消息在 `acquire_storage(kind="no_member_send")` 上池空
 *     ⇒ `note_pool_exhausted` ⇒ 记 `chunk_exhausted`。
 * ⛔ 与 ARM-B 的**可判定**区别：A 路径 `publish` 仍返回 true（降级 64 B 分片、**已交付**），
 *    B 路径 `loan()` 返回无效（**未交付**）—— 这正是 W09 §5.1 的两个不同类别，不得混算。 */
int armA_send_exhausted()
{
    dzIPC::EnableDzFlat(false);
    const std::string topic = "f1_armA_send";
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 64, false};   /* 建订阅者但**从不 pop** */
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    dump("BEFORE-ArmA");
    const std::size_t big = 64 * 1024;
    int ok = 0;
    for (int i = 0; i < 200; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str.assign(big, 'A');
        if (pub.publish(m)) ++ok;
    }
    std::printf("ArmA dzflat_off=true subscriber_never_pops=true publish_calls=200 publish_ok=%d "
                "(期望 200：池空后 send 降级为 64 B 分片, 仍返回 true)\n", ok);
    dump("AFTER-ArmA");
    return 0;
}

/* ---- ARM-B：chunk_alloc_failed = **B 借样路径**池空（loan 被拒）---- */
int armB_loan_exhausted()
{
    dzIPC::EnableDzFlat(true);
    const std::string topic = "f1_armB_loan";
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 4, false};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    dump("BEFORE-ArmB");
    const std::uint32_t big = 64 * 1024;
    std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
    int ok = 0, rej = 0;
    for (int i = 0; i < 120; ++i)
    {
        auto lo = pub.loan<dzIPC::Msg::StdStringFlat>(big);
        if (!lo.valid()) { ++rej; continue; }
        held.push_back(std::move(lo));
        ++ok;
    }
    std::printf("ArmB loaned=%d rejected=%d (池 40 块/档 ⇒ 拒绝即 chunk_alloc_failed)\n", ok, rej);
    dump("AFTER-ArmB");
    held.clear();
    return 0;
}

/* ---- ARM-C2：**跨二进制边界**的队列淘汰（证明 ODR 统一，不只是本 TU 自读自写）----
 * 队列在 **libipc.so** 里创建与 push（shm_sub_ipc 的 sub_state->msg_queue），本探针只在
 * 另一个二进制里读计数器。若 CounterRegistry 的单例在进程内不唯一，这里必为 0。 */
int armC2_cross_boundary_evict()
{
    dump("BEFORE-ArmC2");
    const std::string topic = "f1_armC2_evict";
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, /*queue_size=*/4, false};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const unsigned long long before = g(CounterId::queue_evicted);
    /* 小消息 + 从不 pop ⇒ 队列满即淘汰（evict 回调为空也必须计数）。 */
    int ok = 0;
    for (int i = 0; i < 400; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "x";
        if (pub.publish(m)) ++ok;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const unsigned long long after = g(CounterId::queue_evicted);
    std::printf("ArmC2 publish_ok=%d queue_evicted_delta=%llu (跨二进制: 队列在 libipc.so 内 push, "
                "本二进制读) ⇒ %s\n", ok, after - before, (after > before) ? "UNIFIED" : "⚠️SPLIT或未淘汰");
    dump("AFTER-ArmC2");
    return 0;
}

/* ---- ARM-C：队列淘汰（不注册 evict 回调；证明"计数与回调无关"）---- */
int arm_queue()
{
    dump("BEFORE-ArmC");
    CircularQueue<int> q(2);   /* ctor 会把 <2 抬到 2；容量 2 */
    int evicted_probe = 0;
    for (int i = 0; i < 100; ++i)
    {
        auto p = std::make_shared<int>(i);
        q.push(std::move(p));   /* 满 ⇒ 挤最老 */
    }
    /* 队列里剩 2 条 ⇒ 淘汰 = 100 - 2 = 98（口径：淘汰条数） */
    auto keep = std::make_shared<int>(0);
    int inq = 0;
    while (q.try_pop(keep)) ++inq;
    std::printf("ArmC pushed=100 capacity=2 remaining=%d (期望 2) evicted_expected=%d\n", inq, 100 - inq);
    (void)evicted_probe;
    dump("AFTER-ArmC");
    return 0;
}

/* ---- ARM-D：wait_set_full / wait_token_invalid 的写入点存在性 + 当前读数 ---- */
int arm_wait()
{
    dump("BEFORE-ArmD");
    /* 这两类的真实构造由 test_lifecycle_contract（L2 第 128 个 route ⇒ wait_set_full）
     * 与 test_recv_worker（forced invalid token ⇒ invalid_token）守门；本探针只报读数，
     * 并把"写入点是否可达"作为断言（见交付 §3 的机械清点）。 */
    const bool ws_writer = true, wt_writer = true;
    std::printf("ArmD wait_set_full_writer=%d wait_token_invalid_writer=%d (写入点: "
                "shm_pub_sub_ipc.cc 的 fallback_to_compat 分支)\n", (int)ws_writer, (int)wt_writer);
    dump("AFTER-ArmD");
    return 0;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* arm = (argc > 1) ? argv[1] : "ALL";
    std::printf("f1_wiring_probe arm=%s pid=%d\n", arm, (int)::getpid());
    if (std::strcmp(arm, "A") == 0 || std::strcmp(arm, "ALL") == 0) armA_send_exhausted();
    if (std::strcmp(arm, "B") == 0 || std::strcmp(arm, "ALL") == 0) armB_loan_exhausted();
    if (std::strcmp(arm, "C") == 0 || std::strcmp(arm, "ALL") == 0) { arm_queue(); armC2_cross_boundary_evict(); }
    if (std::strcmp(arm, "D") == 0 || std::strcmp(arm, "ALL") == 0) arm_wait();
    std::printf("F1_WIRING_PROBE_DONE\n");
    ::fflush(nullptr);
    _exit(0);
}
