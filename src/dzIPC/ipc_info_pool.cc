#include "dzIPC/ipc_info_pool.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <process.h>
#    include <windows.h>
#else
#    include <cxxabi.h>
#    include <pthread.h>
#    include <signal.h>
#    include <unistd.h>
#endif

#include "libipc/shm.h"

namespace dzIPC {
namespace info_pool {
namespace {
constexpr char kShmName[] = "dz_ipc_info_pool_v1";
#if defined(_WIN32)
constexpr char kMutexName[] = "Global\\dz_ipc_info_pool_v1_mtx";
#endif
constexpr uint32_t kMagic = 0x5A'49'50'44u;   // 'DZIP'
constexpr uint32_t kVersion = 1;

constexpr uint32_t kInitUninit = 0;
constexpr uint32_t kInitInProgress = 1;
constexpr uint32_t kInitReady = 2;

/* 单条记录的定长 POD，放在共享内存里跨进程共享 */
struct PoolEntry
{
    std::atomic<uint32_t> in_use;   // 0=free, 1=live
    int32_t pid;
    uint32_t kind;
    int64_t register_ts_ns;
    std::atomic<int64_t> heartbeat_ns;
    char topic_name[kMaxTopicName];
    char type_name[kMaxTypeName];
    int32_t domain_id;
    char extra[kMaxExtra];
};

#if defined(_WIN32)
/* Windows 上跨进程互斥量用具名 Win32 mutex（不放在 shm 里），
   header 仍保留同等大小的占位以保持 PoolEntry 数组的偏移稳定 */
struct PoolHeader
{
    std::atomic<uint32_t> init_state;
    uint32_t magic;
    uint32_t version;
    uint32_t max_entries;
    /* 占位：保留与 Linux 版 pthread_mutex_t 同等量级的空间，
       避免不同平台二进制布局差异过大；不被 Windows 实际使用 */
    char mtx_placeholder[64];
};
#else
struct PoolHeader
{
    std::atomic<uint32_t> init_state;
    uint32_t magic;
    uint32_t version;
    uint32_t max_entries;
    pthread_mutex_t mtx;
};
#endif

static_assert(ATOMIC_INT_LOCK_FREE == 2, "std::atomic<uint32_t> must be lock-free for cross-process sharing");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "std::atomic<int64_t> must be lock-free for cross-process sharing");
static_assert(std::is_trivially_copyable<PoolEntry>::value || std::is_standard_layout<PoolEntry>::value,
              "PoolEntry must be standard layout for shm mapping");

constexpr std::size_t kRegionSize = sizeof(PoolHeader) + sizeof(PoolEntry) * kMaxEntries;

int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int32_t current_pid() noexcept
{
#if defined(_WIN32)
    return static_cast<int32_t>(::GetCurrentProcessId());
#else
    return static_cast<int32_t>(::getpid());
#endif
}

bool pid_alive(int32_t pid) noexcept
{
    if (pid <= 0)
        return false;
#if defined(_WIN32)
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr)
    {
        /* ERROR_ACCESS_DENIED 表示进程存在但无权访问，视为存活 */
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD code = 0;
    bool alive = false;
    if (::GetExitCodeProcess(h, &code))
        alive = (code == STILL_ACTIVE);
    ::CloseHandle(h);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    /* EPERM 说明 pid 存在但无权限探测，视为存活 */
    return errno != ESRCH;
#endif
}

void copy_truncate(char* dst, std::size_t dst_cap, const std::string& src) noexcept
{
    if (dst == nullptr || dst_cap == 0)
        return;
    std::size_t n = std::min(src.size(), dst_cap - 1);
    if (n > 0)
        std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

#if defined(_WIN32)
/* Windows 跨进程互斥量句柄（每进程独立打开，指向同一具名内核对象） */
HANDLE g_named_mutex = nullptr;

bool ensure_named_mutex() noexcept
{
    if (g_named_mutex != nullptr)
        return true;
    /* CreateMutexA 在已存在时返回已有对象；持有者异常退出时其它等待者会得到
       WAIT_ABANDONED，等价于 pthread robust mutex 的 EOWNERDEAD 语义 */
    g_named_mutex = ::CreateMutexA(nullptr, FALSE, kMutexName);
    return g_named_mutex != nullptr;
}

int robust_lock_win() noexcept
{
    if (!ensure_named_mutex())
        return -1;
    DWORD r = ::WaitForSingleObject(g_named_mutex, INFINITE);
    /* WAIT_ABANDONED 表示前一个持有者崩溃但锁已转交给本调用者，可以继续 */
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
        return 0;
    return -1;
}

void robust_unlock_win() noexcept
{
    if (g_named_mutex != nullptr)
        ::ReleaseMutex(g_named_mutex);
}
#else
/* 跨进程 robust mutex 的加解锁辅助：
               - 若上一个持有者崩溃 (EOWNERDEAD)，先标记一致再使用 */
int robust_lock(pthread_mutex_t* m) noexcept
{
    int ret = ::pthread_mutex_lock(m);
    if (ret == EOWNERDEAD)
    {
        ::pthread_mutex_consistent(m);
        ret = 0;
    }
    return ret;
}

void robust_unlock(pthread_mutex_t* m) noexcept
{
    ::pthread_mutex_unlock(m);
}
#endif

/* 作用域锁：构造加锁，析构解锁 */
struct ScopedShmLock
{
    bool locked{false};
#if !defined(_WIN32)
    pthread_mutex_t* m{nullptr};
#endif

#if defined(_WIN32)
    explicit ScopedShmLock(PoolHeader* /*hdr*/)
    {
        if (robust_lock_win() == 0)
            locked = true;
    }

    ~ScopedShmLock()
    {
        if (locked)
            robust_unlock_win();
    }
#else
    explicit ScopedShmLock(PoolHeader* hdr)
        : m(hdr ? &hdr->mtx : nullptr)
    {
        if (m && robust_lock(m) == 0)
            locked = true;
    }

    ~ScopedShmLock()
    {
        if (locked)
            robust_unlock(m);
    }
#endif

    ScopedShmLock(const ScopedShmLock&) = delete;
    ScopedShmLock& operator=(const ScopedShmLock&) = delete;
};

}   // namespace

const char* get_type_from_kind(EntryKind kind) noexcept
{
    switch (kind)
    {
    case EntryKind::ShmPub:
    case EntryKind::ShmSub:
        return "shm_pubsub";
    case EntryKind::ShmServer:
    case EntryKind::ShmClient:
        return "shm_sercli";
    case EntryKind::SocketPub:
    case EntryKind::SocketSub:
        return "socket_pubsub";
    case EntryKind::SocketServer:
    case EntryKind::SocketClient:
        return "socket_sercli";
    case EntryKind::Unknown:
    default:
        return "invalid EntryKind";
    }
}

const char* to_string(EntryKind kind) noexcept
{
    switch (kind)
    {
    case EntryKind::ShmPub:
        return "shm_pub";
    case EntryKind::ShmSub:
        return "shm_sub";
    case EntryKind::SocketPub:
        return "socket_pub";
    case EntryKind::SocketSub:
        return "socket_sub";
    case EntryKind::ShmServer:
        return "shm_server";
    case EntryKind::ShmClient:
        return "shm_client";
    case EntryKind::SocketServer:
        return "socket_server";
    case EntryKind::SocketClient:
        return "socket_client";
    case EntryKind::Unknown:
    default:
        return "unknown";
    }
}

std::string demangle(const char* mangled)
{
    if (mangled == nullptr)
        return {};
#if defined(_WIN32)
    /* MSVC 的 typeid(T).name() 已经返回人类可读形式（如 "class Foo"），
       直接原样返回即可 */
    return std::string(mangled);
#else
    int status = 0;
    char* buf = ::abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::string out = (status == 0 && buf != nullptr) ? std::string(buf) : std::string(mangled);
    if (buf != nullptr)
        std::free(buf);
    return out;
#endif
}

/***********************************************************************************/
/*                                     Impl                                        */
/***********************************************************************************/
struct IpcInfoPool::Impl
{
    ipc::shm::handle shm;
    PoolHeader* header{nullptr};
    PoolEntry* entries{nullptr};
    bool ready{false};

