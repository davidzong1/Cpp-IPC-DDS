/* info_pool 全局性判定: N 个进程各注册 M 条, 看是否共享同一个 512 槽段。 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>
#include "dzIPC/ipc_info_pool.h"
int main(int argc, char** argv) {
    const int m = argc > 1 ? atoi(argv[1]) : 300;
    const int tag = argc > 2 ? atoi(argv[2]) : 0;
    int ok = 0, fail = 0;
    std::vector<dzIPC::info_pool::ScopedRegistration> regs;
    regs.reserve(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        dzIPC::info_pool::RegisterInfo info;
        info.kind = dzIPC::info_pool::EntryKind::SocketSub;
        info.topic_name = "poolprobe_" + std::to_string(tag) + "_" + std::to_string(i);
        info.type_name = "T"; info.ipc_mode = "socket"; info.domain_id = 900 + tag;
        regs.emplace_back(info);
        if (regs.back().valid()) ++ok; else ++fail;
    }
    std::printf("pid=%d tag=%d want=%d ok=%d fail=%d\n", (int)getpid(), tag, m, ok, fail);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto snap = dzIPC::info_pool::IpcInfoPool::instance().snapshot(false);
    int mine = 0, other = 0;
    for (auto& e : snap) { if (e.in_use) { if (e.domain_id == 900 + tag) ++mine; else ++other; } }
    std::printf("pid=%d tag=%d snapshot_in_use=%zu mine=%d other=%d\n", (int)getpid(), tag, snap.size(), mine, other);
    return 0;
}
