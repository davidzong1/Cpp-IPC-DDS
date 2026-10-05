#pragma once
/* F1/t35：队列淘汰的**唯一**写入点（见下方 push_owned 的 evict 分支）。
 * ⛔ 只读引用既有 ID `queue_evicted`；counters.h 是 header-only（无其它依赖、无环）。
 * 本头同时被 socket/shm 的 pub_sub/ser_cli 四个模块头包含 —— 引入 counters.h 只增
 * 一份 inline 声明，不改任何既有类型/布局（不新增成员、不改模板签名）。 */
#include "dzIPC/measure/counters.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
template <typename msgType>
class CircularQueue
{
public:
  using MsgPtr = std::shared_ptr<msgType>;
  explicit CircularQueue(size_t capacity)
      : capacity_(capacity <= 2 ? 2 : capacity), cells_(new Cell[capacity_])
  {
    for (size_t i = 0; i < capacity_; ++i)
    {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  void push(const MsgPtr &msg)
  {
    push_impl(msg);
  }

  void push(MsgPtr &&msg)
  {
    push_impl(std::move(msg));
  }

  bool try_pop(MsgPtr &out)
  {
    return try_dequeue(out);
  }

  /* 满队挤最老时的驱逐通知(可选)。
   *
   * 丢弃发生在 push 内部、不经过 try_pop —— 任何"队列内对象计数"(如
   * shm_sub_ipc 的 adopt 借样配额)若只钩 pop 不钩这里, 只会只涨不跌、永久退化。
   * 回调拿到的是**析构前**的 MsgPtr, 观察者可识别类别后自行递减。
   * ⛔ 必须在首次 push 之前设置一次; 之后只读, 无需同步。 */
  using EvictCb = std::function<void(MsgPtr &)>;
  void set_evict_cb(EvictCb cb) { evict_cb_ = std::move(cb); }
  void set_notify_cb(std::function<void()> cb) { notify_cb_ = std::move(cb); }

  // 在首次生产/消费前启用。共享 reader 直接使用队列的等待点，取消后不再复用。
  void enable_cancellable_wait() { cancellable_ = true; }
  void cancel_waits() {
    { std::lock_guard<std::mutex> lock(wait_mtx_); cancelled_ = true; }
    cv_.notify_all();
  }
  bool pop_cancellable(MsgPtr& out, std::uint64_t timeout) {
    const auto now = std::chrono::steady_clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - now).count();
    const auto end = timeout >= static_cast<std::uint64_t>(maximum) ? std::chrono::steady_clock::time_point::max() : now + std::chrono::milliseconds(timeout);
    std::unique_lock<std::mutex> lock(wait_mtx_);
    for (;;) {
      if (cancelled_) return false;
      if (try_pop(out)) return true;
      if (!timeout || cv_.wait_until(lock, end) == std::cv_status::timeout) {
        return !cancelled_ && try_pop(out);
      }
    }
  }

  bool pop(MsgPtr &out, uint64_t tm = std::numeric_limits<uint64_t>::max())
  {
    if (tm == std::numeric_limits<uint64_t>::max())
    {
      for (;;)
      {
        if (try_pop(out))
        {
          return true;
        }
        std::unique_lock<std::mutex> lk(wait_mtx_);
        cv_.wait(lk, [this] { return !empty(); });
      }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(tm);
    for (;;)
    {
      if (try_pop(out))
      {
        return true;
      }

      std::unique_lock<std::mutex> lk(wait_mtx_);
      if (!cv_.wait_until(lk, deadline, [this] { return !empty(); }))
      {
        return false;
      }
    }
  }

  size_t size() const
  {
    const size_t enqueue = enqueue_pos_.load(std::memory_order_acquire);
    const size_t dequeue = dequeue_pos_.load(std::memory_order_acquire);
    const size_t used = enqueue - dequeue;
    return used > capacity_ ? capacity_ : used;
  }

private:
  std::function<void()> notify_cb_;
  struct Cell
  {
    std::atomic<size_t> sequence{0};
    MsgPtr data;
  };

  bool empty() const
  {
    return enqueue_pos_.load(std::memory_order_acquire) == dequeue_pos_.load(std::memory_order_acquire);
  }

  void push_impl(const MsgPtr &msg)
  {
    MsgPtr pending = msg;
    push_owned(std::move(pending));
  }

  void push_impl(MsgPtr &&msg)
  {
    push_owned(std::move(msg));
  }

  void push_owned(MsgPtr msg)
  {
    while (!try_enqueue(msg))
    {
      MsgPtr dropped;
      if (try_dequeue(dropped))
      {
        /* ---- F1/t35：队列淘汰计数（§13.2#3 的"队列淘汰"独立计数）----
         * 位置在**驱逐成功之后、回调之前**：淘汰是"抢不到槽位而挤掉最老"的既成事实，
         * 与应用侧回调是否注册（evict_cb_ 为空也要计）无关 —— 只钩回调会让"没注册回调
         * 的队列"淘汰全不计数（正是本任务要修的漏计形态）。
         * 本处是**全仓唯一的队列淘汰点**（`try_enqueue` 失败的唯一处理分支）⇒ 满足
         * "每 ID 单个写入点"；热路径上多一次 relaxed fetch_add（队列满时才走到）。
         * 口径：淘汰**条数**（不是事件数）—— 每次挤掉最老的一条即 +1。 */
        ::dzIPC::measure::CounterRegistry::instance().inc(
            ::dzIPC::measure::CounterId::queue_evicted);
        if (evict_cb_)
        {
          evict_cb_(dropped);
        }
      }
      else
      {
        std::this_thread::yield();
      }
    }
    if (cancellable_) {
      // 与消费者的“空队列→等待”及取消共用同一把锁，不能在该窗口丢失通知。
      { std::lock_guard<std::mutex> lock(wait_mtx_); }
      // 释放等待锁后再唤醒，避免消费者醒来又阻塞于生产者仍持有的锁。
      cv_.notify_one();
    } else cv_.notify_one();
    if (notify_cb_) notify_cb_();
  }

  bool try_enqueue(MsgPtr &msg)
  {
    Cell *cell = nullptr;
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;)
    {
      cell = &cells_[pos % capacity_];
      const size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (diff == 0)
      {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        {
          break;
        }
      }
      else if (diff < 0)
      {
        return false;
      }
      else
      {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    cell->data = std::move(msg);
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_dequeue(MsgPtr &out)
  {
    Cell *cell = nullptr;
    size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;)
    {
      cell = &cells_[pos % capacity_];
      const size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
      if (diff == 0)
      {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        {
          break;
        }
      }
      else if (diff < 0)
      {
        return false;
      }
      else
      {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }

    out = std::move(cell->data);
    cell->data.reset();
    cell->sequence.store(pos + capacity_, std::memory_order_release);
    return true;
  }

  const size_t capacity_;
  std::unique_ptr<Cell[]> cells_;
  alignas(64) std::atomic<size_t> enqueue_pos_{0};
  alignas(64) std::atomic<size_t> dequeue_pos_{0};
  EvictCb evict_cb_;   /* 见 set_evict_cb; 构造后只读 */
  mutable std::mutex wait_mtx_;
  std::condition_variable cv_;
  bool cancellable_ = false, cancelled_ = false;
};
