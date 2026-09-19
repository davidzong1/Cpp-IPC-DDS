/* 一次性诊断: 列出所有 CHUNK_INFO 段, 打印文件大小 + 头 33 字节 + 空闲链走链结果。
 * 由 docs/shm_chunk_pool_occupancy_plan.md 步骤② 的量测准备阶段用, 不进 test/
 * (CMake 的 test/*.cpp 是 GLOB, 放进去会多出一个非 gtest 目标)。 */
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <string>

int main()
{
    DIR* d = opendir("/dev/shm");
    if (d == nullptr) { std::perror("opendir"); return 1; }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strstr(e->d_name, "CHUNK_INFO") == nullptr) continue;
        const std::string p = std::string("/dev/shm/") + e->d_name;
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f == nullptr) { std::printf("%-52s fopen 失败\n", e->d_name); continue; }
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        unsigned char b[33];
        const size_t got = std::fread(b, 1, sizeof(b), f);
        std::fclose(f);
        std::printf("%-52s size=%-9ld fread=%zu cursor_=%u\n",
                    e->d_name, sz, got, got >= 33 ? b[32] : 0xFF);
        if (got < 33) continue;
        std::printf("    next_[0..32] =");
        for (int i = 0; i < 33; ++i) std::printf(" %u", b[i]);
        std::printf("\n");
        unsigned cur = b[32];
        int n = 0;
        bool ok = true;
        while (cur < 32 && n <= 32) { cur = b[cur]; ++n; }
        if (n > 32) ok = false;
        std::printf("    走链: ok=%d free=%d L=%d\n", ok ? 1 : 0, ok ? n : -1, ok ? 32 - n : -1);
    }
    closedir(d);
    return 0;
}
