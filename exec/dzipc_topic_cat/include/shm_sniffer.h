#pragma once
#include <memory>
#include "dzIPC/common/control_plane.h"
#include "libipc/ipc.h"
#include "libipc/sniffer.h"
#include "sniffer_base.h"

namespace dzIPC {

class shm_sniffer : public sniffer_base
{
public:
    shm_sniffer() = default;
    ~shm_sniffer();

    shm_sniffer(shm_sniffer const&) = delete;
    shm_sniffer& operator=(shm_sniffer const&) = delete;

    shm_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id);

    void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id) override;

    sniffer_info try_recv() noexcept override;

private:
    bool open_channels(const std::string& topic_name, int domain_id, bool ser_or_topic);
    void reopen_if_generation_changed();

    sniffer_info recv_inner(std::uint64_t timeout_ms = 5'000) noexcept override;
    std::unique_ptr<ipc::sniffer> req_, res_;
    dzIPC::control_plane_shm::TopicControlPlane control_plane_;
    std::string topic_name_;
    int domain_id_{0};
    std::uint32_t generation_{0};
};
}   // namespace dzIPC
