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
    void create(const char* name, const char* ip, uint16_t port) Exception;
    bool connect() Exception;
    bool send(ipc::buffer& data) Exception;
    ipc::buffer receive(uint64_t tm = ipc::invalid_value) Exception;
    bool close() Exception;
    void clear_cache() Exception;

private:
    class UDPNode_;
    UDPNode_* p_;
};
}   // namespace socket
}   // namespace ipc