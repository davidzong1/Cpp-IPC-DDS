#include "dzIPC/dzipc.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace dzIPC {

namespace {
std::atomic<bool> shutdown_requested{false};
std::once_flag shutdown_once;
std::thread shutdown_thread;
std::atomic<bool> shutdown_monitor_started{false};

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
    if (!shutdown_monitor_started.exchange(true))
    {
        StartShutdownMonitor();
    }
}

void SignalHandler(int)
{
    shutdown_requested.store(true, std::memory_order_relaxed);
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

void StartShutdownMonitor()
{
    std::call_once(shutdown_once,
                   []()
                   {
                       std::signal(SIGINT, SignalHandler);
                       std::signal(SIGTERM, SignalHandler);
                       shutdown_thread = std::thread(
                           []()
                           {
                               while (!shutdown_requested.load(std::memory_order_relaxed))
                               {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(100));
                               }
                               CleanupIpcInstances();
                               std::exit(0);
                           });
                       shutdown_thread.detach();
                   });
}

void RequestShutdown()
{
    shutdown_requested.store(true, std::memory_order_relaxed);
}

bool IsShutdownRequested()
{
    return shutdown_requested.load(std::memory_order_relaxed);
}

ServerIPCPtr ServerIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                              ServerCallBackFun callback, size_t domain_id, IPCType ipc_type, bool verbose,
                              bool enable_thread_qos, int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();
    auto ptr = std::make_shared<dzIPC::pimpl::server_ipc_impl>(topic_name_, msg, callback, domain_id, ipc_type, verbose,
                                                               enable_thread_qos, cpu_id, thread_priority);
    RegisterIpcInstance(server_ipc_instances, server_ipc_instance_mutex, ptr);
    return ptr;
}

ClientIPCPtr ClientIPCPtrMake(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                              IPCType ipc_type, bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
{
    EnsureShutdownMonitorStarted();
    auto ptr = std::make_shared<dzIPC::pimpl::client_ipc_impl>(topic_name_, msg, domain_id, ipc_type, verbose,
                                                               enable_thread_qos, cpu_id, thread_priority);
    RegisterIpcInstance(client_ipc_instances, client_ipc_instance_mutex, ptr);
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
    return ptr;
}

}   // namespace dzIPC
