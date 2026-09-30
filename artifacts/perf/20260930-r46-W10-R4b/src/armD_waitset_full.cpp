/* ARM-D：`wait_set_full` / `wait_token_invalid` 的**同臂 0→非 0** 取证。
 *
 * wait_set_full 的确定性构造（W04 §7.1 已实测：127 可用 / 第 128 个 ⇒ wait_set_full）：
 *   · `DZIPC_SHM_RECV_WORKERS=1` ⇒ 全进程只有 1 个 worker ⇒ 全部 route 落同一分片；
 *   · 依次建 N 个**独立话题**订阅者，每个占 1 个 wait token；
 *   · 第 128 个的 `pool.add_route` 必返回 wait_set_full ⇒ 模块 `fallback_to_compat`
 *     走 `kWaitSetFull` 分支 ⇒ `wait_set_full` + `fallback_capacity_full` + `fallback_total`。
 *   ⛔ 与 `fallback_*` 的分工按 W09/t19 冻结口径：`wait_set_full` 是**独立类别**，
 *      `fallback_total` 是"真的回退了"的总数，两者**同时** +1 是设计（不是双计数）。
 *
 * wait_token_invalid：写入点在同一函数（`kInvalidToken` 分支），触发要求
 *   `RecvWorker::add_route` 对**模块自己的适配器**返回 invalid_token（即
 *   `RouteSession::current_route()->read_wait_token()` 无效）。本探针尝试两种构造并
 *   **如实报告**是否触发（⛔ 不伪造）。
 * 用法: armD_waitset_full <n_sub>
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

#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
namespace { constexpr int kMsgId = 96;
std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }
unsigned long long g(CounterId id)
{ return static_cast<unsigned long long>(CounterRegistry::instance().get(id)); }
void dump(const char* tag)
{
    std::printf("%s wait_set_full=%llu wait_token_invalid=%llu fallback_total=%llu "
                "fallback_capacity_full=%llu registration_failed=%llu registration_ok=%llu\n",
                tag, g(CounterId::wait_set_full), g(CounterId::wait_token_invalid), g(CounterId::fallback_total),
                g(CounterId::fallback_capacity_full), g(CounterId::registration_failed), g(CounterId::registration_ok));
    std::fflush(stdout);
}
}  // namespace

/* ARM-D2：尝试触发 `wait_token_invalid`（**如实报告成功/失败**）。
 *
 * 已知前提（本包实测）：libipc 的 `conn_info_head::init()` 一次性打开 cc/wt/rd_waiter_，
 * 而 `read_wait_token()` 走 `rd_waiter_`；正常运行时它恒定有效 ⇒ 该分支在常规负载下
 * **取不到非 0**。本臂按两种尽力构造尝试，失败即打印 NOT_REPRODUCED（⛔ 不用 0 冒充）：
 *   ① 只建订阅者、不建发布者 ⇒ route 永不存在（应落 kNoRoute，不是 kInvalidToken）；
 *   ② 建齐后**先断开发布者**（析构 pub）再让控制面重建 ⇒ 尝试让 route 释放后 token 失效。
 */
int armD2_token_invalid()
{
    dump("BEFORE-ArmD2");
    const std::string topic = "f1_armD2_token";
    auto sub_td = td();
    {
        auto pub_td = td();
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
        dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        /* sub 仍活着，pub 先走：迫使订阅端在"无发布者"下继续跑控制面 tick。 */
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    dump("AFTER-ArmD2");
    const bool repro = g(CounterId::wait_token_invalid) != 0;
    std::printf("ARM_D2_%s (registration_attempts=%llu registration_ok=%llu fallback_total=%llu)\n",
                repro ? "REPRODUCED" : "NOT_REPRODUCED", g(CounterId::registration_attempts),
                g(CounterId::registration_ok), g(CounterId::fallback_total));
    return 0;
}

int main(int argc, char** argv)
{
    const int n = (argc > 1) ? std::atoi(argv[1]) : 140;
    std::printf("armD n_sub=%d recv_workers_env=%s pid=%d\n", n,
                std::getenv("DZIPC_SHM_RECV_WORKERS") ? std::getenv("DZIPC_SHM_RECV_WORKERS") : "(unset)",
                (int)::getpid());
    dump("BEFORE-ArmD");

    /* ⛔ 必须**同时**建发布者：SHM 订阅端的 route 由控制面 Ready 之后的 generation
     * 重建产生，而控制面进入 Ready 需要发布端存在。只有订阅者时 route 永不出现
     * （registration_attempts 恒 0）—— 这一点由本探针的第一版实测抓到并记入交付。 */
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::string> pub_topics;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    pubs.reserve(static_cast<std::size_t>(n));
    subs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const std::string topic = "f1_armD_" + std::to_string(i);
        pub_topics.push_back(topic);
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), topic, 0, 8, false));
        subs.back()->InitChannel("d");
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), topic, 0, false));
        pubs.back()->InitChannel("d");
    }
    /* 控制面 Ready 之后的第一次 tick（10 ms）才触发 add_route；给足窗口。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    dump("AFTER-ArmD");
    if (argc > 2 && std::strcmp(argv[2], "d2") == 0) armD2_token_invalid();
    std::printf("ARM_D_DONE\n");
    ::fflush(nullptr);
    _exit(0);
}
