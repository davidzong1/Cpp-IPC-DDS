#include "dzIPC/threepools/socket_wait_set.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libipc/utility/log.h"

#if defined(__linux__)
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

namespace dzIPC {
namespace threepools {

namespace {

/* Linux 侧策略上限：内核 epoll 对路数没有硬限制，但一个 worker 拖上千条通道
 * 时单次 wait 的扫描成本、以及 ready 数组的内存都会线性增长。给一个远高于
 * 单进程 socket 话题数的值，超过它由上层（阶段 5 的分配规则）把通道分到别的
 * worker —— 而不是在这里静默扩容。
 *
 * Windows 侧是硬限制：WaitForMultipleObjects 上限 64，内部唤醒事件占 1。 */
#if defined(_WIN32)
constexpr std::size_t kMaxChannels = 63;
#else
constexpr std::size_t kMaxChannels = 4096;
#endif

#if defined(__linux__)
bool probe_backend() noexcept
{
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

const char* kBackendName = "epoll";
#elif defined(_WIN32)
bool probe_backend() noexcept { return true; }
const char* kBackendName = "WaitForMultipleObjects";
#else
bool probe_backend() noexcept { return false; }
const char* kBackendName = "none";
#endif

void report_unavailable_once() noexcept
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
        ipc::log("SocketWaitSet backend unavailable; keep one receive thread per socket channel\n");
}

}   // namespace

bool SocketWaitSet::backend_available() noexcept
{
    static const bool supported = probe_backend();
    return supported;
}

const char* SocketWaitSet::backend_name() noexcept
{
    return backend_available() ? kBackendName : "none";
}

std::size_t SocketWaitSet::max_channels() noexcept
{
    return backend_available() ? kMaxChannels : 0;
}

struct SocketWaitSet::Impl
{
    struct Entry
    {
        SocketWaitToken token;
        /* 注册代际。**必须**用代际而不是 handle 作为内核事件的载荷：fd 号会被
         * 复用（实测 close 后新 socket 拿到同一个 fd），若把 fd 直接写进
         * epoll_event.data，一个已经 remove 但仍在用户态 ready 数组里的旧事件
         * 会与"复用同一 fd 号的新 token"匹配上 ⇒ 新通道被凭空报告就绪。
         * 代际单调递增、永不复用，旧事件找不到对应 entry 即被丢弃。 */
        std::uint64_t epoch{0};
    };

    mutable std::mutex mtx;
    std::vector<Entry> entries;
    std::vector<SocketWaitToken> ready;
#if defined(__linux__)
    std::unordered_map<std::uint64_t, SocketWaitToken> tokens_by_epoch;
    std::unordered_set<std::uint64_t> ready_epochs;
#endif
    std::uint64_t next_epoch{1};
    bool stopped{false};
    /* 当前在 wait() 内部的线程数（含"已声明有等待者、尚未进 epoll_wait"的窗口）。
     * remove/stop 只在 >0 时敲唤醒通道：没有等待者时敲，计数会留在 eventfd 里，
     * 让**下一次** wait 无谓地返回 true（一次虚假唤醒）。有了这个计数，
     * "remove 唤醒残留"与"fd 复用导致旧事件算到新通道"就能被区分开 ——
     * 后者是缺陷，前者不是。 */
    std::size_t waiters{0};

#if defined(__linux__)
    int epfd{-1};
    /* remove/stop 的独立唤醒通道。**不能**靠 epoll_ctl(DEL) 唤醒：本机实测
     * （Linux 6.8）DEL 一个正在被 epoll_wait 等待的 fd，epoll_wait 会一直睡到
     * 超时（返回 0），并不返回。所以注销协议的第 2 步必须显式敲 eventfd。 */
    int wakefd{-1};

    ~Impl()
    {
        if (wakefd >= 0) ::close(wakefd);
        if (epfd >= 0) ::close(epfd);
    }
#elif defined(_WIN32)
    HANDLE wake_event{nullptr};

    ~Impl()
    {
        if (wake_event != nullptr) ::CloseHandle(wake_event);
    }
#endif
};

SocketWaitSet::SocketWaitSet()
    : impl_(new Impl)
{
#if defined(__linux__)
    if (backend_available())
    {
        impl_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
        impl_->wakefd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl_->epfd >= 0 && impl_->wakefd >= 0)
        {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = 0;   // 0 不是合法代际（next_epoch 从 1 起），永不匹配 entry
            if (::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, impl_->wakefd, &ev) != 0)
            {
                ::close(impl_->wakefd);
                impl_->wakefd = -1;
            }
        }
        if (impl_->epfd < 0 || impl_->wakefd < 0)
        {
            if (impl_->wakefd >= 0) { ::close(impl_->wakefd); impl_->wakefd = -1; }
            if (impl_->epfd >= 0) { ::close(impl_->epfd); impl_->epfd = -1; }
        }
    }
#elif defined(_WIN32)
    impl_->wake_event = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
#endif
}

SocketWaitSet::~SocketWaitSet()
{
    stop();
    delete impl_;
    impl_ = nullptr;
}

