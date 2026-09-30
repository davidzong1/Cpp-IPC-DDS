/* 对照实验：**修复前**的库（只有 counters.h 表、无写入点）在同一负载下的读数。
 * 判据：修复前 chunk_exhausted == 0（而 stderr 已打 chunk pool exhausted），
 * 修复后同臂 > 0 —— 这就是 t31 反例的闭环。
 * 本文件与 undeclared_probe 同源，仅输出更少字段。 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
int main()
{
    dzIPC::EnableDzFlat(true);
    auto p = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 99);
    auto s = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 99);
    dzIPC::shm::shm_pub_ipc pub{p, "f1_legacy_arm", 0, false};
    dzIPC::shm::shm_sub_ipc sub{s, "f1_legacy_arm", 0, 4, false};
    pub.InitChannel(); sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    auto& reg = CounterRegistry::instance();
    const auto g = [&](CounterId id) { return (unsigned long long)reg.get(id); };
    std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
    int ok = 0, rej = 0;
    for (int i = 0; i < 120; ++i)
    { auto lo = pub.loan<dzIPC::Msg::StdStringFlat>(64 * 1024);
      if (!lo.valid()) { ++rej; continue; } held.push_back(std::move(lo)); ++ok; }
    std::printf("legacy_arm loaned=%d rejected=%d chunk_exhausted=%llu chunk_alloc_failed=%llu\n",
                ok, rej, g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed));
    std::fflush(stdout);
    held.clear();
    _exit(0);
}
