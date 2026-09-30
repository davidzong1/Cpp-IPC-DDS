/* C5 取证探针: N 路订阅+发布反复建/删, 检查
 *   · entry_count 每次回到 0（无条目泄漏）
 *   · callback_exception_count 恒 0（无回调泄漏/异常隔离）
 *   · tick_deferred_count 记录（同步注销被延迟的次数）
 *   · worker 在全部销毁后仍 active（进程级常驻）
 * 用法: w05_churn_probe <N> <rounds> */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_image.hpp"

int main(int argc, char** argv)
{
    const int n = std::atoi(argc > 1 ? argv[1] : "100");
    const int rounds = std::atoi(argc > 2 ? argv[2] : "3");
    const int kMsgId = 56;
    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    std::printf("churn_probe n=%d rounds=%d\n", n, rounds);

    for (int r = 0; r < rounds; ++r)
    {
        {
            std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
            std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
            for (int i = 0; i < n; ++i)
            {
                const std::string topic = "/w05c/r" + std::to_string(r) + "_t" + std::to_string(i);
                auto st = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
                subs.push_back(std::make_unique<dzIPC::shm::shm_sub_ipc>(st, topic, 0, 8));
                subs.back()->InitChannel("w05churn");
                auto pt = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
                pubs.push_back(std::make_unique<dzIPC::shm::shm_pub_ipc>(pt, topic, 0, false));
                pubs.back()->InitChannel("w05churn");
            }
            const auto mid = sched.stats();
            std::printf("round=%d phase=live entry_count=%zu callback_exception=%llu tick_deferred=%llu\n",
                        r, sched.entry_count(), (unsigned long long)mid.callback_exception_count,
                        (unsigned long long)mid.tick_deferred_count);
            std::fflush(stdout);
        }   /* 全部析构: 每项必须同步注销 */
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto after = sched.stats();
        std::printf("round=%d phase=after_dtor entry_count=%zu callback_exception=%llu tick_deferred=%llu worker_active=%d\n",
                    r, sched.entry_count(), (unsigned long long)after.callback_exception_count,
                    (unsigned long long)after.tick_deferred_count, sched.worker_active() ? 1 : 0);
        std::fflush(stdout);
    }
    std::printf("CHURN_DONE\n");
    ::fflush(nullptr);
    _exit(0);
}
