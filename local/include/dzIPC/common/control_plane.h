#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dzIPC {
namespace control_plane_shm {

enum class TopicState : uint32_t
{
    Empty = 0,
    Clearing = 1,
    Ready = 2,
    Stopping = 3
};

struct TopicControl
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> generation;
    std::atomic<uint32_t> state;
    std::atomic<int32_t> owner_pid;
    std::atomic<int64_t> heartbeat_ns;
    std::atomic<uint32_t> peer_count;
};

class TopicControlPlane
{
public:
    TopicControlPlane();
    ~TopicControlPlane();

    TopicControlPlane(const TopicControlPlane&) = delete;
    TopicControlPlane& operator=(const TopicControlPlane&) = delete;

    bool open(const std::string& name);
    bool valid() const noexcept { return control_ != nullptr; }

    uint32_t begin_rebuild();
    void set_ready();
    void set_stopping();
    void heartbeat();

    uint32_t generation() const;
    TopicState state() const;
    uint32_t peer_count() const;

    bool add_peer(uint32_t generation);
    void remove_peer(uint32_t generation);

private:
    void initialize_if_needed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TopicControl* control_{nullptr};
};

}   // namespace control_plane_shm
}   // namespace dzIPC
