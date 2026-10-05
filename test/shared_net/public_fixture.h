#pragma once
#include "shm_wire_fixture.h"
#include "dzIPC/shared_pub_sub_ipc.h"
#include "dzIPC/topic_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace shared_net_test {
inline Directory& public_directory() { static Directory directory; return directory; }
struct PublicFixture {
    BusinessTopic topic;
    GatewayConfig config;
    std::unique_ptr<GatewayRuntime> gateway;
    PublicFixture() : config(configuration(public_directory())) {
        setenv("DZIPC_NET_BACKEND", "shared_v1", 1);
        setenv("DZIPC_GATEWAY_CONTROL", public_directory().control().c_str(), 1);
        gateway = std::make_unique<GatewayRuntime>(config);
    }
    std::shared_ptr<dzIPC::TopicData> model(std::uint32_t id = 71) const {
        return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), id);
    }
    std::shared_ptr<dzIPC::Msg::StdImage> message(std::uint32_t id = 71) const {
        auto value = std::make_shared<dzIPC::Msg::StdImage>(); value->set_msg_id(id); value->width = 8; value->height = 8; value->data.assign(64, 0x7b); return value;
    }
};
}
