/* W07 一次性探针: 打印容量与共享布局的**实测**数字(供容量表/交付引用)。
 * 不是回归用例(回归在 test/test_ipc_info_pool_layout.cpp)。
 * 编译见 artifacts/perf/20260928-r01-W07/command.txt。 */
#include <cstdio>
#include <sys/stat.h>

#include "dzIPC/ipc_info_pool.h"

int main()
{
    using namespace dzIPC::info_pool;
    std::printf("kMaxEntries=%zu\n", kMaxEntries);
    std::printf("kPoolEntryBytes=%zu\n", kPoolEntryBytes);
    std::printf("kLayoutMagic=0x%08X\n", kLayoutMagic);
    std::printf("kLayoutVersion=%u\n", kLayoutVersion);
    std::printf("segment_bytes()=%zu\n", segment_bytes());

    const int32_t slot = IpcInfoPool::instance().register_entry(
        {EntryKind::SocketSub, "w07probe", "probe", "socket", 0, ""});
    std::printf("register_slot=%d\n", slot);

    struct stat st{};
    const char* path = "/dev/shm/dz_ipc_info_pool_v2";
    if (::stat(path, &st) == 0)
    {
        std::printf("shm_file=%s size=%lld\n", path, static_cast<long long>(st.st_size));
        std::printf("mapped_bytes_minus_segment=%lld\n",
                    static_cast<long long>(st.st_size) - static_cast<long long>(segment_bytes()));
    }
    else
    {
        std::printf("shm_file=%s stat_failed\n", path);
    }

    std::printf("pipe_headroom_entries=%zu\n", kMaxEntries - 1000u);
    return 0;
}
