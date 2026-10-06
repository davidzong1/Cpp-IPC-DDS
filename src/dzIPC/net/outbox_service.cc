#include "outbox_service.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <thread>

namespace dzIPC::net
{
struct OutboxService::Impl
{
    struct Reader
    {
        std::shared_ptr<OutboxAttachment> attachment;
        std::unique_ptr<OutboxReceiver> receiver;
        bool deferred = false;
    };
    int control_wake;
    std::size_t init_limit, event_limit;
    local::Fd wake = local::event();
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<OutboxAttachment>> init_tasks;
    std::deque<std::function<void()>> other_tasks;
    std::deque<Reader> initialized;
    std::deque<OutboxEvent> events;
    std::thread initializer, drainer;
    bool emit(OutboxEvent event) noexcept
    {
        const auto attachment = event.attachment;
        auto trace_metrics = event.kind == OutboxEvent::Kind::Record ? event.record.trace_metrics() : nullptr;
        if (trace_metrics && !trace_metrics->trace_enabled()) trace_metrics.reset();
        const auto trace_identity = trace_metrics ? trace_key(event.record.header) : MessageTraceKey{};
        std::uint64_t queued_ns = 0;
        try
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (events.size() >= event_limit)
            {
                event.attachment->cancelled.store(true);
                local::notify(control_wake);
                return false;
            }
            queued_ns = metric_now_ns();
            event.queued_ns = queued_ns;
            events.push_back(std::move(event));
            lock.unlock();
            if (trace_metrics) trace_metrics->trace(trace_identity, MessageTracePoint::DrainEventQueued, queued_ns);
            local::notify(control_wake);
            return true;
        }
        catch (...)
        {
            attachment->cancelled.store(true);
            local::notify(control_wake);
            return false;
        }
    }
    void initialize() noexcept
    {
        bool prefer_other = true;
        for (;;)
        {
            std::shared_ptr<OutboxAttachment> task;
            std::function<void()> other;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopped.load() || !init_tasks.empty() || !other_tasks.empty(); });
                if (stopped.load())
                    return;
                if (!other_tasks.empty() && (init_tasks.empty() || prefer_other)) { other = std::move(other_tasks.front()); other_tasks.pop_front(); prefer_other = false; }
                else { task = std::move(init_tasks.front()); init_tasks.pop_front(); prefer_other = true; }
            }
            if (other) {
                try { other(); }
                catch (...) { stopped.store(true); local::notify(wake.get()); }
                local::notify(control_wake); continue;
            }
            if (task->cancelled.load())
                continue;
            try
            {
                auto receiver = std::make_unique<OutboxReceiver>(
                    task->welcome, task->session, task->epoch, task->event.get(), task->account);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (initialized.size() >= init_limit)
                        throw std::runtime_error("出站初始化完成队列已满");
                    initialized.push_back({task, std::move(receiver), false});
                }
                local::notify(wake.get());
            }
            catch (...)
            {
                task->cancelled.store(true);
                OutboxEvent event;
                event.kind = OutboxEvent::Kind::Error;
                event.attachment = task;
                emit(std::move(event));
            }
        }
    }
    void drain() noexcept
    {
        try
        {
            std::map<std::uint64_t, Reader> readers;
            while (!stopped.load())
            {
                {
                    std::deque<Reader> ready;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ready.swap(initialized);
                    }
                    for (auto &reader : ready)
                    {
                        if (reader.attachment->cancelled.load())
                            continue;
                        auto a = reader.attachment;
                        readers.emplace(a->session, std::move(reader));
                        OutboxEvent event;
                        event.kind = OutboxEvent::Kind::Ready;
                        event.attachment = a;
                        emit(std::move(event));
                    }
                }
                std::vector<pollfd> fds{{wake.get(), POLLIN, 0}};
                bool deferred = false;
                for (auto i = readers.begin(); i != readers.end();)
                {
                    if (i->second.attachment->cancelled.load())
                    {
                        i = readers.erase(i);
                        continue;
                    }
                    fds.push_back({i->second.receiver->event_fd(), POLLIN, 0});
                    deferred |= i->second.deferred;
                    ++i;
                }
                const auto n = ::poll(fds.data(), fds.size(), deferred ? 0 : 100);
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    stopped.store(true);
                    changed.notify_all();
                    break;
                }
                if (fds[0].revents)
                    local::drain_event(wake.get());
                std::size_t index = 1;
                for (auto &entry : readers)
                {
                    auto &reader = entry.second;
                    const auto revents = fds[index++].revents;
                    if (reader.attachment->cancelled.load() || (!reader.deferred && !revents))
                        continue;
                    reader.receiver->consume_notification();
                    reader.deferred = false;
                    const auto before = reader.receiver->progress();
                    const auto deadline = local::monotonic_ns() + 200000;
                    try
                    {
                        std::size_t bytes = 0;
                        for (unsigned count = 0; count < 64; ++count)
                        {
                            OutboxEvent event;
                            event.kind = OutboxEvent::Kind::Record;
                            event.attachment = reader.attachment;
                            if (!reader.receiver->pull(event.record))
                                break;
                            bytes += event.record.blob.size();
                            if (!emit(std::move(event)))
                                break;
                            if (count == 63 || bytes >= 65536 || local::monotonic_ns() >= deadline)
                            {
                                reader.deferred = true;
                                break;
                            }
                        }
                        const auto progress = reader.receiver->progress();
                        if (progress.records != before.records)
                        {
                            OutboxEvent event;
                            event.kind = OutboxEvent::Kind::Progress;
                            event.attachment = reader.attachment;
                            event.progress = progress;
                            emit(std::move(event));
                        }
                    }
                    catch (...)
                    {
                        reader.attachment->cancelled.store(true);
                        OutboxEvent event;
                        event.kind = OutboxEvent::Kind::Error;
                        event.attachment = reader.attachment;
                        emit(std::move(event));
                    }
                }
            }
        }
        catch (...)
        {
            stopped.store(true);
            changed.notify_all();
            local::notify(control_wake);
        }
    }
};
OutboxService::OutboxService(int wake, std::size_t init_limit, std::size_t event_limit)
    : impl_(new Impl)
{
    impl_->control_wake = wake;
    impl_->init_limit = init_limit;
    impl_->event_limit = event_limit;
    impl_->initializer = std::thread([p = impl_.get()] { p->initialize(); });
    try
    {
        impl_->drainer = std::thread([p = impl_.get()] { p->drain(); });
    }
    catch (...)
    {
        impl_->stopped.store(true);
        impl_->changed.notify_all();
        impl_->initializer.join();
        throw;
    }
}
OutboxService::~OutboxService()
{
    impl_->stopped.store(true);
    impl_->changed.notify_all();
    local::notify(impl_->wake.get());
    if (impl_->initializer.joinable())
        impl_->initializer.join();
    if (impl_->drainer.joinable())
        impl_->drainer.join();
}
bool OutboxService::attach(std::shared_ptr<OutboxAttachment> attachment)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped.load() || impl_->init_tasks.size() + impl_->other_tasks.size() >= impl_->init_limit)
        return false;
    impl_->init_tasks.push_back(std::move(attachment));
    impl_->changed.notify_one();
    return true;
}
void OutboxService::cancel(const std::shared_ptr<OutboxAttachment> &a)
{
    if (a)
    {
        a->cancelled.store(true);
        local::notify(impl_->wake.get());
    }
}
bool OutboxService::initialize(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped.load() || impl_->init_tasks.size() + impl_->other_tasks.size() >= impl_->init_limit) return false;
    impl_->other_tasks.push_back(std::move(task)); impl_->changed.notify_one(); return true;
}
bool OutboxService::pop(OutboxEvent &event)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->events.empty())
        return false;
    event = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}
bool OutboxService::healthy() const noexcept
{
    return !impl_->stopped.load();
}
} // namespace dzIPC::net
