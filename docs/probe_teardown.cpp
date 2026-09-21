/* 一次性探针: route 销毁到底还不还它钉住的 chunk?
 *
 * 读法: chunk_info_t 的第一个成员是 id_pool<> pool_, 其布局为
 *   next_[40]  偏移 0..39   (id_type<0,Align> 就是 1 字节)
 *   cursor_    偏移 40      ← 空闲链表头; == max_count(40) 表示池空
 *   prepared_  偏移 41
 * (池容量 40 = ipc::large_msg_cache, 见 include/libipc/def.h; 改容量时本探针的
 *  偏移与魔数要跟着改 —— 它是裸字节读法, 刻意不引结构体。)
 * 段是 tmpfs 文件, 本进程一边 mmap 一边可以按文件读同一份内存。
 *
 * 阳性对照: 钉干后 cursor_ 必须 == 40(否则读法本身无效, "没变"不可信)。
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "libipc/ipc.h"
#include "libipc/shm.h"

namespace {

constexpr std::size_t kChunkSize = 7168;
constexpr unsigned kPoolCap = 40;   /* = ipc::large_msg_cache(def.h); 裸字节读法故用常量 */

unsigned cursor_of()
{
    std::FILE* f = std::fopen("/dev/shm/__IPC_SHM__CHUNK_INFO__7168__C40", "rb");
    if (f == nullptr) return 0xFFFF;   // 段不存在
    unsigned char b = 0;
    if (std::fseek(f, kPoolCap, SEEK_SET) != 0 || std::fread(&b, 1, 1, f) != 1) b = 0xFF;
    std::fclose(f);
    return b;
}

void report(const char* when)
{
    const unsigned c = cursor_of();
    std::printf("%-28s cursor_ = %u  %s\n", when, c,
                c == 0xFFFF ? "(段不存在)"
                            : (c == kPoolCap ? "→ 池空(全部 40 块都在池外)"
                                             : (c == 0 ? "→ 池满(40 块全可借)" : "→ 部分占用")));
}

}   // namespace

int main()
{
    const char* name = "probe_teardown_release";
    ipc::route::clear_storage(name);
    ipc::shm::handle::clear_storage("__IPC_SHM__CHUNK_INFO__7168__C40");

    std::vector<std::uint8_t> payload(6144, 0xC3);

    report("段刚清掉");
    {
        ipc::route tx{name, ipc::sender};
        ipc::route rx{name, ipc::receiver};   // 从不 recv
        if (!tx.wait_for_recv(1, 2000)) { std::printf("接收方没连上\n"); return 2; }

        report("建 route 后(未发送)");
        for (int i = 0; i < 33; ++i) {
            if (!tx.send(payload.data(), payload.size())) { std::printf("发送失败\n"); return 3; }
        }
        report("钉干 33 条后");
    }
    report("route 已销毁后");
    return 0;
}
