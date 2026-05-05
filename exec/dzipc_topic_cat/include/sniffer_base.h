#pragma once
#include <string>
#include "info.h"

namespace dzIPC {
class sniffer_base
{
public:
    sniffer_base() = default;
    ~sniffer_base() = default;

    sniffer_base(sniffer_base const&) = delete;
    sniffer_base& operator=(sniffer_base const&) = delete;

    virtual void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic) = 0;
    virtual sniffer_info try_recv() noexcept = 0;
    virtual sniffer_info recv(std::uint64_t timeout_ms = 5'000) noexcept = 0;

protected:
    bool ready = false;
    bool ser_or_topic_ = false;
};
}   // namespace dzIPC