#include "dzIPC/net/outbox.h"
#include "libipc/ipc.h"
#include "local_control_linux.h"
#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace dzIPC::net
{
namespace
{
bool fits(CreditCounters a, CreditCounters b)
{
    return a.bytes <= b.bytes && a.records <= b.records;
}
CreditCounters add(CreditCounters a, CreditCounters b)
{
    if (a.bytes > UINT64_MAX - b.bytes || a.records > UINT64_MAX - b.records)
        throw std::overflow_error("会话信用计数耗尽");
    return {a.bytes + b.bytes, a.records + b.records};
}
CreditCounters sub(CreditCounters a, CreditCounters b)
{
    if (!fits(b, a))
        throw std::runtime_error("信用账本下溢");
    return {a.bytes - b.bytes, a.records - b.records};
}
void advance(CreditCounters &old, CreditCounters value)
{
    if (fits(value, old))
        return;
    if (!fits(old, value))
        throw std::runtime_error("累计信用不可比较");
    old = value;
}
CreditCounters network_cost(std::size_t size)
{
    return {((size + 63) / 64) * 64, 1};
}
} // namespace
std::size_t outbox_capacity(std::size_t payload)
{
    if (!payload || payload > kMaxMessageBytes)
        throw std::invalid_argument("出站消息长度无效");
    const auto need = payload + kOutboxHeaderSize;
    if (need <= 65536)
        return ((need + 1023) / 1024) * 1024;
    std::size_t capacity = 131072;
    while (capacity < need)
        capacity *= 2;
    return capacity;
}
struct OutboxSender::Impl
{
    WelcomeBody welcome;
    std::uint64_t session, epoch;
    local::Fd event;
    std::shared_ptr<ipc::route> channel;
    mutable std::mutex mutex;
    std::condition_variable changed;
    CreditCounters out_used, out_reserved, released, net_used, net_reserved, granted;
    bool ready = false, closed = false;
};
OutboxSender::OutboxSender(const WelcomeBody &w, std::uint64_t session, std::uint64_t epoch)
    : impl_(new Impl)
{
    Bytes checked;
    if (!session || !epoch || w.tx_name != outbox_name(w.locality, epoch, session) ||
        !encode_welcome(w, checked))
        throw std::invalid_argument("出站会话参数无效");
    impl_->welcome = w;
    impl_->session = session;
    impl_->epoch = epoch;
    impl_->granted = {w.granted_bytes, w.granted_records};
    impl_->event = local::event();
    impl_->channel = std::make_shared<ipc::route>(ipc::prefix{w.tx_name.c_str()}, w.tx_name.c_str(),
                                                  ipc::sender, false);
    if (!impl_->channel->valid())
        throw std::runtime_error("创建出站 SHM 失败");
}
OutboxSender::~OutboxSender()
{
    stop();
    impl_->channel.reset();
    ipc::route::clear_storage(ipc::prefix{impl_->welcome.tx_name.c_str()},
                              impl_->welcome.tx_name.c_str());
}
int OutboxSender::event_fd() const
{
    return impl_->event.get();
}
void OutboxSender::ready()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->closed)
        impl_->ready = true;
    impl_->changed.notify_all();
}
void OutboxSender::stop()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
    impl_->ready = false;
    impl_->changed.notify_all();
}
void OutboxSender::grant(CreditCounters value)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    advance(impl_->granted, value);
    impl_->changed.notify_all();
}
void OutboxSender::progress(CreditCounters value)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!fits(value, impl_->out_used))
    {
        impl_->closed = true;
        impl_->changed.notify_all();
        throw std::runtime_error("出站进度超过实际提交");
    }
    advance(impl_->released, value);
    impl_->changed.notify_all();
}
CreditCounters OutboxSender::needed(std::size_t payload) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto need = network_cost(payload),
               available = sub(impl_->granted, add(impl_->net_used, impl_->net_reserved));
    return {need.bytes > available.bytes ? need.bytes - available.bytes : 0,
            need.records > available.records ? need.records - available.records : 0};
}
bool OutboxSender::wait(std::uint64_t deadline)
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto now = local::monotonic_ns();
    if (impl_->closed || now >= deadline)
        return false;
    impl_->changed.wait_for(
        lock, std::chrono::nanoseconds(std::min<std::uint64_t>(deadline - now, 1000000)));
    return !impl_->closed && local::monotonic_ns() < deadline;
}
OutboxUsage OutboxSender::usage() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->out_used, impl_->out_reserved, impl_->released, impl_->granted};
}
OutboxSubmit OutboxSender::submit(const OutboxHeader &h, ByteView blob)
{
    if (!blob.data || !blob.size || blob.size > impl_->welcome.max_message_bytes ||
        h.session_id != impl_->session || h.gateway_epoch != impl_->epoch ||
        (h.delivery == Delivery::Reliable && local::monotonic_ns() >= h.deadline_monotonic_ns))
        return {SubmitState::NotSubmitted, OutboxCode::Invalid};
    const CreditCounters out{outbox_capacity(blob.size), 1}, net = network_cost(blob.size);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->ready || impl_->closed)
            return {};
        const auto occupancy = add(sub(impl_->out_used, impl_->released), impl_->out_reserved);
        if (!fits(out, sub({impl_->welcome.outbox_limit_bytes, impl_->welcome.outbox_record_limit},
                           occupancy)) ||
            !fits(net, sub(impl_->granted, add(impl_->net_used, impl_->net_reserved))))
            return {SubmitState::NotSubmitted, OutboxCode::CreditUnavailable};
        impl_->out_reserved = add(impl_->out_reserved, out);
        impl_->net_reserved = add(impl_->net_reserved, net);
    }
    // 借样与大载荷写入不持提交锁。多线程只共享池的短内部锁。
    ipc::loan_t loan;
    bool reserved = true;
    try
    {
        loan = impl_->channel->loan(kOutboxHeaderSize + blob.size);
        if (loan.valid() && loan.size != out.bytes)
            throw std::runtime_error("实际 loan 档位与信用不符");
        const bool encoded =
            loan.valid() && bool(encode_outbox_into(h, blob, loan.data, loan.size));
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->out_reserved = sub(impl_->out_reserved, out);
        impl_->net_reserved = sub(impl_->net_reserved, net);
        reserved = false;
        if (!encoded || impl_->closed ||
            (h.delivery == Delivery::Reliable && local::monotonic_ns() >= h.deadline_monotonic_ns))
        {
            impl_->changed.notify_all();
            return {SubmitState::NotSubmitted,
                    loan.valid() ? OutboxCode::Invalid : OutboxCode::LoanUnavailable};
        }
        // 进度处理也取此短锁：即使网关先消费，也不会在 used 入账前应用 TX_PROGRESS。
        const auto out_next = add(impl_->out_used, out), net_next = add(impl_->net_used, net);
        bool committed;
        try
        {
            committed = impl_->channel->try_publish_loan(loan, false);
        }
        catch (...)
        {
            impl_->out_used = out_next;
            impl_->net_used = net_next;
            impl_->closed = true;
            impl_->changed.notify_all();
            return {SubmitState::Indeterminate, OutboxCode::Indeterminate};
        }
        if (!committed)
        {
            impl_->changed.notify_all();
            return {SubmitState::NotSubmitted, OutboxCode::CommitFailed};
        }
        impl_->out_used = out_next;
        impl_->net_used = net_next;
        local::notify(impl_->event.get());
        return {SubmitState::Committed, OutboxCode::Accepted};
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (reserved)
        {
            impl_->out_reserved = sub(impl_->out_reserved, out);
            impl_->net_reserved = sub(impl_->net_reserved, net);
        }
        impl_->closed = true;
        impl_->changed.notify_all();
        throw;
    }
}
struct SendBudget::Impl
{
    mutable std::mutex mutex;
    CreditCounters limit, session_limit, occupied, inflight;
};
SendBudget::SendBudget(CreditCounters global, CreditCounters session)
    : impl_(std::make_shared<Impl>())
{
    impl_->limit = global;
    impl_->session_limit = session;
}
std::shared_ptr<SendAccount> SendBudget::open(CreditCounters initial)
{
    auto account = std::shared_ptr<SendAccount>(new SendAccount(impl_));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto available = sub(impl_->limit, impl_->occupied);
    account->granted_ = {
        std::min({initial.bytes, available.bytes, impl_->session_limit.bytes}),
        std::min({initial.records, available.records, impl_->session_limit.records})};
    impl_->occupied = add(impl_->occupied, account->granted_);
    return account;
}
CreditCounters SendBudget::occupied() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->occupied;
}
CreditCounters SendBudget::inflight() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->inflight; }
SendAccount::SendAccount(std::shared_ptr<SendBudget::Impl> pool) : pool_(std::move(pool))
{
}
SendAccount::~SendAccount()
{
    close();
}
bool SendAccount::grant(CreditCounters additional)
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    const auto held = add(sub(granted_, used_), inflight_);
    if (closed_ || !fits(additional, sub(pool_->session_limit, held)) ||
        !fits(additional, sub(pool_->limit, pool_->occupied)))
        return false;
    const auto next = add(granted_, additional);
    pool_->occupied = add(pool_->occupied, additional);
    granted_ = next;
    return true;
}
bool SendAccount::take(CreditCounters cost)
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (closed_ || !fits(cost, sub(granted_, used_)))
        return false;
    used_ = add(used_, cost);
    inflight_ = add(inflight_, cost); pool_->inflight = add(pool_->inflight, cost);
    return true;
}
void SendAccount::finish(CreditCounters cost)
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    const auto remaining = sub(inflight_, cost);
    if (!closed_ &&
        (granted_.bytes > UINT64_MAX - cost.bytes || granted_.records > UINT64_MAX - cost.records))
    {
        pool_->occupied = sub(pool_->occupied, sub(granted_, used_));
        closed_ = true;
    }
    if (closed_)
        pool_->occupied = sub(pool_->occupied, cost);
    else
        granted_ = add(granted_, cost);
    inflight_ = remaining; pool_->inflight = sub(pool_->inflight, cost);
}
void SendAccount::close()
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (!closed_)
    {
        pool_->occupied = sub(pool_->occupied, sub(granted_, used_));
        closed_ = true;
    }
}
CreditCounters SendAccount::granted() const
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    return granted_;
}
CreditCounters SendAccount::inflight() const
{
    std::lock_guard<std::mutex> lock(pool_->mutex);
    return inflight_;
}
OutboxRecord::~OutboxRecord()
{
    release();
}
OutboxRecord::OutboxRecord(OutboxRecord &&other) noexcept
{
    *this = std::move(other);
}
OutboxRecord &OutboxRecord::operator=(OutboxRecord &&other) noexcept
{
    if (this != &other)
    {
        release();
        header = other.header;
        blob = std::move(other.blob);
        network_cost = other.network_cost;
        account_ = std::move(other.account_);
    }
    return *this;
}
void OutboxRecord::release() noexcept
{
    blob = {};
    if (account_)
    {
        account_->finish(network_cost);
        account_.reset();
    }
}
struct OutboxReceiver::Impl
{
    WelcomeBody welcome;
    std::uint64_t session, epoch;
    local::Fd event;
    std::unique_ptr<ipc::route> channel;
    std::shared_ptr<SendAccount> account;
    CreditCounters progress;
};
OutboxReceiver::OutboxReceiver(const WelcomeBody &w, std::uint64_t session, std::uint64_t epoch,
                               int fd, std::shared_ptr<SendAccount> account)
    : impl_(new Impl)
{
    char target[128]{};
    const auto path = "/proc/self/fd/" + std::to_string(fd);
    const auto n = readlink(path.c_str(), target, sizeof(target) - 1);
    if (n <= 0 || std::string(target, n) != "anon_inode:[eventfd]" ||
        !(fcntl(fd, F_GETFL) & O_NONBLOCK))
        throw std::invalid_argument("ATTACH_TX 必须传递非阻塞 eventfd");
    if (!account || w.tx_name != outbox_name(w.locality, epoch, session))
        throw std::invalid_argument("出站接收身份无效");
    impl_->event.reset(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    if (!impl_->event)
        throw std::runtime_error("复制出站 eventfd 失败");
    impl_->welcome = w;
    impl_->session = session;
    impl_->epoch = epoch;
    impl_->account = std::move(account);
    impl_->channel = std::make_unique<ipc::route>(ipc::prefix{w.tx_name.c_str()}, w.tx_name.c_str(),
                                                  ipc::receiver, false);
    if (!impl_->channel->valid())
        throw std::runtime_error("打开出站接收 SHM 失败");
}
OutboxReceiver::~OutboxReceiver()
{
    impl_->channel.reset();
    ipc::route::clear_storage(ipc::prefix{impl_->welcome.tx_name.c_str()},
                              impl_->welcome.tx_name.c_str());
}
int OutboxReceiver::event_fd() const
{
    return impl_->event.get();
}
void OutboxReceiver::consume_notification()
{
    local::drain_event(impl_->event.get());
}
bool OutboxReceiver::pull(OutboxRecord &record)
{
    bool consumed = false;
    auto sample = impl_->channel->try_recv_loan(consumed);
    if (sample.empty())
    {
        if (consumed)
            throw std::runtime_error("出站记录已出队但未取得完整 loan");
        return false;
    }
    OutboxHeader h;
    ByteView payload;
    if (!decode_outbox({sample.data(), sample.size()}, h, payload,
                       impl_->welcome.max_message_bytes) ||
        h.session_id != impl_->session || h.gateway_epoch != impl_->epoch ||
        sample.size() != outbox_capacity(payload.size))
        throw std::runtime_error("出站记录损坏或会话代次不符");
    const auto cost = network_cost(payload.size);
    if (!impl_->account->take(cost))
        throw std::runtime_error("出站记录超出预授予网络信用");
    WireBlob blob;
    try
    {
        if (!WireEncoder::copy(payload, h.encoding, h.route.msg_id, h.schema_hash, blob))
            throw std::runtime_error("出站业务 blob 无效");
    }
    catch (...)
    {
        impl_->account->finish(cost);
        throw;
    }
    const auto actual = sample.size();
    sample = {}; // 必须在进度发布前释放 loan
    impl_->progress = add(impl_->progress, {actual, 1});
    record.release();
    record.header = h;
    record.blob = std::move(blob);
    record.network_cost = cost;
    record.account_ = impl_->account;
    return true;
}
CreditCounters OutboxReceiver::progress() const
{
    return impl_->progress;
}
} // namespace dzIPC::net
