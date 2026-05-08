#pragma once

#include <cstdint>
#include <string>
#include "libipc/export.h"
#define UDP_DISCOVERY_BASE_PORT 11'451

namespace dzIPC::common {
IPC_EXPORT uint64_t fnv1a64(const std::string& data);
IPC_EXPORT uint16_t udp_discovery_port_calculate(const std::string& topic_name, int domain_id);
IPC_EXPORT std::string udp_discovery_addr_calculate(const std::string& topic_name);
}   // namespace dzIPC::common