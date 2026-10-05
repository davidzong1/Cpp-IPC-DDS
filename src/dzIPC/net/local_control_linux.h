#pragma once
#include "dzIPC/net/local_protocol.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dzIPC::net::local
{
class Fd
{
  public:
    explicit Fd(int value = -1) noexcept : value_(value)
    {
    }
    ~Fd();
    Fd(Fd &&other) noexcept : value_(other.release())
    {
    }
    Fd &operator=(Fd &&other) noexcept;
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const noexcept
    {
        return value_;
    }
    int release() noexcept
    {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) noexcept;
    explicit operator bool() const noexcept
    {
        return value_ >= 0;
    }

  private:
    int value_;
};
struct Credentials
{
    std::int32_t pid = 0;
    std::uint32_t uid = 0;
};
Credentials credentials(int fd);
Identity locality();
Identity random_identity();
std::uint64_t random_epoch();
std::uint64_t monotonic_ns() noexcept;
std::uint64_t process_start(std::int32_t pid);
std::uint64_t clock_domain(std::int32_t pid);
std::string hex(const Identity &id);
Fd event();
void notify(int fd) noexcept;
void drain_event(int fd) noexcept;
bool ready(int fd, short events, std::uint64_t deadline);
Fd connect_control(const std::string &path, std::uint64_t deadline);
enum class Receive
{
    Packet,
    WouldBlock,
    Closed,
    Invalid
};
struct Packet
{
    Bytes bytes;
    std::vector<Fd> descriptors;
};
Receive receive(int fd, Packet &packet);
bool send(int fd, ByteView bytes, int descriptor = -1); // false = EAGAIN/EINTR

// 目录 FD、锁和 socket 同寿命；只能清理由本对象创建的 socket inode。
class Listener
{
  public:
    explicit Listener(const std::string &path);
    ~Listener();
    int fd() const noexcept
    {
        return socket_.get();
    }

  private:
    Fd directory_, lock_, socket_;
    std::string name_;
    std::uint64_t inode_ = 0;
};
} // namespace dzIPC::net::local
