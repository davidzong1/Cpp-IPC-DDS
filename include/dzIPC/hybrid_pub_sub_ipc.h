#pragma once
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include <chrono>
#include <condition_variable>

namespace dzIPC::hybrid {
struct Wakeup {
    std::mutex mutex; std::condition_variable cv; std::uint64_t generation=0;
    void notify(){std::lock_guard<std::mutex> l(mutex);++generation;cv.notify_all();}
};
class IPC_EXPORT Publisher final : public pub_ipc_base {
public:
    Publisher(const std::shared_ptr<TopicData>& msg,const std::string& topic,size_t domain,bool verbose=false,
              bool qos=false,int cpu=-1,int priority=20);
    ~Publisher() override;
    void InitChannel(std::string extra="") override;
    void reset_message(const std::shared_ptr<TopicData>& msg) override;
    bool publish(std::shared_ptr<IpcMsgBase> msg) override {return publish_best_effort(std::move(msg));}
    bool publish_best_effort(std::shared_ptr<IpcMsgBase> msg) override;
    bool publish_blocking(std::shared_ptr<IpcMsgBase> msg,std::uint64_t tm) override;
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg) override;
    bool publish_prebuilt_segment(const void* seg,std::size_t size) override;
    bool has_subscribed() const override;
private:
    void refresh_discovery();
    bool network_needed() const;
    std::array<std::uint8_t,32> source(bool local);
    std::string topic_;size_t domain_;std::uint32_t msg_id_;Identity identity_{};
    std::shared_ptr<TopicData> topic_msg_;
    std::unique_ptr<shm::shm_pub_ipc> shm_;std::unique_ptr<socket::socket_pub_ipc> socket_;
    std::shared_ptr<DiscoveryState> discovery_;
    std::chrono::steady_clock::time_point started_;
    std::uint64_t endpoint_=0,sequence_=0;
    mutable std::mutex mutex_;
    info_pool::ScopedRegistration pool_reg_;
};
class IPC_EXPORT Subscriber final : public sub_ipc_base {
public:
    Subscriber(const std::shared_ptr<TopicData>& msg,const std::string& topic,size_t domain,size_t queue_size,
               bool verbose=false,bool qos=false,int cpu=-1,int priority=20);
    ~Subscriber() override;
    void InitChannel(std::string extra="") override;
    void reset_message(const std::shared_ptr<TopicData>& msg) override;
    void get(Sample& out) override;
    bool get(Sample& out,std::uint64_t tm) override;
    bool try_get(Sample& out) override;
    void get_clone(std::shared_ptr<TopicData>& out) override;
    bool try_get_clone(std::shared_ptr<TopicData>& out) override;
    std::uint64_t suppressed_datagrams() const {return socket_->hybrid_suppressed();}
private:
    std::shared_ptr<Wakeup> wake_=std::make_shared<Wakeup>();
    std::string topic_;size_t domain_{0};std::uint32_t msg_id_{0};
    std::shared_ptr<TopicData> topic_msg_;
    std::unique_ptr<shm::shm_sub_ipc> shm_;std::unique_ptr<socket::socket_sub_ipc> socket_;
    std::atomic<unsigned> turn_{0};
    info_pool::ScopedRegistration pool_reg_;
};
}
