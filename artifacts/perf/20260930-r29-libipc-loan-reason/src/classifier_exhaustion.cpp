/* t46：证明 `borrow_failed_oversized` 在**当前唯一落点**
 * （`note_dzflat_borrow_failed`）的输入空间里**结构上不可达**。
 *
 * 方法：把该函数的三个 bool 输入穷举 2^3 = 8 个组合，**每个组合一个全新进程**
 * （注册表是函数局部 static、进程级，故每进程天然干净，不需 reset API）。
 * 判据：8 个组合里若没有任何一个产出 oversized，则该 ID 的缺口**不在 libipc 接口**，
 *       而在落点本身（counters.h，W03 维护）⇒ 本任务只能立需求，不得越界实现。
 * usage: classifier_exhaustion <had_receiver 0|1> <finalize_ok 0|1> <publish_ok 0|1>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "dzIPC/measure/counters.h"

using namespace dzIPC::measure;

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: %s <hr> <fo> <po>\n", argv[0]);
        return 2;
    }
    const bool hr = std::atoi(argv[1]) != 0;
    const bool fo = std::atoi(argv[2]) != 0;
    const bool po = std::atoi(argv[3]) != 0;
    note_dzflat_borrow_failed(hr, fo, po);
    const char* which = "(未记任何计数)";
    unsigned long long total = 0;
    for (std::size_t i = 0; i < kCounterCount; ++i)
    {
        const auto v = static_cast<unsigned long long>(
            CounterRegistry::instance().get(static_cast<CounterId>(i)));
        if (v != 0)
        {
            which = counter_name(static_cast<CounterId>(i));
            total += v;
        }
    }
    std::printf("hr=%d fo=%d po=%d produced=%s total=%llu\n", (int)hr, (int)fo, (int)po, which, total);
    std::printf("OVERSIZED=%d\n", std::strcmp(which, "borrow_failed_oversized") == 0 ? 1 : 0);
    return 0;
}
