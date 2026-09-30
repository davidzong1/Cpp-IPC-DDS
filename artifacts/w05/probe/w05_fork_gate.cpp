/* W05 验收探针（非交付物；证据用）—— fork 出来的子进程必须仍能完成订阅控制面握手。
 *
 * 背景（本探针要证明的失败模式）：`ShmControlScheduler::instance()` 是**故意泄漏的进程级
 * 单例**，它的 worker 线程在 `fork()` 后**不存在**（子进程只继承调用线程）。若父进程已经
 * 用过 SHM pub/sub（单例已建、`worker_active()` 仍读到 true），子进程再建订阅者时就会把
 * 控制面动作注册进一个**没有线程**的调度器 ⇒ 子进程的订阅者永远不 attach。
 *
 * 判据：父进程建 pub 并**先**用一次 SHM pub/sub（让单例在父进程里建好），再 fork；
 *       子进程建订阅者并在 3s 内收到父进程发布的消息 ⇒ PASS。
 *
 * 编译：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_fork_gate.cpp \
 *       -o artifacts/w05/scratch-probe/w05_fork_gate -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

constexpr std::uint32_t kMsgId = 97;
using Clock = std::chrono::steady_clock;

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

int main()
{
    const std::string topic = "w05fork_probe";
    const std::size_t domain = 98;

    /* ① 父进程先用一次 SHM pub/sub：这一步把 ShmControlScheduler 单例（含 worker 线程）建起来。 */
    {
        dzIPC::shm::shm_sub_ipc warm_sub{td(), topic, domain, 8, false};
        warm_sub.InitChannel();
        dzIPC::shm::shm_pub_ipc warm_pub{td(), topic, domain, false};
        warm_pub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    /* ② fork：子进程只继承调用线程，调度器 worker **不在**。 */
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        std::printf("fork failed\n");
        return 1;
    }
    if (pid == 0)
    {
        int got = 0;
        {
            dzIPC::shm::shm_sub_ipc sub{td(), topic, domain, 8, false};
            sub.InitChannel();
            auto sink = td();
            const auto dl = Clock::now() + std::chrono::seconds(3);
            while (Clock::now() < dl && got == 0)
            {
                if (sub.try_get_clone(sink))
                {
                    ++got;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::printf("CHILD got=%d\n", got);
        std::fflush(stdout);
        ::_exit(got > 0 ? 0 : 7);
    }

    /* ③ 父进程持续发布，让子进程有机会收到。 */
    {
        dzIPC::shm::shm_pub_ipc pub{td(), topic, domain, false};
        pub.InitChannel();
        const auto dl = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < dl)
        {
            auto img = std::make_shared<dzIPC::Msg::StdImage>();
            img->set_msg_id(kMsgId);
            img->width = 2;
            img->height = 2;
            img->step = 6;
            img->encoding = "rgb8";
            img->data.assign(12, 0x5A);
            pub.publish(img);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    int st = 0;
    ::waitpid(pid, &st, 0);
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    std::printf("PARENT child_exit=%d verdict=%s\n", code, (code == 0) ? "PASS" : "FAIL");
    std::fflush(stdout);
    return code == 0 ? 0 : 1;
}
