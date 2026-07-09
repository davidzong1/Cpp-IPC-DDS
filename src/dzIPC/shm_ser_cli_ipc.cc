#include "dzIPC/shm_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/name_operator.h"

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

std::string service_control_name_for(const std::string& topic_name)
{
    return "dz_ipc_" + topic_name + "_ser_control";
}

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose,
                         bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SerOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
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
void shm_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
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
    if (!control_plane_.open(service_control_name_for(topic_name_)))
    {
        throw std::runtime_error("failed to open service control plane");
    }
    control_plane_.begin_rebuild();
    ipc::server::clear_storage(r_name.c_str());
    ipc::server::clear_storage(w_name.c_str());
    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
    control_plane_.set_ready();
    handshake_thread_ = new std::thread(&shm_ser_ipc::ser_handshake, this);
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
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmServer, topic_name_, request_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Server channel created for topic: " << topic_name_
              << "\033[0m" << std::endl;
    response_thread_ = new std::thread(&shm_ser_ipc::response_thread_func, this);
    dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SerResponseThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_ser_ipc::ser_handshake()
{
    bool had_client = false;
    while (running.load(std::memory_order_acquire))
    {
        control_plane_.heartbeat();
        const bool has_client = control_plane_.peer_count() > 0;
        handshake_completed_.store(has_client, std::memory_order_release);
        if (has_client && !had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client connected to server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        if (!has_client && had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client disconnected from server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        had_client = has_client;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handshake_completed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_SerInfo] Server exiting, marking control plane stopping: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
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
        std::shared_ptr<ServiceData> local_msg;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            if (!message_)
            {
                continue;
            }
            local_msg.reset(message_->clone());
        }
        if (!local_msg->check_msg_id(raw_data))
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[Warning] Received message with invalid ID on topic: " << topic_name_ << "\033[0m"
                          << std::endl;
            }
            continue;
        }
        /* 反序列转换为msg数据 */
        local_msg->request()->deserialize(raw_data);
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
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_CliOwnerThread");
}

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
    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (ipc_r_ptr_ && ipc_r_ptr_->valid())
        {
            ipc_r_ptr_->release();
        }
        if (ipc_w_ptr_ && ipc_w_ptr_->valid())
        {
            ipc_w_ptr_->release();
        }
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
    if (!control_plane_.open(service_control_name_for(topic_name_)))
    {
        throw std::runtime_error("control plane open failed");
    }
    uint32_t attached_generation = 0;
    bool peer_registered = false;
    while (running.load(std::memory_order_acquire))
    {
        const uint32_t generation = control_plane_.generation();
        const TopicState state = control_plane_.state();
        if (state == TopicState::Ready && generation != 0)
        {
            if (!handshake_completed_.load(std::memory_order_acquire) || attached_generation != generation)
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                std::string r_name, w_name;
                r_name = "dz_ipc_" + topic_name_ + "_ser_w";
                w_name = "dz_ipc_" + topic_name_ + "_ser_r";
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
                    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
                }
                attached_generation = generation;
                if (!control_plane_.add_peer(attached_generation))
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_.reset();
                    ipc_w_ptr_.reset();
                    continue;
                }
                peer_registered = true;
                handshake_completed_.store(true, std::memory_order_release);
            }
        }
        else
        {
            if (handshake_completed_.exchange(false, std::memory_order_acq_rel))
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                std::lock_guard<std::mutex> lock(channel_mtx_);
                if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                {
                    ipc_r_ptr_->release();
                }
                if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                {
                    ipc_w_ptr_->release();
                }
                ipc_r_ptr_.reset();
                ipc_w_ptr_.reset();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_CLiInfo] Client exiting, detaching from server: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
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
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (!message_)
        {
            return false;
        }
        message_template.reset(message_->clone());
    }
    std::lock_guard<std::mutex> channel_lock(channel_mtx_);
    if (!handshake_completed_.load(std::memory_order_acquire) || !ipc_w_ptr_ || !ipc_r_ptr_)
    {
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
        if (message_template->check_msg_id(raw_response))   // 收到响应且ID正确,否则重新接收
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
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}
}   // namespace shm
}   // namespace dzIPC
