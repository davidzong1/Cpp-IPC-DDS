#pragma once

#include <cstddef>
#include <array>
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

/* ---------------------------------------------------------------------------
 * W07 容量与共享布局契约（方案 §6.1 有效规模 1000 路 / §10.5 跨进程版本协商）
 * ---------------------------------------------------------------------------
 * 容量取值依据: 目标规模是 **1000 路独立话题**; 每个端点在池里占且只占 1 条,
 * 1000 话题两端齐全 = 2000 条 ⇒ 取 4096 = 2000 × 2 + 96 余量。
 *
 * ⛔ 本值是**编译期常量**(属于启动配置), ⛔不随 worker 数/route 数在运行中变化。
 * ⛔ 池是**全机共享**的: 4096 条是全机总预算, 不是"每进程 4096 条"。
 * ⛔ 超限时**首个失败资源是池槽位**: 第 kMaxEntries+1 次 register_entry 返回 -1 并
 *    打限流诊断(表满/回收后仍满), ⛔不是 fd 或内存耗尽。
 *
 * 内存对账(kPoolEntryBytes 由 .cc 的 static_assert 钉住, 见 segment_bytes()):
 *   PoolEntry 296 B ⇒ 512 条 148 KiB / 4096 条 1.16 MiB(含段头 56 B)。
 *   对 W00 冻结的 /dev/shm ≤ 2 GiB / RSS 增量 ≤ 1 GiB 可忽略。
 * ------------------------------------------------------------------------- */
constexpr std::size_t kMaxEntries = 4096;

/* 共享布局的身份: **段名带版本后缀** + 段头自带 magic/version/max_entries。
 *
 * 为什么必须同时带版本(而不是只把 kMaxEntries 改大): 本仓 libipc 的
 * `shm::handle::acquire(name, size)` 对**既有**段也会按调用方请求的 size 做
 * ftruncate(见 src/libipc/platform/shm_posix.cpp 的 get_mem)。若新旧二进制共用
 * 一个段名, 改容量后的新进程会把旧进程的段 resize 成自己的 size, 而两侧的引用
 * 计数落在**各自 mapped 区间的末尾**(不同地址) ⇒ 一方 release 就可能把另一方还在
 * 用的段 unlink 掉; 且新进程按 kMaxEntries 遍历旧段的 entries 数组会读越界。
 * 因此: 布局一变就换段名后缀 + 升 kLayoutVersion, 旧段与新段物理隔离。
 * 回滚/清理见 .cc 顶部「跨版本与旧段处置」。 */
constexpr std::uint32_t kLayoutMagic = 0x5A'49'50'44u;   // 'DZIP'
constexpr std::uint32_t kLayoutVersion = 4;
/* PoolEntry 的字节数(与 .cc 的 offsetof/sizeof static_assert 同源)。 */
constexpr std::size_t kPoolEntryBytes = 296;

/* 本版本池段的总字节数 = sizeof(PoolHeader) + kMaxEntries * sizeof(PoolEntry)。
 * 容量表(交付/W09)直接引用它, 不手抄数字。 */
IPC_EXPORT std::size_t segment_bytes() noexcept;

/* attach 时的**拒绝性**布局校验: 判定一个既有段是否属于本布局。
 *
 * 传入段首至少 16 字节的原始镜像(通常就是 mmap 基址)与已映射字节数。
 * 返回 nullptr = 相容; 否则返回**静态**拒绝原因(可直接进日志/被用例断言)。
 * 段头字段偏移是冻结契约: init_state@0, magic@4, version@8, max_entries@12
 * (由 .cc 的 offsetof static_assert 钉住; ⛔两者必须同改)。
 * ⛔ 调用方必须在**访问 entries 之前**用它做门禁 —— 旧布局的 entries 数组比本
 *    版本的短, 按 kMaxEntries 遍历即读越界。 */
IPC_EXPORT const char* layout_mismatch(const void* mapped, std::size_t mapped_bytes) noexcept;


/* 注册时提交给池的元信息 */
struct RegisterInfo
{
    EntryKind kind{EntryKind::Unknown};
    std::string topic_name;
    std::string type_name;
    std::string ipc_mode;
    uint64_t domain_id;
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
    uint64_t domain_id{0};
    std::string extra;
    bool in_use{true};
    bool alive{true};
};

class IPC_EXPORT IpcInfoPool
{
public:
    static IpcInfoPool& instance();

    // 当前共享内存实例身份；全零表示不可用。用于判定混合传输的本机可达性。
    std::array<std::uint8_t, 16> local_identity() const noexcept;

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
