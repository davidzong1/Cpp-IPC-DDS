#pragma once
#include "dzIPC/net/local_protocol.h"
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
    SendTicket prepare_send(Identity publisher, std::uint64_t sequence);
    SendResultBody wait_send(const SendTicket &, std::uint64_t deadline_ns);
    std::string status();
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
