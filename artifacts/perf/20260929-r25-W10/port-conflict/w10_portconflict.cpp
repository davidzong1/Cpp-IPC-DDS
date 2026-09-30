/* W10 侦察：socket 订阅端 bind(组地址:PORT) 与"同机端口占用者"的交互（A/B 反事实）。
 *
 * ⛔ 结论链（本文件是最后一环；现象→定位→机制→反事实全部可复现）：
 *   · 现象：`w10_matrix --transport socket --topology independent --n 1000` 在 topic #38
 *     **永久挂起**（rc=124；30 s 内 373 行 `Failed to connect subscriber,reconnect 1s`）。
 *   · 定位：topic #38 的端口 = `udp_discovery_port_calculate("w10_socket_independent_3103_38", 3103)`
 *     = **48650**，而 `ss -uln` 显示 `0.0.0.0:48650` 已被**别的进程**占用；
 *     逐话题离线复算：1000 路里共 **12 路**的端口段 [base, base+4] 与本机已占 UDP 端口冲突。
 *   · 机制：`UDPNode::connect()` 用 SO_REUSEADDR|SO_REUSEPORT 绑**组地址** :PORT；占用者若
 *     **未开** SO_REUSEADDR，dzIPC 侧 bind 必 EADDRINUSE ⇒ connect() 返回 false ⇒
 *     `socket_sub_ipc::InitChannel` 的 `while (!subscriber_->connect()) sleep(1s)`
 *     **无上界重试、无诊断、不可退出**。
 *   · 本文件判定：占用者在位 ⇒ bind 失败；释放 ⇒ 立刻成功（排除 REUSEADDR 能共存）。
 *
 * 归属：**既有缺陷**，与本轮固定 worker 改造无关（端口公式、bind 方式、无界重试都在既有
 * `libipc`/socket 模块）。建议归 socket 模块侧：至少要有**有界重试 + 可识别诊断**。
 *
 * 用法：w10_portconflict <port> [group]
 */
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    const unsigned short port = static_cast<unsigned short>(argc > 1 ? std::atoi(argv[1]) : 48650);
    const char* group = argc > 2 ? argv[2] : "224.0.0.87";

    /* ① 模拟"任意进程的临时源端口"：bind 0.0.0.0:PORT，**不开** SO_REUSEADDR。 */
    int squatter = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(port);
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool squat_ok = ::bind(squatter, reinterpret_cast<sockaddr*>(&any), sizeof any) == 0;
    std::printf("step1 占用 0.0.0.0:%u => %s\n", port, squat_ok ? "ok（模拟临时端口占用者）" : std::strerror(errno));

    /* ② 模拟 dzIPC 订阅端：SO_REUSEADDR + SO_REUSEPORT + bind 组地址:PORT。 */
    const auto dz_bind = [&](int* out_fd) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        sockaddr_in ga{};
        ga.sin_family = AF_INET;
        ga.sin_port = htons(port);
        ::inet_pton(AF_INET, group, &ga.sin_addr);
        const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&ga), sizeof ga);
        *out_fd = (rc == 0) ? fd : -1;
        return rc;
    };
    int fd_b = -1;
    const int rc1 = dz_bind(&fd_b);
    std::printf("step2 占用者在位时 bind %s:%u => rc=%d errno=%s\n", group, port, rc1,
                rc1 < 0 ? std::strerror(errno) : "ok");

    /* ③ 反事实：释放占用者后立刻重试（对应 InitChannel 的 1 s 重试）。 */
    if (rc1 < 0 && squat_ok)
    {
        ::close(squatter);
        int fd_c = -1;
        const int rc2 = dz_bind(&fd_c);
        std::printf("step3 释放占用者后 bind => rc=%d errno=%s\n", rc2, rc2 < 0 ? std::strerror(errno) : "ok");
        if (fd_c >= 0) ::close(fd_c);
    }
    if (fd_b >= 0) ::close(fd_b);
    if (squat_ok) ::close(squatter);
    return (rc1 < 0) ? 1 : 0;
}
