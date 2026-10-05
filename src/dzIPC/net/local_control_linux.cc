#include "local_control_linux.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/net/shared_config.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace dzIPC::net::local
{
namespace
{
[[noreturn]] void fail(const std::string &message)
{
    throw std::system_error(errno, std::generic_category(), message);
}
sockaddr_un address(const std::string &path)
{
    if (path.size() >= sizeof(sockaddr_un::sun_path))
        throw std::invalid_argument("Unix 路径过长");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}
void random_bytes(void *destination, std::size_t size)
{
    auto *p = static_cast<unsigned char *>(destination);
    while (size)
    {
        const auto n = getrandom(p, size, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            fail("读取随机身份失败");
        }
        p += n;
        size -= n;
    }
}
} // namespace
Fd::~Fd()
{
    reset();
}
Fd &Fd::operator=(Fd &&other) noexcept
{
    if (this != &other)
        reset(other.release());
    return *this;
}
void Fd::reset(int value) noexcept
{
    if (value_ >= 0)
        ::close(value_);
    value_ = value;
}
Credentials credentials(int fd)
{
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || size != sizeof(peer))
        fail("读取本机凭据失败");
    return {peer.pid, peer.uid};
}
Identity locality()
{
    const auto id = info_pool::IpcInfoPool::instance().local_identity();
    if (!nonzero(id))
        throw std::runtime_error("LocalityUnavailable: 本机 IPC 身份不可用");
    return id;
}
Identity random_identity()
{
    Identity id;
    do
    {
        random_bytes(id.data(), id.size());
    } while (!nonzero(id));
    return id;
}
std::uint64_t random_epoch()
{
    std::uint64_t value;
    do
    {
        random_bytes(&value, sizeof(value));
    } while (!value);
    return value;
}
std::uint64_t monotonic_ns() noexcept
{
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
std::uint64_t process_start(std::int32_t pid)
{
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string text;
    std::getline(input, text);
    const auto pos = text.rfind(')');
    if (pos == std::string::npos)
        throw std::runtime_error("无法核验进程启动标识");
    std::istringstream fields(text.substr(pos + 2));
    std::string value;
    for (unsigned field = 3; field <= 22; ++field)
        if (!(fields >> value))
            throw std::runtime_error("进程启动标识不完整");
    return std::stoull(value);
}
std::uint64_t clock_domain(std::int32_t pid)
{
    struct stat st
    {
    };
    const auto path = "/proc/" + std::to_string(pid) + "/ns/time";
    if (::stat(path.c_str(), &st))
        fail("无法核验时钟命名空间");
    return st.st_ino;
}
std::string hex(const Identity &id)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto b : id)
    {
        result += digits[b >> 4];
        result += digits[b & 15];
    }
    return result;
}
Fd event()
{
    Fd result(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!result)
        fail("创建 eventfd 失败");
    return result;
}
void notify(int fd) noexcept
{
    const std::uint64_t value = 1;
    while (::write(fd, &value, sizeof(value)) < 0 && errno == EINTR)
    {
    }
}
void drain_event(int fd) noexcept
{
    std::uint64_t value;
    while (::read(fd, &value, sizeof(value)) < 0 && errno == EINTR)
    {
    }
}
bool ready(int fd, short events, std::uint64_t deadline)
{
    while (true)
    {
        const auto now = monotonic_ns();
        if (now >= deadline)
            return false;
        const auto remaining = deadline - now;
        pollfd p{fd, events, 0};
        const int timeout =
            static_cast<int>(std::min<std::uint64_t>(5000, (remaining + 999999) / 1000000));
        const int n = ::poll(&p, 1, timeout);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fail("等待本机控制连接失败");
        if (n)
            return p.revents & (events | POLLHUP | POLLERR);
    }
    return false;
}
Fd connect_control(const std::string &path, std::uint64_t deadline)
{
    const auto status = validate_control_path(path);
    if (!status)
        throw ConfigError(status);
    Fd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd)
        fail("创建控制连接失败");
    auto addr = address(path);
    if (::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) &&
        errno != EINPROGRESS)
        fail("连接网关失败");
    if (!ready(fd.get(), POLLOUT, deadline))
        throw std::runtime_error("网关握手超时");
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &size))
        fail("读取连接状态失败");
    if (error)
    {
        errno = error;
        fail("连接网关失败");
    }
    if (credentials(fd.get()).uid != geteuid())
        throw std::runtime_error("网关 UID 不匹配");
    return fd;
}
Receive receive(int fd, Packet &result)
{
    std::array<std::uint8_t, kLocalMaxSize> buffer{};
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(16 * sizeof(int))> control{};
    iovec iov{buffer.data(), buffer.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    const auto n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return Receive::WouldBlock;
        return Receive::Closed;
    }
    Packet packet;
    bool invalid = msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC);
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
    {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0))
        {
            invalid = true;
            continue;
        }
        const auto bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int))
            invalid = true;
        for (std::size_t i = 0; i + sizeof(int) <= bytes; i += sizeof(int))
        {
            int value;
            std::memcpy(&value, CMSG_DATA(c) + i, sizeof(value));
            packet.descriptors.emplace_back(value);
        }
    }
    if (invalid || n > static_cast<ssize_t>(buffer.size()))
        return Receive::Invalid; // RAII 关闭所有已接收 FD
    if (!n)
        return Receive::Closed;
    packet.bytes.assign(buffer.begin(), buffer.begin() + n);
    result = std::move(packet);
    return Receive::Packet;
}
bool send(int fd, ByteView bytes, int descriptor)
{
    if (bytes.size > kLocalMaxSize || (!bytes.data && bytes.size))
        throw std::invalid_argument("控制报文长度错误");
    iovec iov{const_cast<std::uint8_t *>(bytes.data), bytes.size};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(int))> control{};
    if (descriptor >= 0)
    {
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        auto *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &descriptor, sizeof(descriptor));
    }
    const auto n = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return false;
    if (n < 0)
        fail("发送本机控制报文失败");
    if (static_cast<std::size_t>(n) != bytes.size)
        throw std::runtime_error("控制报文出现部分发送");
    return true;
}
Listener::Listener(const std::string &path)
{
    auto status = validate_control_path(path);
    if (!status)
        throw ConfigError(status);
    const auto slash = path.rfind('/');
    name_ = path.substr(slash + 1);
    const auto parent = path.substr(0, slash);
    directory_.reset(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_)
        fail("打开根目录失败");
    std::size_t pos = 1;
    while (pos < parent.size())
    {
        auto end = parent.find('/', pos);
        if (end == std::string::npos)
            end = parent.size();
        const auto component = parent.substr(pos, end - pos);
        if (end == parent.size() && mkdirat(directory_.get(), component.c_str(), 0700) &&
            errno != EEXIST)
            fail("创建网关目录失败");
        Fd next(openat(directory_.get(), component.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next)
            fail("打开网关目录失败");
        directory_ = std::move(next);
        pos = end + 1;
    }
    struct stat st
    {
    };
    if (fstat(directory_.get(), &st))
        fail("读取网关目录失败");
    if (st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)
        throw std::runtime_error("网关目录必须由当前 UID 拥有且权限为 0700");
    lock_.reset(
        openat(directory_.get(), "gateway.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!lock_)
        fail("打开网关锁失败");
    if (fstat(lock_.get(), &st))
        fail("读取网关锁失败");
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0777) != 0600)
        throw std::runtime_error("网关锁类型或权限错误");
    if (flock(lock_.get(), LOCK_EX | LOCK_NB))
        fail("GatewayAlreadyRunning: 网关锁已占用");
    const auto anchored = "/proc/self/fd/" + std::to_string(directory_.get()) + "/" + name_;
    if (!fstatat(directory_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW))
    {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
            throw std::runtime_error("控制路径不是本 UID 的 socket");
        Fd probe(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!probe)
            fail("探测旧控制路径失败");
        auto a = address(anchored);
        if (::connect(probe.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 ||
            errno != ECONNREFUSED)
            throw std::runtime_error("旧控制 socket 仍存活或状态未知");
        if (unlinkat(directory_.get(), name_.c_str(), 0))
            fail("清理失效控制 socket 失败");
    }
    else if (errno != ENOENT)
        fail("检查控制路径失败");
    socket_.reset(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket_)
        fail("创建 Unix 监听失败");
    auto a = address(anchored);
    if (::bind(socket_.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)))
        fail("绑定 Unix 监听失败");
    if (fstatat(directory_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW))
        fail("读取控制 socket inode 失败");
    inode_ = st.st_ino;
    if (fchmodat(directory_.get(), name_.c_str(), 0600, 0) || ::listen(socket_.get(), 128))
    {
        const int error = errno;
        unlinkat(directory_.get(), name_.c_str(), 0);
        errno = error;
        fail("初始化 Unix 监听失败");
    }
}
Listener::~Listener()
{
    socket_.reset();
    struct stat st
    {
    };
    if (inode_ && !fstatat(directory_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) &&
        std::uint64_t(st.st_ino) == inode_)
        unlinkat(directory_.get(), name_.c_str(), 0);
}
} // namespace dzIPC::net::local
