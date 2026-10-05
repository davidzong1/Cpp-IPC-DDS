#pragma once
#include "dzIPC/net/local_protocol.h"
#include "dzIPC/net/metrics.h"
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace dzIPC::net
{
enum class SubmitState
{
    NotRequired,
    NotSubmitted,
    Committed,
    Indeterminate
};
bool best_effort_result(SubmitState local, SubmitState network) noexcept;
bool reliable_result(SubmitState local, const SendResultBody &remote) noexcept;
struct SendTicket
{
    std::uint64_t request_id = 0;
    std::shared_future<SendResultBody> result;
};
class SendWaitTable
{
  public:
    explicit SendWaitTable(std::size_t limit = 512);
    ~SendWaitTable();
    SendTicket insert(std::uint64_t request_id, Identity publisher, std::uint64_t sequence);
    bool complete(std::uint64_t request_id, const SendResultBody &result);
    SendResultBody wait(const SendTicket &, std::uint64_t deadline_ns);
    SendResultBody cancel(const SendTicket &, SendResultCode);
    void disconnect();
    std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct ControlReply
{
    LocalHeader header;
    Bytes body;
};
struct OutboxSubmit;
// 与一个publisher和ClientRuntime会话绑定；完整状态仅由runtime在mutex内访问。
class RouteStateHint {
public:
    bool local_only() const noexcept { return local_only_.load(std::memory_order_acquire); }
private:
    friend class ClientRuntime;
    RouteStateBody state_;
    std::atomic<bool> local_only_{false};
};
class ClientRuntime : public std::enable_shared_from_this<ClientRuntime>
{
  public:
    static std::shared_ptr<ClientRuntime> acquire(const std::string &control_path);
    ~ClientRuntime();
    ClientRuntime(const ClientRuntime &) = delete;
    ClientRuntime &operator=(const ClientRuntime &) = delete;
    ControlReply request(LocalKind kind, Bytes body,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
                         int descriptor = -1);
    WelcomeBody welcome() const;
    std::uint64_t session_id() const;
    std::uint64_t gateway_epoch() const;
    bool healthy() const noexcept;
    void stop();
    CreditCounters granted() const;
    CreditCounters released() const;
    RouteStateBody route_state(const Identity &publisher) const;
    std::shared_ptr<const RouteStateHint> watch_route(const Identity &publisher);
    SendTicket prepare_send(Identity publisher, std::uint64_t sequence);
    SendResultBody wait_send(const SendTicket &, std::uint64_t deadline_ns);
    SendResultBody cancel_send(const SendTicket &, SendResultCode);
    std::string status();
    std::string gateway_metrics(unsigned category = 0);
    NetMetrics& metrics() const;
    void record_publish(SubmitState local, SubmitState network);
    std::string diagnostics_json() const;
    std::string route_status(const RouteKey&, Identity peer = {});
    void attach_outbox();
    OutboxSubmit submit_outbox(const OutboxHeader &, ByteView blob);

  private:
    explicit ClientRuntime(const std::string &path);
    void check_process() const;
    const std::int32_t owner_pid_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net
