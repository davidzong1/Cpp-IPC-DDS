#include "dzIPC/socket_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/name_operator.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "libipc/semaphore.h"
#define ListenerWaitTime 1'000   // 1 second
#define ServerRevTime 200        // 200 ms, allow large fragmented UDP payloads to complete

namespace dzIPC {
namespace socket {
enum class State {
    RunHS,
    StopHS
};
using namespace ipc;

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_ser_ipc::socket_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                               std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id_);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketSerOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
    callback_ = std::move(callback);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_ser_ipc::~socket_ser_ipc()
{
    running.store(false, std::memory_order_release);
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
        response_thread_ = nullptr;
    }
    /* handshake 线程也必须 join，否则析构返回后该线程仍会读取本对象的成员
       (running / topic_name_ / verbose_ 等)，造成 use-after-free。Windows
       上的堆分配器更激进地复用内存，命中崩溃的概率远高于 Linux */
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
        handshake_thread_ = nullptr;
    }
    if (ipc_r_ptr_)
    {
        ipc_r_ptr_->close();
    }
    if (ipc_w_ptr_)
    {
        ipc_w_ptr_->close();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::InitChannel(std::string extra_info)
{
    try
    {
        ipc_r_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_);
        ipc_w_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_ + 1);
        if (verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "SerInfo] Request initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
            std::cerr << "\033[32m[" << topic_name_ << "SerInfo] Response initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ + 1 << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        }
        while (!ipc_r_ptr_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "SerInfo] Failed to connect request UDP,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        while (!ipc_w_ptr_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "SerInfo] Failed to connect response UDP,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::shared_ptr<ServiceData> message_template;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            message_template = message_;
        }
        std::string request_type_name =
            (message_template && message_template->request())
                ? dzIPC::info_pool::demangle(typeid(*message_template->request()).name())
                : std::string{};
        request_type_name = extract_last_segment(request_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketServer, topic_name_, request_type_name, "socket",
                          static_cast<int32_t>(domain_id_), extra_info});
        response_thread_ = new std::thread(&socket_ser_ipc::response_thread_func, this);
        dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                    topic_name_ + "_SocketSerResponseThread");
        handshake_thread_ = new std::thread(&socket_ser_ipc::server_handshake, this);
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_ser_ipc::server_handshake()
{
    uint64_t handshake_timeout_ms = 100;   // 100 ms
    ipc::socket::UDPNode ser_hs(topic_name_.c_str(), ipaddr_.c_str(), port_hash_ + 2);
    std::shared_ptr<IpcPubSubIdInitMsg> hs_msg = std::make_shared<IpcPubSubIdInitMsg>();
    while (!ser_hs.connect())
    {
        std::cerr << "\033[31m[" << topic_name_
                  << "SerInfo] Failed to connect handshake UDP,reconnect affter 1 second...\033[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    hs_msg->host_flag = true;
    ipc::buffer hs_buf;
    hs_buf = std::move(hs_msg->serialize());
    State st = State::RunHS;
    while (running.load(std::memory_order_acquire))
    {
        if (st == State::RunHS)
        {
            if (!ser_hs.send(hs_buf))
            {
                std::cerr << "\033[31m[" << topic_name_
                          << "SerInfo] Failed to send handshake message,retry affter 1 second...\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            ipc::buffer rev_buf = ser_hs.receive(handshake_timeout_ms);
            if (rev_buf.empty() || !hs_msg->check_cli(rev_buf)
                || !hs_msg->check_run_status(rev_buf))   // 等待客户端回应，完成握手
            {
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "SerInfo] Handshake with client completed for topic: " << topic_name_ << "\033[0m"
                              << std::endl;
                }
                handshake_completed_.store(true, std::memory_order_release);
                st = State::StopHS;
                continue;
            }
        }
        else
        {
            ipc::buffer rev_buf = ser_hs.receive(handshake_timeout_ms);
            if (rev_buf.empty() || !hs_msg->check_cli(rev_buf)
                || hs_msg->check_run_status(rev_buf))   // 握手完成后继续监听客户端状态，直到客户端断开连接
            {
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "SerInfo] Client disconnected, restarting handshake for topic: " << topic_name_
                              << "\033[0m" << std::endl;
                }
                handshake_completed_.store(false, std::memory_order_release);
                st = State::RunHS;
                continue;
            }
        }
    }
    hs_msg->run_status = false;   // 客户端主动断开连接时通知服务端
    hs_buf = std::move(hs_msg->serialize());
    ser_hs.send(hs_buf);
    ser_hs.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::response_thread_func()
{
    while (running.load(std::memory_order_acquire))
    {
        /* 服务端等待请求,超时跳过 */
        std::shared_ptr<ServiceData> local_msg;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            if (!message_)
            {
                continue;
            }
            local_msg.reset(message_->clone());
        }
        if (!chunk_rev_server(ipc_r_ptr_, local_msg, ServerRevTime, true))
        {
            continue;
        }
        std::function<void(std::shared_ptr<ServiceData>&)> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mtx_);
            callback = callback_;
        }
        if (callback)
        {
            callback(local_msg);
        }
        ipc::buffer response_data(std::move(local_msg->response()->serialize()));
        if (!chunk_send(ipc_w_ptr_, response_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Error sending response: Failed to send"
                      << "\033[0m" << std::endl;
        }
    }   // 客户段发送请求
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_cli_ipc::socket_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name_, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name_);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketCliOwnerThread");
}

