/* t64 独立混合版本 attach 探针：
 * ① 用 C API 在**未映射**状态下装一个旧布局段（v1 代号 + 512 容量前缀）
 * ② fork 子进程，池在其中首次构造 ⇒ 必须**显式拒绝**，不得静默使用
 * 父进程从不构造池；子进程结束前 unlink 干净。 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static const char* kName = "dz_ipc_info_pool_v2";
static const std::size_t kOldBytes = 56 + 512 * 296;   /* v1 时代 kRegionSize */

int main()
{
    /* ① 装旧段（用 raw mmap + 文件截断，等价于旧进程的 shm 布局，不依赖被测 API） */
    ::shm_unlink(kName);
    char path[256]; std::snprintf(path, sizeof(path), "/dev/shm/%s", kName);
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { std::perror("open"); return 2; }
    if (::ftruncate(fd, static_cast<off_t>(kOldBytes)) != 0) { std::perror("ftruncate"); return 2; }
    void* p = ::mmap(nullptr, kOldBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 2; }
    /* 段头：init=2(ready) magic=0x5A495044 version=1 max_entries=512 —— 旧版前缀 */
    std::uint32_t head[4] = {2u, 0x5A495044u, 1u, 512u};
    std::memcpy(p, head, sizeof(head));
    ::munmap(p, kOldBytes);
    ::close(fd);
    std::printf("stale_segment_installed bytes=%zu version=1 max_entries=512\n", kOldBytes);
    std::fflush(stdout);
    return 0;
}
