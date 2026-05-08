#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libipc/export.h"
#include "libipc/shm.h"

namespace dzIPC {
namespace info_pool {
/* 当前条目的通信角色 */
enum class EntryKind : uint32_t {
    Unknown = 0,
    ShmPub = 1,
    ShmSub = 2,
    SocketPub = 3,
    SocketSub = 4,
    ShmServer = 5,
    ShmClient = 6,
    SocketServer = 7,
    SocketClient = 8,
};

IPC_EXPORT const char* to_string(EntryKind kind) noexcept;
IPC_EXPORT const char* get_type_from_kind(EntryKind kind) noexcept;
/* Best-effort demangle（ABI 为 Itanium 时有效；回退为原始 mangled 名） */
IPC_EXPORT std::string demangle(const char* mangled);

/* 池内字符串字段的定长上限（与 PoolEntry 物理布局严格对应） */
constexpr std::size_t kMaxTopicName = 128;
constexpr std::size_t kMaxTypeName = 64;
constexpr std::size_t kMaxExtra = 64;
constexpr std::size_t kMaxEntries = 512;

/* 注册时提交给池的元信息 */
struct RegisterInfo
{
    EntryKind kind{EntryKind::Unknown};
    std::string topic_name;
    std::string type_name;
    std::string ipc_mode;
    int32_t domain_id;
    std::string extra;
};

/* 第三方观察进程读取时得到的每条记录 */
struct EntrySnapshot
{
    int32_t slot{-1};
    EntryKind kind{EntryKind::Unknown};
    int32_t pid{0};
    int64_t register_ts_ns{0};
    int64_t heartbeat_ns{0};
    std::string topic_name;
    std::string type_name;
    int32_t domain_id{0};
    std::string extra;
    bool in_use{true};
    bool alive{true};
};

class IPC_EXPORT IpcInfoPool
{
public:
    static IpcInfoPool& instance();

    /* 注册一条记录，返回 slot（>=0 成功，-1 失败） */
    int32_t register_entry(const RegisterInfo& info);

    /* 清除指定 slot；slot < 0 时静默忽略 */
    void unregister_entry(int32_t slot);

    /* 刷新心跳时间戳（steady_clock ns） */
    void heartbeat(int32_t slot);

    /* 整表快照；gc_dead=true 时同步回收 pid 已不存在的条目 */
    std::vector<EntrySnapshot> snapshot(bool gc_dead = true);

    /* 主动回收 pid 已不存在的条目，返回回收数量 */
    std::size_t gc_dead();

    /* 摧毁底层共享内存；仅用于测试/清理工具 */
    static void reset_storage();

    IpcInfoPool(const IpcInfoPool&) = delete;
    IpcInfoPool& operator=(const IpcInfoPool&) = delete;

private:
    IpcInfoPool();
    ~IpcInfoPool();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* RAII 包装：构造时注册，析构时注销；建议作为 pub/sub/ser/cli 成员 */
class IPC_EXPORT ScopedRegistration
{
public:
    ScopedRegistration() = default;
    explicit ScopedRegistration(const RegisterInfo& info);

    ScopedRegistration(const ScopedRegistration&) = delete;
    ScopedRegistration& operator=(const ScopedRegistration&) = delete;

    ScopedRegistration(ScopedRegistration&& other) noexcept;
    ScopedRegistration& operator=(ScopedRegistration&& other) noexcept;

    ~ScopedRegistration();

    bool valid() const noexcept { return slot_ >= 0; }

    int32_t slot() const noexcept { return slot_; }

    /* 重新绑定到另一条记录；会先注销当前的 slot */
    void rebind(const RegisterInfo& info);

    /* 主动释放 slot，可幂等调用 */
    void reset();

    /* 刷新心跳 */
    void heartbeat();

private:
    int32_t slot_{-1};
};

}   // namespace info_pool
}   // namespace dzIPC
