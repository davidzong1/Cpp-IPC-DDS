#pragma once
#include "dzIPC/net/outbox.h"
#include "local_control_linux.h"
#include <atomic>
#include <functional>

namespace dzIPC::net
{
// 初始化线程只创建映射，drain 线程独占接收者；控制线程只交换有界事件。
struct OutboxAttachment
{
    WelcomeBody welcome;
    std::uint64_t session = 0, epoch = 0, request = 0;
    local::Fd event;
    std::shared_ptr<SendAccount> account;
    std::atomic<bool> cancelled{false};
};
struct OutboxEvent
{
    enum class Kind
    {
        Ready,
        Record,
        Progress,
        Error
    } kind;
    std::shared_ptr<OutboxAttachment> attachment;
    OutboxRecord record;
    CreditCounters progress;
};
class OutboxService
{
  public:
    OutboxService(int control_wake, std::size_t init_limit, std::size_t event_limit);
    ~OutboxService();
    bool attach(std::shared_ptr<OutboxAttachment>);
    bool initialize(std::function<void()> task);
    void cancel(const std::shared_ptr<OutboxAttachment> &);
    bool pop(OutboxEvent &);
    bool healthy() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net
