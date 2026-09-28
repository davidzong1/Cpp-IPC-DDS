// t6 fd-boundary probe — ABI-valid (compiled against CURRENT headers).
// Replaces build_baseline/bin/baseline_probe_after, which is compiled against PRE-port headers
// and is therefore ABI-stale for the ported lib (class layouts changed -> heap corruption).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {
constexpr std::uint32_t kMsgId = 37;

long arg_long(int argc, char** argv, const char* key, long def) {
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], key, klen) && argv[i][klen] == '=')
            return std::strtol(argv[i] + klen + 1, nullptr, 10);
        if (!std::strcmp(argv[i], key) && i + 1 < argc)
            return std::strtol(argv[i + 1], nullptr, 10);
    }
    return def;
}
bool arg_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], key)) return true;
    return false;
}
const char* arg_str(int argc, char** argv, const char* key, const char* def) {
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], key)) return argv[i + 1];
    return def;
}
std::size_t fd_count(std::size_t* maxfd) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    std::size_t n = 0, mx = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ++n;
        const long v = std::strtol(e->d_name, nullptr, 10);
        if (v > 0 && static_cast<std::size_t>(v) > mx) mx = static_cast<std::size_t>(v);
    }
    closedir(d);
    if (maxfd) *maxfd = mx;
    return n;
}
std::size_t thread_count() {
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    std::size_t n = 0;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n;
    closedir(d);
    return n;
}
void ms(long m) { std::this_thread::sleep_for(std::chrono::milliseconds(m)); }
}  // namespace

int main(int argc, char** argv) {
    const long scale   = arg_long(argc, argv, "--scale", 1);
    const long domain  = arg_long(argc, argv, "--domain", 1);
    const long idle    = arg_long(argc, argv, "--idle-ms", 500);
    const bool publish = arg_flag(argc, argv, "--publish");
    const std::string topic = arg_str(argc, argv, "--topic", "fdprobe");
    std::printf("fdprobe pid=%d scale=%ld domain=%ld publish=%d topic=%s\n",
                (int)getpid(), scale, domain, (int)publish, topic.c_str());
    std::fflush(stdout);

    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    subs.reserve(static_cast<std::size_t>(scale));
    for (long i = 0; i < scale; ++i) {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(
            td, topic, static_cast<std::size_t>(domain), 8, false));
        subs.back()->InitChannel("fdprobe");
        if ((i + 1) % 100 == 0) {
            std::size_t mx = 0; const std::size_t n = fd_count(&mx);
            std::printf("[fdprobe] created %ld/%ld fds=%zu maxfd=%zu threads=%zu\n",
                        i + 1, scale, n, mx, thread_count());
            std::fflush(stdout);
        }
    }
    std::size_t mx = 0; std::size_t n = fd_count(&mx);
    std::printf("[fdprobe] all created: fds=%zu maxfd=%zu threads=%zu (FD_SETSIZE=1024)\n",
                n, mx, thread_count());
    std::fflush(stdout);

    if (publish) {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::socket::socket_pub_ipc pub{td, topic, static_cast<std::size_t>(domain), false};
        pub.InitChannel("fdprobe");
        ms(300);
        auto m = std::make_shared<dzIPC::Msg::StdImage>();
        const bool ok = pub.publish(m);
        std::printf("[fdprobe] published=%d -> data now flows into the receive path\n", (int)ok);
        std::fflush(stdout);
        ms(idle);
        std::printf("[fdprobe] SURVIVED publish (no abort)\n");
        std::fflush(stdout);
    } else {
        ms(idle);
        std::printf("[fdprobe] SURVIVED idle (no abort)\n");
        std::fflush(stdout);
    }
    return 0;
}
