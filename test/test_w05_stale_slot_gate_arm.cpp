/* t84：`peer_count()==0` + 陈旧 `in_use` 槽位 —— **双臂**常驻判据（默认臂 + L1 回退臂）
 *
 * ── 为什么必须双臂（N1 的教训）────────────────────────────────────────────
 * W05 有**两条驱动源**：
 *   ① 进程级 `ShmControlScheduler`（默认臂）；
 *   ② 每话题 `compat_control_loop()`（`DZIPC_SHM_CONTROL_SCHEDULER=1` 的 **L1 运行时回退**）。
 * t77 只把「何时扫 stale」的判据修在①里；②自带一份**修复前**的同样判据 ⇒ 回退臂
 * **比基线更差**（400 发 0 收 vs 400/400）。**根因：同一判据被复制到两条驱动路径。**
 * t84 把判据收敛成**唯一的内联自由函数** `pub_control_tick()` ⇒ 结构上不可能再分叉。
 *
 * 本用例因此**按臂各跑一次**：
 *   · 臂 ①：`DZIPC_SHM_CONTROL_SCHEDULER` 未设  ⇒ 走调度器；
 *   · 臂 ②：`DZIPC_SHM_CONTROL_SCHEDULER=1`     ⇒ 走每话题兼容线程（= L1 回退）。
 * 环境变量是**进程内只读一次**的静态量 ⇒ 两臂必须在**两个子进程**里跑（fork），
 * 且子进程用 `_exit`（不跑 atexit / 不析构全局），避免父进程状态污染。
 *
 * ── 判据（每臂两条，都必须成立）──────────────────────────────────────────
 *   ① **回收**：`peer_count()==0` 期间，陈旧槽位必须在 `peer_dead_timeout` 量级内被清掉；
 *   ② **活订阅者不被误断**：随后挂真实订阅者 + 发布 N 条，必须全收到。
 *   （② 是①的反向闸门：把回收"提前"到错误方向会让陈旧 cc_id 在活订阅者复用后被
 *     `disconnect_receivers()` 摘掉 ⇒ ② 变红。）
 *
 * ── 反向验证（"判据的判据"）────────────────────────────────────────────
 * 把任一条驱动源的判据改回「`has_peers()` 为假就跳过」⇒ **该臂**必须变红。
 * 实测见 `artifacts/perf/20261001-t84-W05-F1/negative/`（默认臂与回退臂各一份消融）。
 *
 * ── 段名纪律 ────────────────────────────────────────────────────────────
 * topic 名不含 '/'；子进程用 `_exit` 但会在退出前清段 + 清控制面段。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "libipc/ipc.h"
#include "libipc/shm.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 93;
constexpr int kMsgs = 100;

/* 私有段探针：按段名 open 控制面段读 PeerSlot（⛔ 不新建、不 unlink）。 */
struct RawControl
{
    ipc::shm::handle h;
    dzIPC::control_plane_shm::TopicControl* c{nullptr};

    bool open(const std::string& name)
    {
        if (!h.acquire(name.c_str(), sizeof(dzIPC::control_plane_shm::TopicControl), ipc::shm::open))
        {
            return false;
        }
        c = static_cast<dzIPC::control_plane_shm::TopicControl*>(h.get());
        return c != nullptr && c->magic.load() == dzIPC::control_plane_shm::kTopicControlMagic;
    }
    std::uint32_t count() const { return c->peer_count.load(std::memory_order_acquire); }
    std::uint32_t in_use(int i) const { return c->peers[i].in_use.load(std::memory_order_acquire); }
};

bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

void clear_segs(const std::string& topic)
{
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
}

/* 单臂回流：返回 0 = 本臂通过；非 0 = 失败码（1 未回收 / 2 误断 / 3 前提不成立）。
 * 在**子进程**里跑（环境变量是进程内只读一次的静态量）。 */
