#include "dzIPC/dzipc.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dzIPC {

namespace {
std::atomic<bool> shutdown_requested{false};
std::once_flag shutdown_once;
std::thread shutdown_thread;
std::atomic<bool> shutdown_monitor_started{false};
/* UF-004 opt-out: 应用在**首次 IPC 构造之前**调用 DisableShutdownMonitor() 后置位。
 * 只被 EnsureShutdownMonitorStarted() 读, 且**只在那一处** —— 置位后恒不再隐式安装。
 * 刻意不提供反向开关(运行中翻转会得到"处理器装了、线程没起"这类不可判状态)。 */
std::atomic<bool> shutdown_monitor_disabled{false};

/* ---- UF-009 链式接管(2026-09-18) ----
 * 缺陷: 库的监控用 std::signal 直接覆盖应用处理器, 信号后走 std::exit(0) 不展开栈
 * ⇒ main 里的 IPC 实例 shared_ptr 不析构 ⇒ SHM 段不 unlink ⇒ factory 路径恒 17 段残留,
 * 应用自己的收尾(如 SERVER SUMMARY)永不执行(docs/unfixed_defects.md 0.3.4/0.3.5)。
 *
 * 修法(有条件): 保存既有处置; 信号到来时**只回放**收到的那个——先调既有处理器,
 * 再**复权**(应用处理器装回、库处理器退出), 然后**宽限等待**应用自行退出:
 * 优雅路径上栈正常展开, 段被析构 unlink, 应用收尾恢复执行; 超时(500ms)才走原库收尾
 * ——该路径栈不展开, 残留语义与改前一致(如实登记, 不扩权)。
 * 回放跳过 SIG_DFL/SIG_IGN: 回放 SIG_DFL 会把"应用从未装处理器"的默认路径
 * (UF-004 验收臂 2)退化成信号硬杀, "默认路径逐位不变"即被打破; 调 SIG_IGN 则是 SEGV。
 * 已知边界: 同进程内 SIGINT、SIGTERM 接连到来的链式序列, 第二个信号可能赶不上回放
 * (首个已复权后处置已归应用, 第二个信号本就归应用处理器, 只有"复权完成前到达"
 * 这一窗口例外); 单信号场景(验收矩阵与真实应用的主流形态)不受影响。 */
using prev_handler_t = void (*)(int);
std::atomic<prev_handler_t> prev_sigint_handler{nullptr};
std::atomic<prev_handler_t> prev_sigterm_handler{nullptr};
std::atomic<int> uf009_last_signal{0};
std::atomic<bool> uf009_signal_dispatched{false};
/* 宽限窗口: 驱动/典型应用从置位到 return main 实测 20~40ms(0.3.4 corroboration),
 * 500ms 留 10 倍裕量; 超时即认为"应用不会自行退出"(无处理器/no-op/太慢), 走原库收尾。 */
constexpr std::chrono::milliseconds kUf009GraceInterval{500};

template<typename T>
void RegisterIpcInstance(std::vector<std::weak_ptr<T>>& instances, std::mutex& mutex, const std::shared_ptr<T>& ptr)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = instances.begin();
    while (it != instances.end())
    {
        if (it->expired())
        {
            it = instances.erase(it);
        }
        else
        {
            ++it;
        }
    }
    instances.emplace_back(ptr);
}

void EnsureShutdownMonitorStarted()
{
    /* UF-004 opt-out: 判定必须在 exchange(true) **之前**, 且**不能**搬进
     * StartShutdownMonitor() 的 std::call_once —— 提前 return 会被 call_once 记成
     * "已执行完毕", once_flag 随即被**消费**, 于是之后**任何**显式
     * StartShutdownMonitor()(公开 API + Python 绑定)都变成永久静默 no-op。
     * 在这里提前返回则两个标志都不动: started 保持 false(没启动就是没启动),
     * call_once 完整留给显式调用。 */
    if (shutdown_monitor_disabled.load(std::memory_order_relaxed))
    {
        return;
    }
    if (!shutdown_monitor_started.exchange(true))
    {
        StartShutdownMonitor();
    }
}

void SignalHandler(int signo)
{
    shutdown_requested.store(true, std::memory_order_relaxed);
    /* UF-009: 记录实际收到的信号(监控线程据此回放/复权)。处理器内只碰无锁原子量。 */
    uf009_last_signal.store(signo, std::memory_order_relaxed);
    uf009_signal_dispatched.store(true, std::memory_order_relaxed);
}

