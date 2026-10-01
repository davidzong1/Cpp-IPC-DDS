/* t58 独立复算：socket 侧 `DZIPC_SOCKET_COMPAT_THREAD` 的极性对照（用于核对
 * W05 交付 §4.1 的"与 socket 侧极性相反"这一声明）。 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <chrono>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
static constexpr std::uint32_t kMsgId = 93;
static std::size_t tc(){ DIR*d=opendir("/proc/self/task"); if(!d) return 0; std::size_t n=0;
  while(dirent*e=readdir(d)) if(e->d_name[0]!='.') ++n; closedir(d); return n; }
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long n = argc > 1 ? strtol(argv[1], nullptr, 10) : 5;
    const long dom = argc > 2 ? strtol(argv[2], nullptr, 10) : 8000;
    const char* v = getenv("DZIPC_SOCKET_COMPAT_THREAD");
    const char* show = v ? (v[0] ? v : "<empty>") : "<unset>";
    printf("env_DZIPC_SOCKET_COMPAT_THREAD=%s n=%ld\n", show, n);
    printf("threads_before=%zu\n", tc());
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    for (long i = 0; i < n; ++i)
    {
        const std::string t = "t58sock_" + std::to_string(dom) + "_" + std::to_string(i);
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
        subs.emplace_back(new dzIPC::socket::socket_sub_ipc(td, t, (size_t)dom, 16, false));
        subs.back()->InitChannel("t58s");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    printf("threads_after=%zu threads_per_topic=%.3f\n", tc(), n ? (double)tc()/n : 0.0);
    printf("T58_SOCKPOL_DONE\n");
    _exit(0);
}
