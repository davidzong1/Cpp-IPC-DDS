#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libipc/export.h"

template <typename msgType>
class CircularQueue;

class IpcMsgBase;

namespace dzIPC {

enum class ChannelKind : uint8_t
{
    ShmPubSub = 0,
    SocketPubSub,
    ShmService,
};

struct ChannelKey
{
    std::string topic_name;
    size_t domain_id;
    uint32_t msg_id;
    ChannelKind kind{ChannelKind::ShmPubSub};  // default for backward compatibility

    bool operator==(const ChannelKey& other) const
    {
        return kind == other.kind && topic_name == other.topic_name && domain_id == other.domain_id
               && msg_id == other.msg_id;
    }
};

}   // namespace dzIPC

namespace std {
template <>
struct hash<dzIPC::ChannelKey>
{
    std::size_t operator()(const dzIPC::ChannelKey& k) const
    {
        std::size_t h1 = hash<std::string>{}(k.topic_name);
        std::size_t h2 = hash<size_t>{}(k.domain_id);
        std::size_t h3 = hash<uint32_t>{}(k.msg_id);
        std::size_t h4 = hash<uint8_t>{}(static_cast<uint8_t>(k.kind));
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};
}   // namespace std

namespace dzIPC {

class IPC_EXPORT LocalPubSubRegistry
{
public:
    static LocalPubSubRegistry& instance();

    void register_subscriber(const ChannelKey& key, std::weak_ptr<CircularQueue<IpcMsgBase>> queue);

    void unregister_subscriber(const ChannelKey& key, std::weak_ptr<CircularQueue<IpcMsgBase>> queue);

    std::vector<std::shared_ptr<CircularQueue<IpcMsgBase>>> subscriber_snapshot(const ChannelKey& key);

private:
    LocalPubSubRegistry() = default;
    ~LocalPubSubRegistry() = default;
    LocalPubSubRegistry(const LocalPubSubRegistry&) = delete;
    LocalPubSubRegistry& operator=(const LocalPubSubRegistry&) = delete;

    using WeakQueueVec = std::vector<std::weak_ptr<CircularQueue<IpcMsgBase>>>;
    using RegistryMap = std::unordered_map<ChannelKey, WeakQueueVec>;

    void cleanup_expired(WeakQueueVec& vec);

    mutable std::shared_mutex mutex_;
    RegistryMap registry_;
};

}   // namespace dzIPC
