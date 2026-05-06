#pragma once
#include <memory>
#include "libipc/udp.h"
#include "sniffer_base.h"

namespace dzIPC {

class socket_sniffer : public sniffer_base
{
public:
    socket_sniffer() = default;
    ~socket_sniffer();

    socket_sniffer(socket_sniffer const&) = delete;
    socket_sniffer& operator=(socket_sniffer const&) = delete;

    socket_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id)
        : sniffer_base(topic_name, domain_id, ser_or_topic, msg_id)
    {
        create_sniffer(topic_name, domain_id, ser_or_topic, msg_id);
    }

    void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id) override;
    sniffer_info try_recv() noexcept override;

private:
    bool check_msg_id(const ipc::buffer& buf) const;
    sniffer_info recv_inner(std::uint64_t timeout_ms = 5'000) noexcept override;
    std::unique_ptr<ipc::socket::UDPNode> req_, res_;
};
}   // namespace dzIPC