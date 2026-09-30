/* 关键前置：libipc.so 内部新增的 CounterRegistry 写入，能否被**测试二进制**里的
 * CounterRegistry::instance() 读到？
 *
 * 风险：CounterRegistry::instance() 是 header-only 的 inline 函数 + 函数局部 static。
 * 若 libipc.so 与该二进制各自持有**一份** static，则"库内写、库外读"必然恒为 0 ——
 * 那会让本次接线看起来成功、实际读数不动。必须实测，不能假定。
 *
 * 判据：对 libipc 内部确有写入的既有 ID（registration_*）做一次真实 shm pub/sub 注册，
 * 在**本二进制**里读；同时用一个只在本文档内自增的对照 ID 验证读取通道本身可用。 */
#include <cstdio>
#include <memory>
#include <thread>
#include <chrono>
#include <unistd.h>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/common/topic_data.h"
#include "ipc_msg/std_msgs/std_string.hpp"

int main()
{
    using namespace dzIPC::measure;
    auto& reg = CounterRegistry::instance();
    const auto g = [&](CounterId id) { return static_cast<unsigned long long>(reg.get(id)); };

    /* 对照：本二进制自己写一次，确认"读自己的写"可用。 */
    reg.inc(CounterId::publish_blocked, 1u);
    const unsigned long long self_write = g(CounterId::publish_blocked);

    auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 91);
    auto std_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 91);
    dzIPC::shm::shm_pub_ipc pub{ptd, "odr_probe", 0};
    dzIPC::shm::shm_sub_ipc sub{std_td, "odr_probe", 0, 8};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    /* registration_attempts/ok 的写入点在 libipc.so 的 shm_pub_sub_ipc.cc 里。 */
    const unsigned long long in_lib_attempts = g(CounterId::registration_attempts);
    const unsigned long long in_lib_ok = g(CounterId::registration_ok);

    std::printf("self_write_publish_blocked=%llu (期望 1 ⇒ 本二进制读自己的写可用)\n", self_write);
    std::printf("lib_written_registration_attempts=%llu registration_ok=%llu\n", in_lib_attempts, in_lib_ok);
    const bool unified = (in_lib_attempts > 0);
    std::printf("ODR_UNIFICATION verdict=%s  ⇒ %s\n", unified ? "UNIFIED" : "SPLIT",
                unified ? "库内写入可被库外读到（本次接线有效）"
                        : "⚠️ 库内写入读不到（两份 static）—— 必须先解决才能接线");
    ::fflush(nullptr);
    _exit(unified && self_write == 1 ? 0 : 1);
}
