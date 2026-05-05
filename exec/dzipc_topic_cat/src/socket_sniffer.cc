#include "socket_sniffer.h"
#include <string>
#include "dzIPC/common/hash.h"

namespace dzIPC {
void socket_sniffer::create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic)
{
    this->ser_or_topic_ = ser_or_topic;
    std::string ip_hash = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    uint16_t port_hash = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    req_ = std::make_unique<ipc::socket::UDPNode>();
    req_->create(topic_name.c_str(), ip_hash.c_str(), port_hash);
    if (ser_or_topic_)
    {
        res_ = std::make_unique<ipc::socket::UDPNode>();
        res_->create(topic_name.c_str(), ip_hash.c_str(), port_hash + 1);
    }
    ready = true;
}

sniffer_info socket_sniffer::try_recv() noexcept
{
    if (!ready)
        return sniffer_info{ipc::buffer{}, ipc::buffer{}};
    if (ser_or_topic_)
    {
        ipc::buffer req_cache, res_cache;
        try
        {
            req_cache = std::move(req_->receive(0));
            res_cache = std::move(res_->receive(0));
        }
        catch (const std::exception& e)
        {
            // std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
        return sniffer_info{std::move(req_cache), std::move(res_cache)};
    }
    else
    {
        try
        {
            return sniffer_info{req_->receive(0), ipc::buffer{}};
        }
        catch (const std::exception& e)
        {
            // std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
    }
}

sniffer_info socket_sniffer::recv(std::uint64_t timeout_ms) noexcept
{
    if (!ready)
        return sniffer_info{ipc::buffer{}, ipc::buffer{}};
    if (ser_or_topic_)
    {
        ipc::buffer req_cache, res_cache;
        try
        {
            req_cache = std::move(req_->receive(timeout_ms));
            res_cache = std::move(res_->receive(timeout_ms));
        }
        catch (const std::exception& e)
        {
            // std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
        return sniffer_info{std::move(req_cache), std::move(res_cache)};
    }
    else
    {
        try
        {
            return sniffer_info{req_->receive(timeout_ms), ipc::buffer{}};
        }
        catch (const std::exception& e)
        {
            // std::fprintf(stderr, "socket_sniffer try_recv error: %s\n", e.what());
            return sniffer_info{ipc::buffer{}, ipc::buffer{}};
        }
    }
}
}   // namespace dzIPC
