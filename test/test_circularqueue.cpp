#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include "dzIPC/common/circularqueue.h"

#include <gtest/gtest.h>

namespace {

struct QueueMsg
{
    explicit QueueMsg(int v)
        : value(v)
    {}

    int value;
};

}   // namespace

TEST(CircularQueue, FifoTryPop)
{
    CircularQueue<QueueMsg> queue(4);
    queue.push(std::make_shared<QueueMsg>(1));
    queue.push(std::make_shared<QueueMsg>(2));

    std::shared_ptr<QueueMsg> out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out->value, 1);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out->value, 2);
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(CircularQueue, DropOldestWhenFull)
{
    CircularQueue<QueueMsg> queue(3);
    queue.push(std::make_shared<QueueMsg>(1));
    queue.push(std::make_shared<QueueMsg>(2));
    queue.push(std::make_shared<QueueMsg>(3));
    queue.push(std::make_shared<QueueMsg>(4));

    std::shared_ptr<QueueMsg> out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out->value, 2);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out->value, 3);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out->value, 4);
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(CircularQueue, BlockingPopWakesOnPush)
{
    CircularQueue<QueueMsg> queue(2);
    std::shared_ptr<QueueMsg> out;
    std::atomic<bool> popped{false};

    std::thread consumer(
        [&]()
        {
            popped.store(queue.pop(out, 1000), std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queue.push(std::make_shared<QueueMsg>(42));
    consumer.join();

    ASSERT_TRUE(popped.load(std::memory_order_acquire));
    ASSERT_TRUE(out);
    EXPECT_EQ(out->value, 42);
}

TEST(CircularQueue, SpscStress)
{
    constexpr int kCount = 100000;
    CircularQueue<QueueMsg> queue(1024);
    std::atomic<bool> producer_done{false};
    std::atomic<int> received{0};

    std::thread producer(
        [&]()
        {
            for (int i = 0; i < kCount; ++i)
            {
                queue.push(std::make_shared<QueueMsg>(i));
            }
            producer_done.store(true, std::memory_order_release);
        });

    std::thread consumer(
        [&]()
        {
            std::shared_ptr<QueueMsg> out;
            while (!producer_done.load(std::memory_order_acquire) || queue.size() > 0)
            {
                if (queue.try_pop(out))
                {
                    ++received;
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        });

    producer.join();
    consumer.join();

    EXPECT_GT(received.load(), 0);
    EXPECT_LE(received.load(), kCount);
}

TEST(CircularQueue, MultiConsumerStress)
{
    constexpr int kCount = 20000;
    CircularQueue<QueueMsg> queue(kCount);
    std::atomic<bool> producer_done{false};
    std::atomic<int> received{0};
    std::atomic<int64_t> sum{0};

    std::thread producer(
        [&]()
        {
            for (int i = 0; i < kCount; ++i)
            {
                queue.push(std::make_shared<QueueMsg>(i));
            }
            producer_done.store(true, std::memory_order_release);
        });

    auto consume = [&]()
    {
        std::shared_ptr<QueueMsg> out;
        while (!producer_done.load(std::memory_order_acquire) || queue.size() > 0)
        {
            if (queue.try_pop(out))
            {
                sum.fetch_add(out->value, std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    };

    std::thread consumer_a(consume);
    std::thread consumer_b(consume);

    producer.join();
    consumer_a.join();
    consumer_b.join();

    EXPECT_EQ(received.load(), kCount);
    EXPECT_EQ(sum.load(), static_cast<int64_t>(kCount - 1) * kCount / 2);
}