    Impl()
    {
        if (!shm.acquire(kShmName, kRegionSize, ipc::shm::create | ipc::shm::open))
        {
            return;
        }
        auto* base = static_cast<uint8_t*>(shm.get());
        if (base == nullptr)
            return;

        header = reinterpret_cast<PoolHeader*>(base);
        entries = reinterpret_cast<PoolEntry*>(base + sizeof(PoolHeader));

#if defined(_WIN32)
        /* Windows：每个进程都需要打开同名内核 mutex */
        if (!ensure_named_mutex())
            return;
#endif

        /* 首个进程完成 header 初始化，后续进程自旋等待就绪 */
        uint32_t expect = kInitUninit;
        if (header->init_state.compare_exchange_strong(expect, kInitInProgress, std::memory_order_acq_rel))
        {
            header->magic = kMagic;
            header->version = kVersion;
            header->max_entries = static_cast<uint32_t>(kMaxEntries);

#if !defined(_WIN32)
            pthread_mutexattr_t attr;
            ::pthread_mutexattr_init(&attr);
            ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
            ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
            ::pthread_mutex_init(&header->mtx, &attr);
            ::pthread_mutexattr_destroy(&attr);
#endif

            for (std::size_t i = 0; i < kMaxEntries; ++i)
            {
                entries[i].in_use.store(0, std::memory_order_relaxed);
            }

            header->init_state.store(kInitReady, std::memory_order_release);
        }
        else
        {
            /* 等待别的进程完成初始化，最多等 ~2 秒 */
            for (int i = 0; i < 2'000; ++i)
            {
                if (header->init_state.load(std::memory_order_acquire) == kInitReady)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (header->init_state.load(std::memory_order_acquire) != kInitReady || header->magic != kMagic)
                return;
        }
        ready = true;
    }

    bool ok() const noexcept { return ready && header && entries; }
};

/***********************************************************************************/
IpcInfoPool& IpcInfoPool::instance()
{
    static IpcInfoPool pool;
    return pool;
}

IpcInfoPool::IpcInfoPool()
    : impl_(new Impl())
{}

IpcInfoPool::~IpcInfoPool() = default;

int32_t IpcInfoPool::register_entry(const RegisterInfo& info)
{
    if (!impl_ || !impl_->ok())
        return -1;

    ScopedShmLock lock(impl_->header);
    if (!lock.locked)
        return -1;

    for (std::size_t i = 0; i < kMaxEntries; ++i)
    {
        PoolEntry& e = impl_->entries[i];
        if (e.in_use.load(std::memory_order_relaxed) != 0)
            continue;

        e.pid = current_pid();
        e.kind = static_cast<uint32_t>(info.kind);
        e.register_ts_ns = now_ns();
        e.heartbeat_ns.store(e.register_ts_ns, std::memory_order_relaxed);
        copy_truncate(e.topic_name, kMaxTopicName, info.topic_name);
        copy_truncate(e.type_name, kMaxTypeName, info.type_name);
        e.domain_id = info.domain_id;
        copy_truncate(e.extra, kMaxExtra, info.extra);
        e.in_use.store(1, std::memory_order_release);
        return static_cast<int32_t>(i);
    }
    return -1;
}

void IpcInfoPool::unregister_entry(int32_t slot)
{
    if (slot < 0 || static_cast<std::size_t>(slot) >= kMaxEntries)
        return;
    if (!impl_ || !impl_->ok())
        return;

    ScopedShmLock lock(impl_->header);
    if (!lock.locked)
        return;

    PoolEntry& e = impl_->entries[slot];
    if (e.in_use.load(std::memory_order_acquire) == 0)
        return;
    e.pid = 0;
    e.kind = 0;
    e.register_ts_ns = 0;
    e.heartbeat_ns.store(0, std::memory_order_relaxed);
    e.topic_name[0] = '\0';
    e.type_name[0] = '\0';
    e.domain_id = 0;
    e.extra[0] = '\0';
    e.in_use.store(0, std::memory_order_release);
}

void IpcInfoPool::heartbeat(int32_t slot)
{
    if (slot < 0 || static_cast<std::size_t>(slot) >= kMaxEntries)
        return;
    if (!impl_ || !impl_->ok())
        return;

    PoolEntry& e = impl_->entries[slot];
    if (e.in_use.load(std::memory_order_acquire) == 0)
        return;
    e.heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
}

std::vector<EntrySnapshot> IpcInfoPool::snapshot(bool gc_dead_flag)
{
    std::vector<EntrySnapshot> out;
    if (!impl_ || !impl_->ok())
        return out;

    /* 先在锁内拷贝为 POD 数组，解锁后再转成 std::string，避免锁内堆分配 */
    struct RawCopy
    {
        int32_t slot;
        EntryKind kind;
        int32_t pid;
        int64_t register_ts_ns;
        int64_t heartbeat_ns;
        char topic_name[kMaxTopicName];
        char type_name[kMaxTypeName];
        int32_t domain_id;
        char extra[kMaxExtra];
        bool in_use;
        bool alive;
    };

    std::vector<RawCopy> raws;
    raws.reserve(32);

    {
        ScopedShmLock lock(impl_->header);
        if (!lock.locked)
            return out;

        for (std::size_t i = 0; i < kMaxEntries; ++i)
        {
            PoolEntry& e = impl_->entries[i];
            if (e.in_use.load(std::memory_order_acquire) == 0)
                continue;

            bool alive = pid_alive(e.pid);
            bool in_use = e.in_use.load(std::memory_order_acquire) != 0;
            if (!alive && gc_dead_flag)
            {
                e.pid = 0;
                e.kind = 0;
                e.register_ts_ns = 0;
                e.heartbeat_ns.store(0, std::memory_order_relaxed);
                e.topic_name[0] = '\0';
                e.type_name[0] = '\0';
                e.domain_id = 0;
                e.extra[0] = '\0';
                e.in_use.store(0, std::memory_order_release);
                continue;
            }

            RawCopy r{};
            r.slot = static_cast<int32_t>(i);
            r.kind = static_cast<EntryKind>(e.kind);
            r.pid = e.pid;
            r.register_ts_ns = e.register_ts_ns;
            r.heartbeat_ns = e.heartbeat_ns.load(std::memory_order_relaxed);
            std::memcpy(r.topic_name, e.topic_name, kMaxTopicName);
            std::memcpy(r.type_name, e.type_name, kMaxTypeName);
            r.domain_id = e.domain_id;
            std::memcpy(r.extra, e.extra, kMaxExtra);
            r.in_use = in_use;
            r.alive = alive;
            raws.push_back(r);
        }
    }

    out.reserve(raws.size());
    for (const auto& r : raws)
    {
        EntrySnapshot s;
        s.slot = r.slot;
        s.kind = r.kind;
        s.pid = r.pid;
        s.register_ts_ns = r.register_ts_ns;
        s.heartbeat_ns = r.heartbeat_ns;
        s.topic_name = r.topic_name;
        s.type_name = r.type_name;
        s.domain_id = r.domain_id;
        s.extra = r.extra;
        s.in_use = r.in_use;
        s.alive = r.alive;
        out.push_back(std::move(s));
    }
    return out;
}

std::size_t IpcInfoPool::gc_dead()
{
    if (!impl_ || !impl_->ok())
        return 0;

    ScopedShmLock lock(impl_->header);
    if (!lock.locked)
        return 0;

    std::size_t n = 0;
    for (std::size_t i = 0; i < kMaxEntries; ++i)
    {
        PoolEntry& e = impl_->entries[i];
        if (e.in_use.load(std::memory_order_acquire) == 0)
            continue;
        if (pid_alive(e.pid))
            continue;
        e.pid = 0;
        e.kind = 0;
        e.register_ts_ns = 0;
        e.heartbeat_ns.store(0, std::memory_order_relaxed);
        e.topic_name[0] = '\0';
        e.type_name[0] = '\0';
        e.domain_id = 0;
        e.extra[0] = '\0';
        e.in_use.store(0, std::memory_order_release);
        ++n;
    }
    return n;
}

void IpcInfoPool::reset_storage()
{
    ipc::shm::handle::clear_storage(kShmName);
}

/***********************************************************************************/
/*                              ScopedRegistration                                 */
/***********************************************************************************/
ScopedRegistration::ScopedRegistration(const RegisterInfo& info)
{
    slot_ = IpcInfoPool::instance().register_entry(info);
}

ScopedRegistration::ScopedRegistration(ScopedRegistration&& other) noexcept
    : slot_(other.slot_)
{
    other.slot_ = -1;
}

ScopedRegistration& ScopedRegistration::operator=(ScopedRegistration&& other) noexcept
{
    if (this != &other)
    {
        reset();
        slot_ = other.slot_;
        other.slot_ = -1;
    }
    return *this;
}

ScopedRegistration::~ScopedRegistration()
{
    reset();
}

void ScopedRegistration::rebind(const RegisterInfo& info)
{
    reset();
    slot_ = IpcInfoPool::instance().register_entry(info);
}

void ScopedRegistration::reset()
{
    if (slot_ >= 0)
    {
        IpcInfoPool::instance().unregister_entry(slot_);
        slot_ = -1;
    }
}

void ScopedRegistration::heartbeat()
{
    if (slot_ >= 0)
        IpcInfoPool::instance().heartbeat(slot_);
}

}   // namespace info_pool
}   // namespace dzIPC
