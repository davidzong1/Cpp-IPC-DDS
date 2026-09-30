/* ARM-E2：用"**预占 RD_CONN waiter 段且尺寸不匹配**"让 libipc 的
 * `conn_info_head::init()` 里 `rd_waiter_.open()` 失败 —— 于是
 * `detail_impl::read_wait_token()` 返回**无效令牌**（waiter.h:173-176 的
 * `state()==nullptr` 分支）⇒ `RecvWorker::add_route` 返回 `invalid_token`
 * ⇒ 模块 `fallback_to_compat(kInvalidToken)` ⇒ 记 `wait_token_invalid`。
 *
 * 段名由实测得到（/dev/shm 列表）：
 *   __IPC_SHM__RD_CONN__dz_ipc_d<domain>_<topic>_topic_WAITER_STATE_
 * （`waiter::open` 依次开 _WAITER_COND_ / _WAITER_LOCK_ / _WAITER_STATE_，任一失败即 invalid）
 *
 * ⛔ 若仍不可复现，探针如实打印 NOT_REPRODUCED。
 * 用法: armE2_token_invalid <which:state|lock|cond>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
namespace {
constexpr int kMsgId = 101;
std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }
unsigned long long g(CounterId id)
{ return static_cast<unsigned long long>(CounterRegistry::instance().get(id)); }
void dump(const char* tag)
{
    std::printf("%s wait_token_invalid=%llu registration_invalid_token=%llu fallback_total=%llu "
                "registration_failed=%llu registration_attempts=%llu registration_ok=%llu\n",
                tag, g(CounterId::wait_token_invalid), g(CounterId::registration_invalid_token),
                g(CounterId::fallback_total), g(CounterId::registration_failed),
                g(CounterId::registration_attempts), g(CounterId::registration_ok));
    std::fflush(stdout);
}
}  // namespace

int main(int argc, char** argv)
{
    const char* which = (argc > 1) ? argv[1] : "state";
    const std::string topic = "f1_armE2_token";
    const std::string base = "__IPC_SHM__RD_CONN__dz_ipc_d0_" + topic + "_topic_WAITER_";
    const std::string victim = base + (std::strcmp(which, "lock") == 0 ? "LOCK_"
                                     : std::strcmp(which, "cond") == 0 ? "COND_"
                                                                       : "STATE_");
    /* 让 `waiter::open()` 的 acquire 失败：在 /dev/shm 下把该名**预建为目录**。
     * 为什么不用"尺寸不匹配"：tmpfs 的 mmap 长度大于对象尺寸**不会失败**（只会在越界页
     * 首次访问时 SIGBUS），故 ftruncate(1) 那条路测不出来（本包实测三次均
     * NOT_REPRODUCED，记入交付）。目录则让 shm_open 直接以 EISDIR/EACCES 失败。 */
    const std::string dev = "/dev/shm/" + victim;
    if (::mkdir(dev.c_str(), 0777) == 0)
        std::printf("preoccupied_as_directory %s\n", dev.c_str());
    else
        std::printf("mkdir failed %s errno=%d(%s)\n", dev.c_str(), errno, std::strerror(errno));

    dump("BEFORE-ArmE2");
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    dump("AFTER-ArmE2");
    const bool repro = g(CounterId::wait_token_invalid) != 0;
    std::printf("ARM_E2_%s\n", repro ? "REPRODUCED" : "NOT_REPRODUCED");
    ::fflush(nullptr);
    _exit(repro ? 0 : 2);
}
