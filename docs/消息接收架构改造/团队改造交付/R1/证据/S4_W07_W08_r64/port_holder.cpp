/* t64：自控端口占位器（不经 dzIPC）—— 在 239.255.213.238:48650 上绑定且**不设** SO_REUSEADDR。
 * 目的：把 D-22 的"外部端口占用"变成我可复现的注入件。 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv)
{
    const char* ip = argc > 1 ? argv[1] : "239.255.213.238";
    int port = argc > 2 ? std::atoi(argv[2]) : 48650;
    int hold = argc > 3 ? std::atoi(argv[3]) : 70;
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 2; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) { std::perror("bind"); return 3; }
    std::printf("holder bound %s:%d for %ds\n", ip, port, hold);
    std::fflush(stdout);
    ::sleep((unsigned)hold);
    return 0;
}
