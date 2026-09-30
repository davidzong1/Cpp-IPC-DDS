/* D-22 前置探针: socket_sub_ipc 的重试循环若要"区分 EADDRINUSE / 其它 errno",
 * 前提是**调用方在 connect() 返回 false 之后还能读到 bind 的 errno**。
 * libipc 的 `UDPNode::connect()` 是 IPC_EXCEPTION_ 包装 + pimpl 间接层,
 * 期间会走 ::close(fd) —— 该调用本身可能覆写 errno。本探针实测这一点。 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "libipc/udp.h"

int main(int argc, char** argv)
{
    const int port = (argc > 1) ? std::atoi(argv[1]) : 47777;
    const char* ip = (argc > 2) ? argv[2] : "224.0.0.87";
    int hog = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    /* 关键: 占用者**不开** SO_REUSEADDR ⇒ 后到的 bind 必 EADDRINUSE */
    if (::bind(hog, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
    {
        std::printf("hog_bind_failed=%d(%s)\n", errno, std::strerror(errno));
        return 2;
    }
    ipc::socket::UDPNode n("errno_probe", ip, static_cast<uint16_t>(port), ipc::socket::NodeRole::RecvOnly);
    errno = 0;
    const bool ok = n.connect();
    std::printf("port=%d connect_ok=%d errno_after=%d(%s) -> readable_as_EADDRINUSE=%d\n", port, (int)ok, errno,
                std::strerror(errno), (errno == EADDRINUSE) ? 1 : 0);
    ::close(hog);
    return 0;
}
