/* R1/t31 独立反证：threads=34 是否真等于「32 worker + 1 调度器 + 1 主线程」。
 *
 * 判据不是"数一数"（静态 grep 不算数），而是**创建顺序 + 逐 tid CPU 归因**：
 *   编译期开关 ORDER_POOL_FIRST 决定先建池还是先取调度器单例；外部脚本按
 *   /proc/<pid>/task/<tid>/stat 的 starttime 排序，看空闲 CPU 落在**第几个**创建的线程上。
 *   ① 池先起(ORDER_POOL_FIRST=1)：主=1，worker=2..33，调度器=34 ⇒ 忙线程应为 #34
 *   ② 调度器先取(=0)：           主=1，调度器=2，worker=3..34 ⇒ 忙线程应为 #2
 * 两序都命中预期 ⇒ 34 = 1 主 + 1 调度器 + 32 worker 成立；否则该等式被推翻。
 *
 * 编译（⛔ 只读复算，产物落 build/）：
 *   g++ -std=c++17 -O2 -DNDEBUG -DORDER_POOL_FIRST=1 -I include -I src -I 3rdparty \
 *       -o build/t31/t31_threads_a <本文件> -L build/lib -lipc -lpthread \
 *       -Wl,-rpath,$PWD/build/lib
 *   g++ ... -DORDER_POOL_FIRST=0 -o build/t31/t31_threads_b ...
 * 运行 + 外部逐 tid 读（脚本见 verify_w10_threads.command.txt）：
 *   ./build/t31/t31_threads_a & PID=$!; sleep 12; python3 perthread3.py $PID 5
 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_string.hpp"

#ifndef ORDER_POOL_FIRST
#define ORDER_POOL_FIRST 1
#endif

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int n = 1000;
    const int dom = 23001;
#if ORDER_POOL_FIRST
    /* ① 池先起：主线程(1) → worker(2..33) → 调度器(34) */
    dzIPC::threepools::RecvWorkerPool::instance().start(32);
    (void)dzIPC::shm_control::ShmControlScheduler::instance();
    std::printf("ORDER: main(1), workers(2..33), scheduler(34)\n");
#else
    /* ② 调度器先取：主线程(1) → 调度器(2) → worker(3..34) */
    (void)dzIPC::shm_control::ShmControlScheduler::instance();
    dzIPC::threepools::RecvWorkerPool::instance().start(32);
    std::printf("ORDER: main(1), scheduler(2), workers(3..34)\n");
#endif
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93); };
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (int i = 0; i < n; ++i)
    {
        const std::string nm = "t31thr_" + std::to_string(i);
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), nm, dom, false));
        pubs.back()->InitChannel("t31thr");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), nm, dom, 64, false));
        subs.back()->InitChannel("t31thr");
    }
    std::printf("routes=%zu entries=%zu\n", dzIPC::threepools::RecvWorkerPool::instance().route_count(),
                dzIPC::shm_control::ShmControlScheduler::instance().entry_count());
    std::this_thread::sleep_for(std::chrono::seconds(40));
    return 0;
}
