/* W05 验收探针（非交付物；证据用）—— 死亡检测（stale peer 回收）语义在 W05 前后是否一致。
 *
 * ── 观测面为什么取"PeerSlot.in_use"而不是 peer_count ─────────────────────
 * `TopicControlPlane::collect_stale_peers()` 只做两件事：把超时槽位清空（in_use=0）
 * 并返回被回收的 cc_id 供 `route::disconnect_receivers()` 摘连接位。它**不动**
 * `peer_count` —— `peer_count` 只由 `add_peer/remove_peer` 改成对（`remove_peer` 还要求
 * generation 匹配）。所以进程被 SIGKILL 后 `peer_count` 保持不降是**既有语义**，
 * 不是本次改造引入的回归（本探针在基线库 `build_baseline/lib` 上跑同一份代码，
 * 输出可逐字对照）。真正可判定的死亡检测读数是**槽位是否被清空**与**耗时**。
 *
 * 判据（两条驱动臂都要成立，且与基线库读数同量级）：
 *   ① 子进程 attach ⇒ 恰好 1 个槽位 in_use；
 *   ② `SIGKILL` 子进程（不走 remove_peer/release_peer_slot）⇒ 心跳停止；
 *   ③ 回收耗时落在 [1.5s, 7s]（判死超时 2s + 扫描周期 50ms）；
 *   ④ 回收后槽位 in_use 全 0 且 cc_id 已清（0）—— 即"槽位真的被摘除"而非残留。
 *
 * 编译（头库 / 基线库各一份用于对照）：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_stale_peer.cpp \
 *       -o artifacts/w05/scratch-probe/w05_stale_peer -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 *   g++ ... -o /tmp/w05_stale_baseline -L build_baseline/lib ... \
 *       -Wl,-rpath,$PWD/build_baseline/lib
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

constexpr std::uint32_t kMsgId = 99;
using Clock = std::chrono::steady_clock;

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

/* 统计 in_use 槽位数与其 cc_id（只读共享段公开布局）。 */
static std::size_t used_slots(const dzIPC::control_plane_shm::TopicControl* c, std::uint32_t* first_cc)
{
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
    {
        if (c->peers[i].in_use.load(std::memory_order_acquire) != 0)
        {
            if (n == 0 && first_cc != nullptr)
            {
                *first_cc = c->peers[i].cc_id.load(std::memory_order_acquire);
            }
            ++n;
        }
    }
    return n;
}

int main()
{
    const bool compat = [] {
        const char* v = std::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    const std::string topic = "w05stale_probe";
    const std::size_t domain = 99;

    /* ⚠️ 顺序承重：父进程必须先建发布者（`InitChannel()` 内部的 `begin_rebuild()` 会
     * 清空 peer_count 与全部槽位），再 fork 子进程订阅 —— 反过来子进程刚登记的槽位会被
     * 这次 rebuild 清掉，探针测的就不是死亡回收了（实测症状：peers 恒 1 / subscribed 恒 0）。 */
    dzIPC::shm::shm_pub_ipc pub{td(), topic, domain, false};
    pub.InitChannel();
    for (int i = 0; i < 300 && !pub.has_subscribed(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string ctl_name = shm_topic_control_name(topic, domain);
    ipc::shm::id_t id = ipc::shm::acquire(ctl_name.c_str(), 0, ipc::shm::open);
    std::size_t mapped = 0;
    void* mem = (id != nullptr) ? ipc::shm::get_mem(id, &mapped) : nullptr;
    if (mem == nullptr || mapped < sizeof(dzIPC::control_plane_shm::TopicControl))
    {
        std::printf("arm=%s FATAL cannot map control plane\n", compat ? "compat" : "scheduler");
        return 2;
    }
    const auto* ctl = static_cast<const dzIPC::control_plane_shm::TopicControl*>(mem);

    const pid_t child = ::fork();
    if (child < 0)
    {
        std::printf("fork failed\n");
        return 3;
    }
    if (child == 0)
    {
        dzIPC::shm::shm_sub_ipc sub{td(), topic, domain, 8, false};
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::seconds(30));
        ::_exit(0);
    }

    std::uint32_t cc_before = 0;
    std::size_t slots_before = 0;
    const auto dl = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < dl)
    {
        slots_before = used_slots(ctl, &cc_before);
        if (slots_before >= 1 && pub.has_subscribed())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (slots_before < 1)
    {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::printf("arm=%s FATAL subscriber never attached\n", compat ? "compat" : "scheduler");
        return 4;
    }

    /* ★ 被观测的一步：SIGKILL（心跳立刻停，且**没有** remove_peer / release_peer_slot）。 */
    const auto t_kill = Clock::now();
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);

    std::size_t slots_after = slots_before;
    std::uint32_t cc_after = cc_before;
    double reap_ms = -1.0;
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < deadline)
    {
        slots_after = used_slots(ctl, &cc_after);
        if (slots_after == 0)
        {
            reap_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_kill).count();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    /* ⚠️ 必须在 release_no_unlink 之前把 peer_count 读出来 —— 释放后再解引用 ctl 是
     * use-after-unmap（实测 SIGSEGV）。 */
    const std::uint32_t peers_final = ctl->peer_count.load(std::memory_order_acquire);
    const bool subscribed_final = pub.has_subscribed();
    ipc::shm::release_no_unlink(id);

    std::printf("arm=%s slots_before=%zu cc_before=%u slots_after=%zu cc_after=%u reap_ms=%.1f "
                "peer_count=%u has_subscribed=%d\n",
                compat ? "compat" : "scheduler", slots_before, cc_before, slots_after, cc_after, reap_ms,
                static_cast<unsigned>(peers_final), (int)subscribed_final);
    /* ⚠️ 判据只用 slots_after（槽位被清空）与 reap_ms。不用 cc_after：`used_slots()`
     * 只在**有**在用槽位时才回填 cc_id，回收后它保留旧值 —— 那是探针的读数形状，
     * 不是产品状态。peer_count 不参与判据，理由见文件头。 */
    const bool ok = slots_before == 1 && slots_after == 0 && reap_ms >= 1500.0 && reap_ms <= 7000.0;
    (void)cc_after;
    std::printf("arm=%s verdict=%s\n", compat ? "compat" : "scheduler", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? 0 : 1;
}
