#pragma once
#include <cstdint>
#include <cstring>
#include "libipc/buffer.h"
#include "libipc/debug.h"
#include "libipc/export.h"

namespace ipc {
namespace socket {
class IPC_EXPORT UDPNode
{
    UDPNode(const UDPNode&) = delete;
    UDPNode& operator=(const UDPNode&) = delete;

public:
    UDPNode();
    ~UDPNode();
    /* Instantiation */
    UDPNode(const char* name, const char* ip, uint16_t port);
    void create(const char* name, const char* ip, uint16_t port) IPC_EXCEPTION_;
    bool connect() IPC_EXCEPTION_;
    bool send(ipc::buffer& data) IPC_EXCEPTION_;
    ipc::buffer receive_nowait() IPC_EXCEPTION_;
    ipc::buffer receive(uint64_t tm = ipc::invalid_value) IPC_EXCEPTION_;
    bool close() IPC_EXCEPTION_;
    void clear_cache() IPC_EXCEPTION_;

private:
    class UDPNode_;
    UDPNode_* p_;
};
}   // namespace socket
}   // namespace ipc