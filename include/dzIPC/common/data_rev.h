#pragma once
#include <cstdint>
#include <string>
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/topic_data.h"
#include "libipc/export.h"
#include "libipc/udp.h"

namespace dzIPC {
namespace socket {
enum class SocketDeliveryMode
{
    BestEffort,
    Reliable
};

enum class SocketIntegrityMode
{
    None,
    CRC32C
};

enum class SocketSendStatus
{
    SentUnconfirmed,
    DeliveredAcked,
    FailedTimeout,
    FailedLocalSend,
    FailedIntegrity,
    FailedInvalidArgument
};

struct SocketSendOptions
{
    SocketDeliveryMode delivery{SocketDeliveryMode::BestEffort};
    SocketIntegrityMode integrity{SocketIntegrityMode::None};
    uint64_t ack_timeout_ms{ipc::invalid_value};
};

struct SocketSendReport
{
    SocketSendStatus status{SocketSendStatus::FailedInvalidArgument};
    uint32_t crc32c{0};
    uint32_t ack_crc32c{0};
    uint32_t sequence{0};

    bool ok() const
    {
        return status == SocketSendStatus::SentUnconfirmed || status == SocketSendStatus::DeliveredAcked;
    }
};

IPC_EXPORT bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg,
                                uint64_t tm);
IPC_EXPORT bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg,
                                 uint64_t tm, bool ser_or_cli);
IPC_EXPORT SocketSendReport chunk_send_ex(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                          const SocketSendOptions& options);
IPC_EXPORT bool chunk_send(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data);
IPC_EXPORT ipc::buffer chunk_rev_sniff(ipc::socket::UDPNode& node, uint64_t tm);
}   // namespace socket
}   // namespace dzIPC