void RecordEndpointMetaNoThrow()
{
    if (!logger::IsDzipcLogRunning()) return;
    try {
        logger::RecordEndpointMeta();
    } catch (...) {
        // Endpoint logging is diagnostic and must not break IPC construction.
    }
}
}   // namespace

std::mutex server_ipc_instance_mutex;                                                      // 保护IPC实例容器的互斥锁
std::mutex client_ipc_instance_mutex;                                                      // 保护IPC实例容器的互斥锁
std::mutex publisher_ipc_instance_mutex;                                                   // 保护IPC实例容器
std::mutex subscriber_ipc_instance_mutex;                                                  // 保护IPC实例容器
std::vector<std::weak_ptr<dzIPC::pimpl::server_ipc_impl>> server_ipc_instances;   // 存储服务端实例的全局容器
std::vector<std::weak_ptr<dzIPC::pimpl::client_ipc_impl>> client_ipc_instances;   // 存储客户端实例的全局容器
std::vector<std::weak_ptr<dzIPC::pimpl::publisher_ipc_impl>> publisher_ipc_instances;   // 存储发布者实例的全局容器
std::vector<std::weak_ptr<dzIPC::pimpl::subscriber_ipc_impl>>
    subscriber_ipc_instances;   // 存储订阅者实例的全局容器

static void CleanupIpcInstances()
{
    {
        std::lock_guard<std::mutex> lock(server_ipc_instance_mutex);
        server_ipc_instances.clear();
    }
    {
        std::lock_guard<std::mutex> lock(client_ipc_instance_mutex);
        client_ipc_instances.clear();
    }
    {
        std::lock_guard<std::mutex> lock(publisher_ipc_instance_mutex);
        publisher_ipc_instances.clear();
    }
    {
        std::lock_guard<std::mutex> lock(subscriber_ipc_instance_mutex);
        subscriber_ipc_instances.clear();
    }
}

static void ShutdownMonitorThreadBody();

void StartShutdownMonitor()
{
    std::call_once(shutdown_once,
                   []()
                   {
                       /* UF-009 链式接管第一步: 先保存既有处置, 再装库处理器。
                        * 保存的是 sa_handler 原值(SIG_DFL/SIG_IGN/真处理器三类),
                        * 复权时按原值恢复(见 ShutdownMonitorThreadBody)。 */
                       struct sigaction old;
                       std::memset(&old, 0, sizeof(old));
                       if (::sigaction(SIGINT, nullptr, &old) == 0)
                           prev_sigint_handler.store(old.sa_handler, std::memory_order_relaxed);
                       if (::sigaction(SIGTERM, nullptr, &old) == 0)
                           prev_sigterm_handler.store(old.sa_handler, std::memory_order_relaxed);
                       std::signal(SIGINT, SignalHandler);
                       std::signal(SIGTERM, SignalHandler);
                       shutdown_thread = std::thread(ShutdownMonitorThreadBody);
                       shutdown_thread.detach();
                   });
}

void ShutdownMonitorThreadBody()
{
    while (!shutdown_requested.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!uf009_signal_dispatched.load(std::memory_order_relaxed))
    {
        /* 库内部请求的退出(RequestShutdown): 不回放不宽限, 行为与改前一致。 */
        dzIPC::logger::StopDzipcLog();
        CleanupIpcInstances();
        std::exit(0);
    }

    /* ---- UF-009: 信号驱动 —— 回放既有处理器, 再复权 ----
     * 以下全部运行在普通线程上下文(非信号上下文), 可以安全做 sigaction/调用应用处理器;
     * StopDzipcLog 同样刻意留在这里: 锁与文件 I/O 不是信号安全的。 */
    const int signo = uf009_last_signal.load(std::memory_order_relaxed);
    prev_handler_t prev = nullptr;
    if (signo == SIGINT)
        prev = prev_sigint_handler.load(std::memory_order_relaxed);
    else if (signo == SIGTERM)
        prev = prev_sigterm_handler.load(std::memory_order_relaxed);
    if (prev != nullptr && prev != SIG_IGN)
    {
        prev(signo);
    }

    /* 复权: 应用处理器(或 SIG_DFL/SIG_IGN)装回, 库处理器退出。
     * 此时 signo 的当前处置仍是库处理器, sigaction 查询可原样取回 sa_flags/mask,
     * 只把 handler 换回保存值, 其余字段保留。SIG_DFL 保存值为空指针, 走 signal(SIG_DFL)。 */
    struct sigaction cur;
    std::memset(&cur, 0, sizeof(cur));
    if (::sigaction(signo, nullptr, &cur) == 0 && prev != nullptr)
    {
        cur.sa_handler = prev;
        ::sigaction(signo, &cur, nullptr);
    }
    else
    {
        ::signal(signo, SIG_DFL);
    }

    /* ---- 宽限: 给应用自行退出的机会 ----
     * 优雅路径上 main return ⇒ 栈展开 ⇒ IPC 实例析构 ⇒ 段 unlink, 应用收尾恢复执行;
     * deadline 在监控线程里取(不在信号处理器里做时钟调用), 实际窗口 ≤ 500ms+100ms 轮询抖动。 */
    const auto deadline = std::chrono::steady_clock::now() + kUf009GraceInterval;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* 超时: 原库收尾。⛔ 栈不展开 ⇒ main 里的实例不析构 ⇒ 段不 unlink ——
     * 残留语义与改前一致, 这是"有条件修复"的保留面, 不在此处强拆实例。 */
    dzIPC::logger::StopDzipcLog();
    CleanupIpcInstances();
    std::exit(0);
}

