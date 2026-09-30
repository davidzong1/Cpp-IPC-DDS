/* F4 复现②：让栈越界写覆盖到**堆指针**，从而在 free() 处崩溃（对齐 R1-F4 记录的
 * `__GI___libc_free ← main()` 栈形）。 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "dzIPC/threepools/recv_worker.h"   /* 旧头（-I tmp/t36/abi/oldinc） */

int main()
{
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    /* 反复按值取 stats()：每次都会越界写一段返回槽之后的栈，堆指针一旦被覆盖，
     * 随后的 free() 即检测到堆结构损坏并 abort/SIGSEGV。 */
    for (int i = 0; i < 64; ++i)
    {
        const dzIPC::threepools::RecvWorkerStats s = pool.stats();
        if (i == 0) std::printf("sizeof(old)=%zu route_count=%zu\n",
                                sizeof(dzIPC::threepools::RecvWorkerStats),
                                static_cast<std::size_t>(s.route_count));
    }
    char* p = static_cast<char*>(std::malloc(32));
    std::printf("malloc=%p\n", static_cast<void*>(p));
    std::strcpy(p, "heap payload");
    std::free(p);
    std::printf("ABI_CRASH_DEMO_DONE\n");
    return 0;
}
