#pragma once
#include <memory>
#include "libipc/ipc.h"
#include "libipc/sniffer.h"
#include "sniffer_base.h"

namespace dzIPC {

class shm_sniffer : public sniffer_base
{
public:
    shm_sniffer() = default;
    ~shm_sniffer() = default;

    shm_sniffer(shm_sniffer const&) = delete;
    shm_sniffer& operator=(shm_sniffer const&) = delete;

    shm_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic)
    {
        create_sniffer(topic_name, domain_id, ser_or_topic);
    }

    void create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic) override;

    sniffer_info try_recv() noexcept override;

    sniffer_info recv(std::uint64_t timeout_ms = 5'000) noexcept override;

private:
    std::unique_ptr<ipc::sniffer> req_, res_;
};
}   // namespace dzIPC