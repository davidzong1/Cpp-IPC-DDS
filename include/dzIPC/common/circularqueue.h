#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
      : capacity_(capacity == 0 ? 1 : capacity), cells_(new Cell[capacity_])
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
      if (!try_dequeue(dropped))
      {
        std::this_thread::yield();
      }
    }
    cv_.notify_one();
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
  mutable std::mutex wait_mtx_;
  std::condition_variable cv_;
};