socket_cli_ipc::~socket_cli_ipc()
{
    running.store(false, std::memory_order_release);
    /* 必须先 join handshake 线程，再让本对象的成员变量被销毁；
       否则该线程后续 cli_hs.receive 超时返回时会读到已被释放的成员 */
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
        handshake_thread_ = nullptr;
    }
    if (ipc_r_ptr_)
    {
        ipc_r_ptr_->close();
    }
    if (ipc_w_ptr_)
    {
        ipc_w_ptr_->close();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_cli_ipc::InitChannel(std::string extra_info)
{
    try
    {
        ipc_r_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_);
        ipc_w_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_ + 1);
        if (verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "CliInfo] Request initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
            std::cerr << "\033[32m[" << topic_name_ << "CliInfo] Response initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ + 1 << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        }
        while (!ipc_r_ptr_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "CliInfo] Failed to connect request UDP,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        while (!ipc_w_ptr_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "CliInfo] Failed to connect response UDP,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "CliInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
        return;
    }
    handshake_thread_ = new std::thread(&socket_cli_ipc::client_handshake, this);
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string response_type_name =
        (message_template && message_template->response())
            ? dzIPC::info_pool::demangle(typeid(*message_template->response()).name())
            : std::string{};
    response_type_name = extract_last_segment(response_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketClient, topic_name_, response_type_name, "socket",
                      static_cast<int32_t>(domain_id_), extra_info});
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "_CliInfo] Client connected to server topic: " << topic_name_
                  << "\033[0m" << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_cli_ipc::client_handshake()
{
    uint64_t handshake_timeout_ms = 100;   // 100 ms
    ipc::socket::UDPNode cli_hs(topic_name_.c_str(), ipaddr_.c_str(), port_hash_ + 2);
    std::shared_ptr<IpcPubSubIdInitMsg> hs_msg = std::make_shared<IpcPubSubIdInitMsg>();
    while (!cli_hs.connect())
    {
        std::cerr << "\033[31m[" << topic_name_
                  << "CliInfo] Failed to connect handshake UDP,reconnect affter 1 second...\033[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    hs_msg->cli_flag = true;
    ipc::buffer hs_buf;
    hs_buf = std::move(hs_msg->serialize());
    State st = State::RunHS;
    int send_cnt = 0;
    while (running.load(std::memory_order_acquire))
    {
        if (st == State::RunHS)
        {
            ipc::buffer rev_buf = cli_hs.receive(handshake_timeout_ms);
            if (rev_buf.empty() || !hs_msg->check_host(rev_buf)
                || !hs_msg->check_run_status(rev_buf))   // 等待服务端发送握手消息
            {
                continue;
            }
            else
            {
                while (!cli_hs.send(hs_buf))
                {
                    send_cnt++;
                    if (send_cnt > 10)
                    {
                        send_cnt = 0;
                        break;
                    }
                    std::cerr << "\033[31m[" << topic_name_
                              << "CliInfo] Failed to send handshake message,retry affter 1 second...\033[0m"
                              << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                handshake_completed_.store(true, std::memory_order_release);
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "CliInfo] Handshake with server completed for topic: " << topic_name_ << "\033[0m  "
                              << std::endl;
                }
                st = State::StopHS;
                continue;
            }
        }
        else
        {
            ipc::buffer rev_buf = cli_hs.receive(handshake_timeout_ms);
            if (rev_buf.empty() || !hs_msg->check_host(rev_buf)
                || hs_msg->check_run_status(rev_buf))   // 握手完成后继续监听服务端状态，直到服务端断开连接
            {
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[31m[" << topic_name_
                              << "CliInfo] Server disconnected, restarting handshake for topic: " << topic_name_
                              << "\033[0m" << std::endl;
                }
                handshake_completed_.store(false, std::memory_order_release);
                st = State::RunHS;
                continue;
            }
        }
    }
    hs_msg->run_status = false;   // 客户端主动断开连接时通知服务端
    hs_buf = std::move(hs_msg->serialize());
    cli_hs.send(hs_buf);
    cli_hs.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    if (handshake_completed_.load(std::memory_order_acquire))
    {
        ipc::buffer request_data(std::move(request->request()->serialize()));
        if (!chunk_send(ipc_r_ptr_, request_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "CliInfo] Error sending request: Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
        if (!chunk_rev_server(ipc_w_ptr_, request, rev_tm, false))
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "CliInfo] Error receiving response: Failed to receive or parse response\033[0m" << std::endl;
            return false;
        }
        return true;
    }
    else
    {
        return false;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
}   // namespace socket
}   // namespace dzIPC
