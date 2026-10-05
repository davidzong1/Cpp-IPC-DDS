#pragma once
#if DZIPC_SHARED_NET_BUILT
#include "dzIPC/shared_pub_sub_ipc.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/net/wire_protocol.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include <charconv>
#include <iostream>

inline int shared_cat(const std::string& topic, const std::string& domain_text,
                      std::uint32_t id, bool once, int timeout, volatile std::sig_atomic_t& running) {
    std::uint64_t domain = 0;
    const auto number = std::from_chars(domain_text.data(), domain_text.data() + domain_text.size(), domain);
    if (number.ec != std::errc{} || number.ptr != domain_text.data() + domain_text.size() || timeout < 1) {
        std::cerr << "InvalidOption: 无效域或等待时间\n"; return 2;
    }
    try {
        auto data = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), id);
        dzIPC::shared_net::Subscriber sub(data, topic, domain, 32); sub.InitChannel("topic_cat");
        std::cerr << "shared_v1: 订阅已 Ready\n";
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (running) {
            if (sub.try_get_clone(data)) {
                const auto message = std::static_pointer_cast<dzIPC::GenericMessage>(data->topic());
                if (message->has_dzflat()) {
                    const auto bytes = message->dzflat_data(); const auto size = message->dzflat_len();
                    std::cout << "{\"encoding\":\"dzflat\",\"bytes\":" << size
                              << ",\"crc32c\":" << dzIPC::net::crc32c({bytes, size}) << ",\"segment_hex\":\"";
                    constexpr char digits[] = "0123456789abcdef";
                    for (std::size_t i = 0; i < size; ++i) std::cout << digits[bytes[i] >> 4] << digits[bytes[i] & 15];
                    std::cout << "\"}" << std::endl;
                } else {
                    auto wire = message->serialize(); std::cout << dzIPC::msg_to_string(wire) << std::endl;
                }
                if (once) return 0;
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            } else if (once && std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "TimedOut: 未收到消息\n"; return 4;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "GatewayUnavailable: " << e.what() << '\n'; return 3; }
}
#endif