bool SocketWaitSet::add(const SocketWaitToken& token)
{
    if (!impl_ || !backend_available())
    {
        report_unavailable_once();
        return false;
    }
    /* 无效 token（owner/handle 任一为空）是调用方参数错，不是后端不可用 ——
     * 若走上面的分支会打出"backend unavailable"的误导日志。 */
    if (!token.valid()) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->stopped) return false;
    for (const auto& entry : impl_->entries)
    {
        if (entry.token == token) return true;   // 幂等
        /* 同 owner 换 handle、或不同 owner 同 handle：都拒绝。
         * 前者说明宿主在没 remove 的情况下换了底层 socket（漏了注销协议）；
         * 后者会让"事件 → token"的映射二义，必须显式拒绝而不是覆盖。 */
        if (entry.token.owner == token.owner || entry.token.handle == token.handle)
            return false;
    }
    if (impl_->entries.size() >= kMaxChannels) return false;

    const std::uint64_t epoch = impl_->next_epoch++;
#if defined(__linux__)
    if (impl_->epfd < 0) return false;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = epoch;
    if (::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, static_cast<int>(token.handle), &ev) != 0)
    {
        ipc::error("SocketWaitSet epoll_ctl(ADD) failed for handle %llu: %d\n",
                   static_cast<unsigned long long>(token.handle), errno);
        return false;
    }
#elif defined(_WIN32)
    if (impl_->wake_event == nullptr) return false;
#endif
    try
    {
        impl_->entries.push_back(Impl::Entry{token, epoch});
#if defined(__linux__)
        impl_->tokens_by_epoch.emplace(epoch, token);
#endif
    }
    catch (...)
    {
#if defined(__linux__)
        if (!impl_->entries.empty() && impl_->entries.back().epoch == epoch)
            impl_->entries.pop_back();
#endif
#if defined(__linux__)
        ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, static_cast<int>(token.handle), nullptr);
#endif
        throw;
    }
    return true;
}

bool SocketWaitSet::remove(const SocketWaitToken& token)
{
    if (!impl_) return true;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    const auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                 [&](const Impl::Entry& e) { return e.token == token; });
    if (it == impl_->entries.end()) return true;   // 幂等
#if defined(__linux__)
    /* 摘除必须显式 DEL：DEL 同步清除该 fd 在 ready list 里的残留（实测），
     * 这是"fd 复用不误触发"的第一道保证；代际是第二道。 */
    if (impl_->epfd >= 0)
        ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, static_cast<int>(it->token.handle), nullptr);
    impl_->tokens_by_epoch.erase(it->epoch);
    impl_->ready_epochs.erase(it->epoch);
#endif
    impl_->entries.erase(it);
    /* 契约：remove 返回后该 token 不得再出现在 consume_ready()。已经收进 ready
     * 缓存的那一份必须在这里摘掉 —— 否则 worker 会拿着一个已注销的 token 去
     * 访问宿主已经销毁的通道。 */
    impl_->ready.erase(std::remove(impl_->ready.begin(), impl_->ready.end(), token),
                       impl_->ready.end());
    /* 唤醒阻塞中的 wait（不能靠 DEL，见 Impl::wakefd 注释）。只在**确实有
     * 等待者**时敲：没有等待者时敲会留下一次虚假唤醒，让下一次 wait 无谓返回。 */
    const bool has_waiter = impl_->waiters > 0;
#if defined(__linux__)
    if (has_waiter && impl_->wakefd >= 0)
    {
        const std::uint64_t one = 1;
        const ssize_t n = ::write(impl_->wakefd, &one, sizeof(one));
        (void)n;   // EAGAIN 表示计数已饱和，等待方一定会被叫醒
    }
#elif defined(_WIN32)
    if (has_waiter && impl_->wake_event != nullptr) ::SetEvent(impl_->wake_event);
#endif
    return true;
}

