#pragma once
#include <memory>
#include "handshake_probe.h"
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

    /* watch_handshake 默认 false: 不打开时与历史行为逐字一致(不多占端口、
     * 不多输出一行)。见 handshake_probe.h 的说明与实测证据。 */
    socket_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id,
                   bool watch_handshake = false)
        : sniffer_base(topic_name, domain_id, ser_or_topic, msg_id)
    {
        watch_handshake_ = watch_handshake;
        create_sniffer(topic_name, domain_id, ser_or_topic, msg_id);
    }

    void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id) override;
    sniffer_info try_recv() noexcept override;

private:
    bool check_msg_id(const ipc::buffer& buf) const;
    sniffer_info recv_inner(std::uint64_t timeout_ms = 5'000) noexcept override;
    std::unique_ptr<ipc::socket::UDPNode> req_, res_;
    bool watch_handshake_{false};
    handshake_probe hs_probe_;
};
}   // namespace dzIPC