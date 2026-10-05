// 仅由测试驱动显式 LD_PRELOAD；保持真实源 socket/IP/端口，不改业务库。
#define _GNU_SOURCE
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
std::atomic<unsigned long> data_count{0}, ack_count{0}, dropped{0}, duplicated{0}, reordered{0};
unsigned kind(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    return size >= 160 && !std::memcmp(bytes, "DZMX", 4) ? bytes[5] : 0;
}
bool drop(unsigned type) {
    const char* mode = std::getenv("DZIPC_TEST_FAULT_MODE"); if (!mode) return false;
    if (type == 1) {
        const auto ordinal = ++data_count;
        const auto period = !std::strcmp(mode, "loss1") ? 100u : 20u;
        const bool reject = !std::strcmp(mode, "burst") ? (ordinal % 256 < 8) : ordinal % period == 0;
        if (reject) { ++dropped; return true; }
    } else if (type == 2 && ++ack_count == 1 && !std::strcmp(mode, "mixed")) {
        ++dropped; return true;
    }
    return false;
}
__attribute__((destructor)) void report() {
    const auto* directory = std::getenv("DZIPC_TEST_FAULT_REPORT_DIR");
    if (!directory || (!data_count.load() && !ack_count.load())) return;
    char path[4096]; std::snprintf(path, sizeof(path), "%s/fault-%d.json", directory, getpid());
    if (auto* file = std::fopen(path, "w")) {
        std::fprintf(file, "{\"data\":%lu,\"acks\":%lu,\"dropped\":%lu,\"duplicated\":%lu,\"reordered_batches\":%lu}\n",
                     data_count.load(), ack_count.load(), dropped.load(), duplicated.load(), reordered.load());
        std::fclose(file);
    }
}
}
extern "C" ssize_t sendto(int fd, const void* data, size_t size, int flags, const sockaddr* address, socklen_t length) {
    using Function = ssize_t(*)(int,const void*,size_t,int,const sockaddr*,socklen_t);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "sendto"));
    if (drop(kind(data, size))) return size;
    return real(fd,data,size,flags,address,length);
}
extern "C" int sendmmsg(int fd, mmsghdr* messages, unsigned count, int flags) {
    using Function = int(*)(int,mmsghdr*,unsigned,int);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT,"sendmmsg"));
    const auto* mode = std::getenv("DZIPC_TEST_FAULT_MODE");
    if (!mode) return real(fd,messages,count,flags);
    const bool mixed = !std::strcmp(mode,"mixed"); if (mixed && count > 1) ++reordered;
    for (unsigned index = 0; index < count; ++index) {
        const auto n = mixed ? count - index - 1 : index;
        auto& item = messages[n];
        if (!item.msg_hdr.msg_iovlen) return real(fd,messages,count,flags);
        const auto& io = item.msg_hdr.msg_iov[0];
        const auto type = kind(io.iov_base,io.iov_len);
        item.msg_len = io.iov_len;
        if (drop(type)) continue;
        // 故障驱动把内核拒收也视为丢失，网关按原身份和期限自行恢复。
        real(fd,&item,1,flags);
        if (mixed && type == 1 && data_count.load() % 10 == 0) { real(fd,&item,1,flags); ++duplicated; }
    }
    return count;
}
