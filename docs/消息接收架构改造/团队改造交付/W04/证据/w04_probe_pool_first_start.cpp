/* 探针 5：进程级池"首次初始化责任方"并发争抢 —— 两个线程用不同 worker_count
 * 同时 start()，检查是否**恰好一个**生效、另一个必须 false，且 worker_count 等于胜者。
 * 有界；不挂死。 */
#include <atomic>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include "dzIPC/threepools/recv_worker.h"
using namespace dzIPC::threepools;

int main() {
    RecvWorkerPool& pool = RecvWorkerPool::instance();
    std::atomic<int> go{0};
    bool r1 = false, r2 = false;
    std::thread t1([&] { while (!go.load()) std::this_thread::yield(); r1 = pool.start(3); });
    std::thread t2([&] { while (!go.load()) std::this_thread::yield(); r2 = pool.start(9); });
    go.store(1);
    t1.join(); t2.join();
    std::printf("PROBE5: start(3)=%d start(9)=%d worker_count=%zu running=%d -> %s\n",
                (int)r1, (int)r2, pool.worker_count(), (int)pool.running(),
                ((r1 ^ r2) ? "EXACTLY ONE WINS" : "*** BOTH SAME (责任方不明) ***"));
    const void* a = (const void*)RecvWorkerPool::worker_for("x", 1, pool.worker_count());
    (void)a;
    std::fflush(nullptr);
    _exit(0);
}
