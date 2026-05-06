#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "info.h"

namespace dzIPC {
class sniffer_base
{
public:
    sniffer_base() = default;
    virtual ~sniffer_base() = default;

    sniffer_base(sniffer_base const&) = delete;
    sniffer_base& operator=(sniffer_base const&) = delete;

    sniffer_base(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id) {}

    virtual void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id) = 0;
    virtual sniffer_info try_recv() noexcept = 0;

protected:
    virtual sniffer_info recv_inner(std::uint64_t timeout_ms = 5'000) noexcept = 0;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop_{false};
    bool ser_or_topic_ = false;
    std::thread recv_thread_;
    std::unique_ptr<sniffer_info> msg_cache;
    std::mutex msg_mutex;
    uint32_t msg_id_{0};
};
}   // namespace dzIPC