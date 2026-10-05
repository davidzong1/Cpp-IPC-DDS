#pragma once
#include "dzIPC/common/channel_scope.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/net/shm_wire.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "outbox_fixture.h"

namespace shared_net_test
{
struct BusinessTopic
{
    RouteDescriptor descriptor;
    bool nodelet = dzIPC::IsNodeletEnabled(), flat = dzIPC::IsDzFlatEnabled();
    BusinessTopic()
    {
        setenv("DZIPC_SHM_MPMC", "1", 1);
        dzIPC::EnableDzFlat(true);
        dzIPC::EnableNodelet(false);
        descriptor.topic =
            "shared_wire_" + std::to_string(getpid()) + "_" + std::to_string(local::random_epoch());
        descriptor.key.scope = dzIPC::common::channel_scope_token(descriptor.topic, 0,
                                                                  dzIPC::common::ScopeKind::PubSub);
        descriptor.key.msg_id = 71;
    }
    ~BusinessTopic()
    {
        ipc::mpmc_channel::clear_storage(segment().c_str());
        ipc::shm::handle::clear_storage(shm_topic_mpmc_control_name(descriptor.topic, 0).c_str());
        ipc::shm::handle::clear_storage(
            shm_topic_publisher_registry_name(descriptor.topic, 0).c_str());
        dzIPC::EnableDzFlat(flat);
        dzIPC::EnableNodelet(nodelet);
    }
    std::string segment() const
    {
        return shm_topic_mpmc_segment_name(descriptor.topic, 0);
    }
    std::shared_ptr<dzIPC::TopicData> generic() const
    {
        return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(),
                                                  descriptor.key.msg_id);
    }
};
} // namespace shared_net_test
