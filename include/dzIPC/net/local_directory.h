#pragma once
#include "dzIPC/net/peer_directory.h"
#include "dzIPC/net/shm_wire.h"

namespace dzIPC::net {
struct LocalBinding {
    RouteDescriptor descriptor;
    std::shared_ptr<ShmWireBridge> bridge;
    std::uint32_t generation = 0;
};
struct LocalRegistration {
    Identity id{};
    std::uint64_t session = 0;
    bool publisher = false, ready = false;
    std::shared_ptr<LocalBinding> binding;
};
// 控制线程拥有登记；bridge 初始化在公共初始化线程执行，完成后通过 set_bridge 交回。
class LocalDirectory {
public:
    explicit LocalDirectory(std::shared_ptr<DirectoryBudget>, Limits = {});
    ~LocalDirectory();
    std::shared_ptr<LocalRegistration> add(std::uint64_t session, Identity id,
                                          RouteDescriptor, bool publisher);
    bool set_bridge(const std::shared_ptr<LocalBinding>&, std::shared_ptr<ShmWireBridge>);
    std::uint64_t ready(std::uint64_t session, Identity, std::uint32_t generation);
    bool remove(std::uint64_t session, Identity, bool publisher);
    void close_session(std::uint64_t session);
    std::shared_ptr<LocalRegistration> find(std::uint64_t session, Identity) const;
    std::shared_ptr<LocalBinding> binding(const RouteKey&) const;
    std::vector<std::shared_ptr<LocalRegistration>> publishers() const;
    std::shared_ptr<const DirectorySnapshot> snapshot() const;
    std::size_t handle_count() const;
    bool healthy() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net
