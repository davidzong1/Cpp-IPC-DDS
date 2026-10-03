#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "libipc/export.h"

namespace dzIPC::hybrid {
using Identity = std::array<std::uint8_t,16>;
struct DiscoveryState {
    std::array<std::uint8_t,32> scope{};
    Identity locality{};
    std::uint64_t endpoint{0};
    std::uint32_t msg_id{0};
    bool subscriber{false}, local_shm{false};
    std::atomic<bool> active{true}, network_needed{true}, any_subscriber{false};
};
IPC_EXPORT std::uint64_t new_endpoint_id();
IPC_EXPORT Identity local_identity();
// 单进程共享发现线程；租约消失后最长 1.5s 继续保守发包。
IPC_EXPORT std::shared_ptr<DiscoveryState> discover(const std::string& topic, std::uint64_t domain,
                                                   std::uint32_t msg_id, bool subscriber, bool local_shm);
}
