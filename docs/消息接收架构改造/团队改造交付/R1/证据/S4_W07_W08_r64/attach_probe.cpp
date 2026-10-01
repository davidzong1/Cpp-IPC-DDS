/* t64：在"同段名上已有旧布局段"时，本版本池的第一次 register_entry 必须显式拒绝。
 * 段名由环境变量控制，便于分别验证 #1（同段名 → 拒绝）与 #2（异段名 → 不触碰）。 */
#include "dzIPC/ipc_info_pool.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static const char* envv(const char* k, const char* d) { const char* v = ::getenv(k); return v ? v : d; }
int main()
{
    const std::string name = envv("T64_POOL_NAME", "dz_ipc_info_pool_t64mix");
    std::printf("pool_name=%s\n", name.c_str());
    std::fflush(stdout);
    const int32_t slot = dzIPC::info_pool::IpcInfoPool::instance().register_entry(
        {dzIPC::info_pool::EntryKind::SocketSub, "t64_mix", "", "", 0, ""});
    std::printf("slot=%d\n", slot);
    return 0;
}
