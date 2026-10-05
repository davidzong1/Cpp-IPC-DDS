#pragma once
#include "dzIPC/net/reassembly.h"
#include <map>

namespace dzIPC::net {
enum class DirectoryCode { Ok, Ignored, Invalid, IdentityConflict, RetiredEpoch, HistoryFull,
                           QuotaExceeded, Incomplete, Conflict, Stale };
struct DirectoryUsage { std::uint64_t installed = 0, old = 0, candidates = 0, histories = 0; };
// 不可变目录；发送事务/入站 admission 必须持有快照，旧版本的费用随最后一个引用释放。
struct DirectorySnapshot {
    std::shared_ptr<void> charge;
    std::uint64_t version = 0;
    std::uint32_t body_crc = 0;
    Bytes body;
    std::map<RouteKey, std::shared_ptr<RouteAdmission>> routes;
};
class DirectoryBudget {
public:
    explicit DirectoryBudget(Limits limits = {});
    ~DirectoryBudget();
    DirectoryUsage usage() const;
    // 失败保持 old 不变；成功使消失/改变的 admission 永久失效。
    std::shared_ptr<const DirectorySnapshot> replace(const std::vector<RouteDescriptor>&,
        std::uint64_t version, const std::shared_ptr<const DirectorySnapshot>& old = {});
private:
    friend class PeerDirectory;
    struct Impl; std::shared_ptr<Impl> impl_;
};
struct PeerView {
    std::shared_ptr<PeerAdmission> admission;
    std::shared_ptr<const DirectorySnapshot> snapshot;
};
struct CatalogRequest { Ipv4Address destination; Bytes packet; };
class PeerDirectory {
public:
    PeerDirectory(Identity local, std::uint64_t epoch, std::shared_ptr<DirectoryBudget>);
    ~PeerDirectory();
    // 仅控制线程调用；datagram 来源 IP 是唯一远端地址。
    DirectoryCode hello(const DiscoveryHello&, Ipv4Address source, std::uint64_t now_ns);
    DirectoryCode page(const ReceivedDatagram&, std::uint64_t now_ns);
    std::vector<CatalogRequest> tick(std::uint64_t now_ns, std::size_t allowance = 64);
    PeerView peer(const Identity&) const;
    bool reachable(const Identity&, std::uint64_t now_ns) const;
    std::vector<PeerView> peers() const;
    std::vector<PeerView> targets(const RouteDescriptor&) const;
    bool synchronized() const;
    std::uint64_t revision() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
// 每次只生成一页；调用者抓住一个快照后按控制预算推进 page_index。
Bytes catalog_page(const DirectorySnapshot&, CatalogHeader, std::uint32_t page_index);
bool compatible_routes(const RouteDescriptor&, const RouteDescriptor&) noexcept;
} // namespace dzIPC::net
