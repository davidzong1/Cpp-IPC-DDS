#pragma once
#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/wire_blob.h"
#include <functional>

namespace dzIPC::net
{
std::size_t outbox_capacity(std::size_t payload);
enum class OutboxCode
{
    Accepted,
    NotReady,
    Invalid,
    CreditUnavailable,
    LoanUnavailable,
    CommitFailed,
    Indeterminate
};
struct OutboxSubmit
{
    SubmitState state = SubmitState::NotSubmitted;
    OutboxCode code = OutboxCode::NotReady;
};
struct OutboxUsage
{
    CreditCounters used, reserved, released, granted;
};
class OutboxSender
{
  public:
    OutboxSender(const WelcomeBody &, std::uint64_t session, std::uint64_t epoch, NetMetrics* metrics = nullptr);
    ~OutboxSender();
    int event_fd() const;
    void ready();
    void stop();
    void grant(CreditCounters);
    void progress(CreditCounters);
    CreditCounters needed(std::size_t payload) const;
    bool wait(std::uint64_t deadline_ns);
    OutboxUsage usage() const;
    OutboxSubmit submit(const OutboxHeader &, ByteView blob);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 全局占用是未用授予与在途缓存之和；进度不能释放网络缓存信用。
class SendAccount;
class SendBudget
{
  public:
    SendBudget(CreditCounters global_limit, CreditCounters session_limit, std::shared_ptr<NetMetrics> metrics = {});
    std::shared_ptr<SendAccount> open(CreditCounters initial);
    CreditCounters occupied() const;
    CreditCounters inflight() const;

  private:
    friend class SendAccount;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
class SendAccount
{
  public:
    ~SendAccount();
    bool grant(CreditCounters additional);
    bool take(CreditCounters cost);
    // 调用者先释放 WireBlob，再结算。活跃会话复用原预留，关闭会话归还全局额度。
    void finish(CreditCounters cost);
    void close();
    CreditCounters granted() const;
    CreditCounters inflight() const;

  private:
    friend class SendBudget;
    friend class OutboxReceiver;
    explicit SendAccount(std::shared_ptr<SendBudget::Impl>);
    std::shared_ptr<SendBudget::Impl> pool_;
    CreditCounters granted_, used_, inflight_;
    bool closed_ = false;
};
struct OutboxRecord
{
    OutboxRecord() = default;
    ~OutboxRecord();
    OutboxRecord(OutboxRecord &&) noexcept;
    OutboxRecord &operator=(OutboxRecord &&) noexcept;
    OutboxRecord(const OutboxRecord &) = delete;
    OutboxRecord &operator=(const OutboxRecord &) = delete;
    void release() noexcept;
    void trace(MessageTracePoint point, std::uint64_t stamp_ns = 0) const noexcept;
    std::shared_ptr<NetMetrics> trace_metrics() const noexcept { return trace_metrics_; }
    OutboxHeader header;
    WireBlob blob;
    CreditCounters network_cost;
    std::uint64_t pulled_ns = 0;

  private:
    friend class OutboxReceiver;
    std::shared_ptr<SendAccount> account_;
    std::shared_ptr<NetMetrics> trace_metrics_;
};
class OutboxReceiver
{
  public:
    // descriptor 的所有权由调用者保留；内部复制且核验是非阻塞 eventfd。
    OutboxReceiver(const WelcomeBody &, std::uint64_t session, std::uint64_t epoch, int descriptor,
                   std::shared_ptr<SendAccount>);
    ~OutboxReceiver();
    int event_fd() const;
    void consume_notification();
    bool pull(OutboxRecord &);
    CreditCounters progress() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net
