#pragma once
#include <string>
#include "shm_sniffer.h"
#include "hybrid_sniffer.h"
#include "socket_sniffer.h"

namespace dzIPC {
class sniffer
{
public:
    sniffer() = default;
    ~sniffer() = default;

    sniffer(sniffer const&) = delete;
    sniffer& operator=(sniffer const&) = delete;

    sniffer(const std::string& topic_name, std::uint64_t domain_id, bool ser_or_topic, bool shm_or_socket, uint32_t msg_id,
            bool watch_handshake = false, bool hybrid = false)
    {
        create_sniffer(topic_name, domain_id, ser_or_topic, shm_or_socket, msg_id, watch_handshake);
    }

    void create_sniffer(const std::string& topic_name, std::uint64_t domain_id, bool ser_or_topic, bool shm_or_socket,
                        uint32_t msg_id, bool watch_handshake = false, bool hybrid = false)
    {
        if (hybrid && !ser_or_topic)
            sniffer_impl = std::make_unique<hybrid_sniffer>(topic_name, domain_id, msg_id);
        else if (shm_or_socket)
            sniffer_impl = std::make_unique<shm_sniffer>(topic_name, domain_id, ser_or_topic, msg_id);
        else
            sniffer_impl = std::make_unique<socket_sniffer>(topic_name, domain_id, ser_or_topic, msg_id, watch_handshake);
    }

    sniffer_info try_recv() noexcept { return sniffer_impl->try_recv(); }

private:
    std::unique_ptr<sniffer_base> sniffer_impl;
};
}   // namespace dzIPC