void RequestShutdown()
{
    shutdown_requested.store(true, std::memory_order_relaxed);
}

bool IsShutdownRequested()
{
    return shutdown_requested.load(std::memory_order_relaxed);
}

bool DisableShutdownMonitor() noexcept
{
    /* 只对"首次 IPC 构造之前"有效: 监控一旦启动就不可撤销(信号处理器已装、线程已在跑),
     * 此时如实返回 false 表示"来不及", 并且**不改任何行为**。 */
    if (shutdown_monitor_started.load(std::memory_order_acquire))
    {
        return false;
    }
    shutdown_monitor_disabled.store(true, std::memory_order_release);
    /* 与"并发中的首次构造"竞争时以对方为准: 若它已抢在 store 之前 exchange 成功,
     * 库其实已被接管 ⇒ 撤销请求并返回 false。单线程"先调后用"的用法不受影响。 */
    if (shutdown_monitor_started.load(std::memory_order_acquire))
    {
        shutdown_monitor_disabled.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

ServerIPCPtr ServerIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                              ServerCallBackFun callback, size_t domain_id, IPCType ipc_type, bool verbose,
                              bool enable_thread_qos, int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();

    auto ptr = std::make_shared<dzIPC::pimpl::server_ipc_impl>(topic_name_, msg, std::move(callback), domain_id, ipc_type, verbose,
                                                               enable_thread_qos, cpu_id, thread_priority);
    RegisterIpcInstance(server_ipc_instances, server_ipc_instance_mutex, ptr);
    RecordEndpointMetaNoThrow();
    return ptr;
}

ClientIPCPtr ClientIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                              IPCType ipc_type, bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();
    auto ptr = std::make_shared<dzIPC::pimpl::client_ipc_impl>(topic_name_, msg, domain_id, ipc_type, verbose,
                                                               enable_thread_qos, cpu_id, thread_priority);
    RegisterIpcInstance(client_ipc_instances, client_ipc_instance_mutex, ptr);
    RecordEndpointMetaNoThrow();
    return ptr;
}

PublisherIPCPtr PublisherIPCPtrMake(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                    size_t domain_id, IPCType ipc_type, bool verbose, bool enable_thread_qos,
                                    int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();
    auto ptr = std::make_shared<dzIPC::pimpl::publisher_ipc_impl>(msg, topic_name, domain_id, ipc_type, verbose,
                                                                  enable_thread_qos, cpu_id, thread_priority);
    RegisterIpcInstance(publisher_ipc_instances, publisher_ipc_instance_mutex, ptr);
    RecordEndpointMetaNoThrow();
    return ptr;
}

SubscriberIPCPtr SubscriberIPCPtrMake(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                      size_t domain_id, const size_t queue_size, IPCType ipc_type, bool verbose,
                                      bool enable_thread_qos, int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();
    auto ptr = std::make_shared<dzIPC::pimpl::subscriber_ipc_impl>(msg, topic_name, domain_id, queue_size, ipc_type,
                                                                   verbose, enable_thread_qos, cpu_id,
                                                                   thread_priority);
    RegisterIpcInstance(subscriber_ipc_instances, subscriber_ipc_instance_mutex, ptr);
    RecordEndpointMetaNoThrow();
    return ptr;
}

}   // namespace dzIPC
