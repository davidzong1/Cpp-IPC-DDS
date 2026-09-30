/* t31 最小反例：池被明确耗尽（loan 被显式拒绝，stderr 打出 chunk pool exhausted），
 * 而 §13.2#3 要求的 CounterRegistry::chunk_exhausted 是否真的动？ */
#include <cstdio>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/common/topic_data.h"
#include "ipc_msg/std_msgs/std_string.hpp"
int main()
{
    dzIPC::EnableDzFlat(true);
    auto ptd=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93);
    auto std_td=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93);
    dzIPC::shm::shm_pub_ipc pub{ptd, "t31chunk", 0};
    dzIPC::shm::shm_sub_ipc sub{std_td, "t31chunk", 0, 4};
    pub.InitChannel(); sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto& reg = dzIPC::measure::CounterRegistry::instance();
    const auto g=[&](dzIPC::measure::CounterId id){ return (unsigned long long)reg.get(id); };
    std::printf("BEFORE registry: chunk_exhausted=%llu chunk_alloc_failed=%llu fallback_pool_exhausted=%llu\n",
                g(dzIPC::measure::CounterId::chunk_exhausted), g(dzIPC::measure::CounterId::chunk_alloc_failed),
                g(dzIPC::measure::CounterId::fallback_pool_exhausted));
    std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
    const std::uint32_t big = 64*1024;
    int ok=0, rej=0;
    for (int i=0;i<200;++i){ auto lo=pub.loan<dzIPC::Msg::StdStringFlat>(big); if(!lo.valid()){++rej;continue;} held.push_back(std::move(lo)); ++ok; }
    std::printf("loaned=%d rejected=%d (池容量 40/档)\n", ok, rej);
    std::printf("AFTER  registry: chunk_exhausted=%llu chunk_alloc_failed=%llu fallback_pool_exhausted=%llu\n",
                g(dzIPC::measure::CounterId::chunk_exhausted), g(dzIPC::measure::CounterId::chunk_alloc_failed),
                g(dzIPC::measure::CounterId::fallback_pool_exhausted));
    std::printf("VERDICT: 池已明确耗尽(rejected=%d) 而 CounterRegistry::chunk_exhausted=%llu\n",
                rej, g(dzIPC::measure::CounterId::chunk_exhausted));
    held.clear(); return 0;
}
