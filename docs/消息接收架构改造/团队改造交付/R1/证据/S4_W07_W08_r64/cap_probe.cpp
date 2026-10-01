/* t64 独立容量探针：直接问池子"能注册几路"（不依赖被审用例的断言） */
#include "dzIPC/ipc_info_pool.h"
#include <cstdio>
#include <string>
#include <vector>
namespace ip = dzIPC::info_pool;
int main()
{
    std::printf("kMaxEntries=%zu segment_bytes=%zu\n", ip::kMaxEntries, ip::segment_bytes());
    std::vector<int32_t> s; int first_fail = -1;
    for (int i = 0; i < 1200; ++i)
    {
        int32_t h = ip::IpcInfoPool::instance().register_entry(
            {ip::EntryKind::SocketSub, "t64cap_" + std::to_string(i), "", "", 0, ""});
        if (h < 0) { first_fail = i; break; }
        s.push_back(h);
    }
    std::printf("registered=%zu first_fail_index=%d\n", s.size(), first_fail);
    for (int32_t h : s) ip::IpcInfoPool::instance().unregister_entry(h);
    return first_fail < 0 ? 0 : 1;
}