int run_one_arm(const std::string& topic)
{
    const std::string seg = shm_topic_control_name(topic, 0);
    auto pub_td = make_td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, /*verbose=*/false};
    pub.InitChannel();

    /* ⚠️ 必须在 InitChannel **之后**造状态：InitChannel 会 begin_rebuild()
     *    （→ clear_all_peer_slots），把先造的状态清掉。 */
    int slot = -1;
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (!cp.open(seg)) { return 3; }
        const std::uint32_t gen = cp.generation();
        if (gen == 0 || !cp.add_peer(gen)) { return 3; }
        slot = cp.acquire_peer_slot(gen, /*cc_id=*/1u);
        if (slot < 0) { return 3; }
        cp.remove_peer(gen);                 /* 只摘 peer_count，**故意不** release 槽位 */
    }
    {
        RawControl r;
        if (!r.open(seg) || r.count() != 0u || r.in_use(slot) != 1u) { return 3; }
    }

    /* ★ 判据①：peer_count()==0 期间仍须回收（周期 = peer_dead_timeout = 2s 量级）。 */
    const bool reaped = wait_for([&]
                                 {
                                     RawControl r;
                                     return r.open(seg) && r.in_use(slot) == 0u;
                                 },
                                 6000);
    if (!reaped) { return 1; }

    /* ★ 判据②：活订阅者不得被误断（发布 kMsgs 条须全收到）。 */
    auto sub_td = make_td();
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, /*queue_size=*/8, /*verbose=*/false};
    sub.InitChannel();
    if (!wait_for([&]
                  {
                      RawControl r;
                      return r.open(seg) && r.count() >= 1u;
                  },
                  3000))
    {
        return 3;
    }

    int rx = 0;
    {
        for (int i = 0; i < kMsgs; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "gate-" + std::to_string(i);
            if (!pub.publish(m)) { break; }
            const auto deadline = Clock::now() + 200ms;
            while (Clock::now() < deadline)
            {
                auto sink = make_td();
                if (sub.try_get_clone(sink)) { ++rx; break; }
                std::this_thread::sleep_for(1ms);
            }
        }
    }
    /* 子进程直接 `_exit`，不做 RAII 清段 —— ⛔ 段清理交给父进程用例的 RAII（见下），
     * 因为这里显式析构会让栈对象二次析构。 */
    if (rx != kMsgs) { return 2; }
    return 0;
}

/* 在子进程里跑一臂：返回 0/失败码；-1 表示子进程异常（信号/退出码不可解释）。 */
int run_arm_in_child(bool compat_arm, const char* tag)
{
    static std::atomic<int> seq{0};
    const std::string topic = std::string("w05gate_arm_") + tag + "_" + std::to_string(seq.fetch_add(1));

    const ::pid_t pid = ::fork();
    if (pid < 0) { return -1; }
    if (pid == 0)
    {
        if (compat_arm) { ::setenv("DZIPC_SHM_CONTROL_SCHEDULER", "1", 1); }
        else            { ::unsetenv("DZIPC_SHM_CONTROL_SCHEDULER"); }
        const int rc = run_one_arm(topic);
        ::_exit(rc);
    }
    int status = 0;
    int rc = -1;
    for (int i = 0; i < 200; ++i)   // 20 s 硬超时
    {
        if (::waitpid(pid, &status, WNOHANG) == pid)
        {
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    if (rc == -1 && ::waitpid(pid, &status, WNOHANG) != pid)
    {
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
    }
    /* ⛔ 子进程用 `_exit` 不跑 RAII ⇒ **父进程**负责清段（数据段 + 控制面段）。
     * 清在子进程退出**之后**：此刻已无活映射者。 */
    clear_segs(topic);
    return rc;
}

}   // namespace

/* 默认臂（进程级调度器）：陈旧槽位必须回收 + 活订阅者不被误断。 */
TEST(W05StaleSlotGateArm, DefaultArmReapsAndDoesNotDisconnectLiveSubscriber)
{
    const int rc = run_arm_in_child(/*compat_arm=*/false, "sched");
    EXPECT_EQ(rc, 0) << "默认臂失败码 " << rc
                     << "（1=陈旧槽位未回收；2=活订阅者被误断；3=用例前提不成立；-1=子进程异常）"
                        " ⇒ 进程级调度器臂必须满足判据①②";
}

/* L1 运行时回退臂（DZIPC_SHM_CONTROL_SCHEDULER=1，每话题兼容控制线程）：
 * ⛔ 这是 t78/N1 暴露的那条路径 —— 回退臂**不得比基线更差**。 */
TEST(W05StaleSlotGateArm, CompatArmReapsAndDoesNotDisconnectLiveSubscriber)
{
    const int rc = run_arm_in_child(/*compat_arm=*/true, "compat");
    EXPECT_EQ(rc, 0) << "L1 回退臂失败码 " << rc
                     << "（1=陈旧槽位未回收；2=活订阅者被误断；3=用例前提不成立；-1=子进程异常）"
                        " ⇒ ⛔ L1 是文档 §4.2 的**回滚入口**，回退臂必须与默认臂同判据；"
                        "「回退比基线更差」= 回滚路径不安全（t78/N1 的形态）";
}
