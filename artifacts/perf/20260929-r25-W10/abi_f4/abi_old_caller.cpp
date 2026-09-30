/* F4 判决性复现：**陈旧调用方（旧头）× 新库**。
 *
 * ABI 面：`RecvWorkerPool::stats()` 按值返回 `RecvWorkerStats`；t30 在该结构体**尾部追加**
 * 了 5 个 `uint64`。C++ 的 by-value 返回由**调用方**分配返回槽（sret）并按**调用方看到的**
 * 结构体大小截取；若调用方用旧头（更小），库写入的尾部字节会落到返回槽之外 ⇒ 栈破坏，
 * 表现为 `__libc_free` 处崩溃（heap/栈指针被覆盖后 free 检查失败）。
 *
 * 本文件用【旧头】编译、链接【新库】，复现该形态。
 * ⛔ 这是 ABI 纪律演示，不是产品缺陷：正式证据必须「工装二进制指纹 ↔ 库指纹」成对引用。
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "dzIPC/threepools/recv_worker.h"   /* ← 由 -I 指向 tmp/t36/abi/oldinc（旧头） */

int main()
{
    std::printf("sizeof(RecvWorkerStats) [old header] = %zu\n",
                sizeof(dzIPC::threepools::RecvWorkerStats));
    /* 在返回槽附近放哨兵，检测越界写。 */
    volatile std::uint64_t guard[8];
    for (int i = 0; i < 8; ++i) guard[i] = 0x5A5A5A5A5A5A5A5AULL;
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const dzIPC::threepools::RecvWorkerStats s = pool.stats();   /* ← 按值返回（sret） */
    std::printf("stats.route_count=%zu wait_timeouts=%llu\n",
                static_cast<std::size_t>(s.route_count),
                static_cast<unsigned long long>(s.wait_timeouts));
    int clobbered = 0;
    for (int i = 0; i < 8; ++i) if (guard[i] != 0x5A5A5A5A5A5A5A5AULL) ++clobbered;
    std::printf("guard_clobbered=%d\n", clobbered);
    std::printf("ABI_OLD_CALLER_DONE\n");
    return 0;
}