bool SocketWaitSet::wait(std::chrono::milliseconds timeout)
{
    if (!impl_ || !backend_available())
    {
        report_unavailable_once();
        return false;
    }

#if defined(__linux__)
    if (impl_->epfd < 0 || impl_->wakefd < 0) return false;

    /* 在锁内取一份"当前在册代际"的快照大小，锁外 epoll_wait。epoll_wait 期间
     * 允许 add/remove 并发（它们都走 epoll_ctl，内核保证线程安全）；返回后
     * 再拿锁，按代际在当前 entries 中查找，查不到即丢弃 —— 这就同时覆盖了
     * "等待中被 remove"和"fd 被复用后旧事件残留"两种情形。 */
    std::array<epoll_event, 64> events{};
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        stopped = impl_->stopped;
        if (!stopped) impl_->waiters++;   // 必须在释放锁前声明，否则 remove 会漏唤醒
    }
    if (stopped) return true;
    /* 任何退出路径都要撤销"有等待者"，否则后续 remove 会写一个没人等的唤醒
     * 通道，留下一次虚假唤醒。析构顺序：函数尾部的 lock_guard 先析构，本
     * guard 后析构，因此这里重新拿锁不会自锁。 */
    struct WaiterGuard
    {
        Impl* impl;
        ~WaiterGuard()
        {
            std::lock_guard<std::mutex> lock(impl->mtx);
            --impl->waiters;
        }
    } waiter_guard{impl_};

    const int ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
    int n = 0;
    for (;;)
    {
        n = ::epoll_wait(impl_->epfd, events.data(), static_cast<int>(events.size()), ms);
        if (n >= 0) break;
        if (errno == EINTR)
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            if (impl_->stopped) return true;
            continue;   // 未 stop 则重试；上层 stop 时 wakefd 会让我们立刻返回
        }
        ipc::error("SocketWaitSet epoll_wait failed: %d\n", errno);
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mtx);
    bool woke = false;
    for (int i = 0; i < n; ++i)
    {
        const std::uint64_t epoch = events[static_cast<std::size_t>(i)].data.u64;
        if (epoch == 0)
        {
            /* 唤醒通道。清掉计数，避免下一次 wait 空转。 */
            std::uint64_t drained = 0;
            const ssize_t r = ::read(impl_->wakefd, &drained, sizeof(drained));
            (void)r;
            woke = true;
            continue;
        }
        const auto it = impl_->tokens_by_epoch.find(epoch);
        if (it == impl_->tokens_by_epoch.end()) continue;   // 已被 remove / fd 复用残留
        if (impl_->ready_epochs.insert(epoch).second)
            impl_->ready.push_back(it->second);
    }
    if (impl_->stopped) return true;
    /* 契约：true = 至少一路就绪，或被 remove/stop 唤醒。被唤醒但 ready 为空
     * 也要返回 true，让调用方去 consume_ready()（可能空集）并重评估路由表。 */
    return !impl_->ready.empty() || woke;
#elif defined(_WIN32)
    std::vector<HANDLE> handles;
    std::vector<SocketWaitToken> tokens;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->stopped) return true;
        if (impl_->wake_event == nullptr) return false;
        handles.reserve(impl_->entries.size() + 1);
        tokens.reserve(impl_->entries.size());
        handles.push_back(impl_->wake_event);
        for (const auto& entry : impl_->entries)
        {
            handles.push_back(reinterpret_cast<HANDLE>(entry.token.handle));
            tokens.push_back(entry.token);
        }
        impl_->waiters++;   // 容器构造完毕后再声明，异常路径不会泄漏计数
    }
    struct WaiterGuard
    {
        Impl* impl;
        ~WaiterGuard()
        {
            std::lock_guard<std::mutex> lock(impl->mtx);
            --impl->waiters;
        }
    } waiter_guard{impl_};
    const DWORD ms = timeout.count() < 0 ? INFINITE
                                        : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), MAXDWORD - 1));
    const DWORD rc = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, ms);
    if (rc == WAIT_TIMEOUT) return false;
    if (rc == WAIT_FAILED)
    {
        ipc::error("SocketWaitSet WaitForMultipleObjects failed: %lu\n",
                   static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->stopped) return true;
    if (rc == WAIT_OBJECT_0)
    {
        if (impl_->wake_event != nullptr) ::ResetEvent(impl_->wake_event);
        return true;
    }
    const DWORD index = rc - WAIT_OBJECT_0;
    if (index >= 1 && index <= tokens.size())
    {
        const SocketWaitToken& token = tokens[index - 1];
        /* 只认当前仍在册的 token：等待期间可能已被 remove。 */
        if (std::find_if(impl_->entries.begin(), impl_->entries.end(),
                         [&](const Impl::Entry& e) { return e.token == token; }) != impl_->entries.end())
        {
            if (std::find(impl_->ready.begin(), impl_->ready.end(), token) == impl_->ready.end())
                impl_->ready.push_back(token);
        }
        return true;
    }
    return true;
#else
    (void)timeout;
    return false;
#endif
}

std::vector<SocketWaitToken> SocketWaitSet::consume_ready()
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto result = std::move(impl_->ready);
    impl_->ready.clear();
#if defined(__linux__)
    impl_->ready_epochs.clear();
#endif
    return result;
}

void SocketWaitSet::stop() noexcept
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->stopped) return;
    impl_->stopped = true;
    impl_->ready.clear();
#if defined(__linux__)
    impl_->ready_epochs.clear();
#endif
    /* 同 remove：只在有等待者时敲，避免留下虚假唤醒（stop 之后 wait 会在锁内
     * 直接看到 stopped 并返回 true，不需要唤醒通道）。 */
    const bool has_waiter = impl_->waiters > 0;
#if defined(__linux__)
    if (has_waiter && impl_->wakefd >= 0)
    {
        const std::uint64_t one = 1;
        const ssize_t n = ::write(impl_->wakefd, &one, sizeof(one));
        (void)n;
    }
#elif defined(_WIN32)
    if (has_waiter && impl_->wake_event != nullptr) ::SetEvent(impl_->wake_event);
#endif
}

std::size_t SocketWaitSet::size() const noexcept
{
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->entries.size();
}

}   // namespace threepools
}   // namespace dzIPC
