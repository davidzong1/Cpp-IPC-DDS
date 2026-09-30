/* ARM-E：`wait_token_invalid` 的 0→非 0 取证。
 *
 * 该 ID 的写入点是 `shm_pub_sub_ipc.cc` 的 `fallback_to_compat(kInvalidToken)`，触发条件 =
 * `RecvWorker::add_route` 对模块适配器返回 `RecvRegisterStatus::invalid_token`，即
 * `route_session_.current_route()->read_wait_token()` 无效 —— libipc 侧
 * `detail_impl::read_wait_token()` 要求 `conn_info_head::rd_waiter_` 打开成功
 * （`ipc.cpp:1498` → `waiter::read_wait_token()` 在 `state()==nullptr` 时返回无效令牌）。
 *
 * 本探针用**资源耗尽**（RLIMIT_NOFILE）让 `rd_waiter_.open()` 失败，从而让 route 虽
 * 有效但 token 无效；随后订阅端在控制面 Ready 后的第一次 tick 接入 worker ⇒
 * invalid_token ⇒ 模块显式回退兼容线程并记 `wait_token_invalid`。
 *
 * ⛔ 若本构造不可复现，探针**如实打印 NOT_REPRODUCED**，交付里登记为"有写入点但本机
 *    未取到同臂 0→非 0 证据"（不得用 0 冒充）。
 * 用法: armE_token_invalid <soft_nofile>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/resource.h>
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
constexpr int kMsgId = 97;
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

std::vector<int> held_fds;

int main(int argc, char** argv)
{
    const long soft = (argc > 1) ? std::atol(argv[1]) : 0;
    if (soft > 0)
    {
        struct rlimit rl{};
        ::getrlimit(RLIMIT_NOFILE, &rl);
        rl.rlim_cur = static_cast<rlim_t>(soft);
        if (::setrlimit(RLIMIT_NOFILE, &rl) != 0)
        {
            std::printf("SETRLIMIT_FAILED errno=%d (%s)\n", errno, std::strerror(errno));
        }
        struct rlimit chk{};
        ::getrlimit(RLIMIT_NOFILE, &chk);
        std::printf("rlimit_nofile soft=%llu hard=%llu\n", (unsigned long long)chk.rlim_cur,
                    (unsigned long long)chk.rlim_max);
        /* 先把 fd 表格占满到"仅剩 few"——libipc 的 conn_info_head::init() 会按
         * cc/wt/rd/acc 的顺序 acquire 共享句柄，占满是让它失败**而不**让进程崩的
         * 唯一途径（否则 shm_open 失败会先落到 route 建不出来 ⇒ kNoRoute）。 */
        if (argc > 2)
        {
            const int hold = std::atoi(argv[2]);
            for (int i = 0; i < hold; ++i)
            {
                const int fd = ::open("/dev/null", O_RDONLY);
                if (fd < 0) { std::printf("fd_exhausted_after=%d\n", i); break; }
                held_fds.push_back(fd);
            }
            std::printf("held_fds=%zu\n", held_fds.size());
        }
    }
    dump("BEFORE-ArmE");

    /* 建 1 个订阅+发布；不设 RLIMIT 时应 100% 走 worker 臂且 wait_token_invalid==0（对照）。 */
    const std::string topic = "f1_armE_token";
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    dump("AFTER-ArmE");
    const bool reproduced = g(CounterId::wait_token_invalid) != 0;
    std::printf("ARM_E_%s\n", reproduced ? "REPRODUCED" : "NOT_REPRODUCED");
    ::fflush(nullptr);
    _exit(reproduced ? 0 : 2);
}
