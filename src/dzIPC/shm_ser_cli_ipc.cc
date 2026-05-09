#include "dzIPC/shm_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/name_operator.h"
#include "libipc/semaphore.h"

// #include "dzIPC/common/thread_dispatch.h"
namespace dzIPC {
namespace shm {
enum class State {
    RunHS,
    StopHS
};
using namespace ipc;

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
{
    message_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    message_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    callback_ = std::move(callback);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::~shm_ser_ipc()
{
    running = false;
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
    }
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
    {
        ipc_r_ptr_->clear();
    }
    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
    {
        ipc_w_ptr_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::InitChannel(std::string extra_info)
{
    std::string r_name, w_name;
    r_name = "dz_ipc_" + topic_name_ + "_ser_r";
    w_name = "dz_ipc_" + topic_name_ + "_ser_w";
    // 清理残留的共享内存
    ipc::server::clear_storage(r_name.c_str());
    ipc::server::clear_storage(w_name.c_str());
    // 发送清理广播，等待客户端清理完成
    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
    handshake_thread_ = new std::thread(&shm_ser_ipc::ser_handshake, this);
    std::string request_type_name = message_->request()
                                        ? dzIPC::info_pool::demangle(typeid(*message_->request()).name())
                                        : std::string{};
    request_type_name = extract_last_segment(request_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmServer, topic_name_, request_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Server channel created for topic: " << topic_name_
              << "\033[0m" << std::endl;
    response_thread_ = new std::thread(&shm_ser_ipc::response_thread_func, this);
    // ThreadDispatch::set_thread_priority(response_thread_, 20, verbose_, topic_name_ + "_SerResponseThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_ser_ipc::ser_handshake()
{
    std::string sem_ser_name = "dz_ipc_" + topic_name_ + "_ser_post";
    std::string sem_cli_name = "dz_ipc_" + topic_name_ + "_cli_post";
    std::string sem_stop_name = "dz_ipc_" + topic_name_ + "_stop_post";
    ipc::sync::semaphore sem_ser;
    if (!sem_ser.open(sem_ser_name.c_str(), 0))
    {
        throw std::runtime_error("sem_open failed");
    }
    ipc::sync::semaphore sem_cli;
    if (!sem_cli.open(sem_cli_name.c_str(), 0))
    {
        sem_ser.close();
        throw std::runtime_error("sem_open failed");
    }
    ipc::sync::semaphore sem_stop;
    if (!sem_stop.open(sem_stop_name.c_str(), 0))
    {
        sem_ser.close();
        sem_cli.close();
        throw std::runtime_error("sem_open failed");
    }
    auto st = State::RunHS;
    while (sem_ser.try_wait())
        ;   // 清空服务信号量
    while (running.load(std::memory_order_acquire))
    {
        if (st == State::RunHS)
        {
            while (sem_stop.try_wait())
                ;
            while (sem_ser.try_wait())
                ;             // 清空信号量
            sem_ser.post();   // 发送清理开始信号
            if (!sem_cli.wait(100))
            {
                continue;
            }
            while (sem_ser.try_wait())
                ;   // 清空服务信号量
            st = State::StopHS;
            handshake_completed_.store(true, std::memory_order_release);
        }
        else
        {
            if (!sem_stop.wait(100))
            {
                continue;   // 每隔100ms检查客户端是否发送请求，如果没有则继续等待
            }
            else
            {
                std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client disconnected from server: " << topic_name_
                          << "\033[0m" << std::endl;
                st = State::RunHS;   // 进入重新连接状态
                handshake_completed_.store(false, std::memory_order_release);
            }
        }
    }
    while (sem_stop.try_wait())
        ;   // 清空服务信号量
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_SerInfo] Server exiting, sending stop signal to client: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
    sem_stop.post();   // 发送停止信号，通知客户端退出等待
    sem_ser.close();
    sem_cli.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::response_thread_func()
{
    while (running.load(std::memory_order_acquire))
    {
        /* 服务端等待请求 */
        ipc::buffer raw_data = ipc_r_ptr_->recv(50);
        if (raw_data.empty())
        {
            continue;
        }
        /* 身份判断 */
        if (!message_->check_msg_id(raw_data))
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[Warning] Received message with invalid ID on topic: " << topic_name_ << "\033[0m"
                          << std::endl;
            }
            continue;
        }
        /* 反序列转换为msg数据 */
        message_->request()->deserialize(raw_data);
        callback_(message_);
        ipc::buffer response_data(std::move(message_->response()->serialize()));
        int retry_count = 0;
        while (!ipc_w_ptr_->try_send(response_data.data(), response_data.size())
               && running.load(std::memory_order_acquire))
        {
            retry_count++;
            if (retry_count > 10)
            {
                break;
            }
        }
    }   // 客户段发送请求
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_cli_ipc::shm_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                         bool verbose)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , message_(msg)
    , domain_id_(domain_id)
    , verbose_(verbose)
{}

shm_cli_ipc::~shm_cli_ipc()
{
    running.store(false, std::memory_order_release);
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
    {
        ipc_r_ptr_->release();
    }
    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
    {
        ipc_w_ptr_->release();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::InitChannel(std::string extra_info)
{
    // 等待服务端创建通道
    handshake_thread_ = new std::thread(&shm_cli_ipc::cli_handshake, this);
    // 等待握手完成
    std::string response_type_name = message_->response()
                                         ? dzIPC::info_pool::demangle(typeid(*message_->response()).name())
                                         : std::string{};
    response_type_name = extract_last_segment(response_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmClient, topic_name_, response_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "_CLiInfo] Client connected to server topic: " << topic_name_
                  << "\033[0m" << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::cli_handshake()
{
    std::string sem_ser_name = "dz_ipc_" + topic_name_ + "_ser_post";
    std::string sem_cli_name = "dz_ipc_" + topic_name_ + "_cli_post";
    std::string sem_stop_name = "dz_ipc_" + topic_name_ + "_stop_post";
    ipc::sync::semaphore sem_ser;
    if (!sem_ser.open(sem_ser_name.c_str(), 0))
    {
        throw std::runtime_error("sem_open failed");
    }
    ipc::sync::semaphore sem_cli;
    if (!sem_cli.open(sem_cli_name.c_str(), 0))
    {
        sem_ser.close();
        throw std::runtime_error("sem_open failed");
    }
    ipc::sync::semaphore sem_stop;
    if (!sem_stop.open(sem_stop_name.c_str(), 0))
    {
        sem_ser.close();
        sem_cli.close();
        throw std::runtime_error("sem_open failed");
    }
    auto st = State::RunHS;
    while (sem_ser.try_wait())
        ;   // 清空服务信号量
    while (running.load(std::memory_order_acquire))
    {
        if (st == State::RunHS)
        {
            while (sem_stop.try_wait())
                ;
            while (sem_cli.try_wait())
                ;   // 清空信号量
            if (!sem_ser.wait(100))
            {
                continue;
            }   // 等待服务端发送清理开始信号
            // 创建新的通道
            std::string r_name, w_name;
            r_name = "dz_ipc_" + topic_name_ + "_ser_w";
            w_name = "dz_ipc_" + topic_name_ + "_ser_r";
            ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
            ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
            sem_cli.post();   // 通知服务端连接完成
            st = State::StopHS;
            handshake_completed_.store(true, std::memory_order_release);
        }
        else
        {
            if (!sem_stop.wait(100))   // 等待服务端发送停止信号，若没有则继续等待
            {
                continue;
            }
            st = State::RunHS;   // 进入重新连接状态
            handshake_completed_.store(false, std::memory_order_release);
        }
    }
    while (sem_stop.try_wait())
        ;
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_CLiInfo] Client exiting, sending stop signal to server: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
    sem_stop.post();   // 发送停止信号，通知服务端退出等待
    sem_ser.close();
    sem_cli.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    /* 主线程执行，因此无需考虑running */
    if (!handshake_completed_.load(std::memory_order_acquire))
    {
        if (verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_
                      << "_CLiInfo] Handshake not completed, cannot send request on topic: " << topic_name_ << "\033[0m"
                      << std::endl;
        }
        return false;
    }
    ipc::buffer request_data(std::move(request->request()->serialize()));
    int retry_count = 0;
    while (!ipc_w_ptr_->try_send(request_data.data(), request_data.size()))
    {
        retry_count++;
        if (retry_count > 10)
        {
            return false;
        }
    };
    do
    {
        ipc::buffer raw_response = ipc_r_ptr_->recv(rev_tm);
        if (raw_response.empty())   // 超时未收到响应
        {
            return false;
        }
        if (message_->check_msg_id(raw_response))   // 收到响应且ID正确,否则重新接收
        {
            request->response()->deserialize(raw_response);
            break;
        }
    } while (true);
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    message_.reset(msg->clone());
}
}   // namespace shm
}   // namespace dzIPC