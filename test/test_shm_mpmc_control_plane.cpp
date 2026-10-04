#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <gtest/gtest.h>

#include "dzIPC/common/publisher_registry.h"
#include "libipc/shm.h"

namespace {

std::string registry_name(const char* suffix)
{
    static std::atomic<unsigned> sequence{0};
    return std::string("test_shm_mpmc_registry_") + suffix + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

using dzIPC::control_plane_shm::PublisherRegistry;

std::int64_t monotonic_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

TEST(ShmMpmcControlPlane, JoinIsSharedAndGenerationIsStable)
{
    const auto name = registry_name("join");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry first;
    PublisherRegistry second;
    ASSERT_TRUE(first.open(name));
    ASSERT_TRUE(second.open(name));

    const int first_slot = first.join(7, 1001, 111, 9001);
    const int second_slot = second.join(7, 1002, 222, 9002);
    ASSERT_GE(first_slot, 0);
    ASSERT_GE(second_slot, 0);
    EXPECT_EQ(first.generation(), 7u);
    EXPECT_EQ(second.generation(), 7u);
    EXPECT_EQ(first.publisher_count(), 2u);

    EXPECT_EQ(first.join(8, 1003, 333, 9003), -1);
    EXPECT_EQ(second.join(7, 1001, 999, 9004), -1)
        << "PID/start token mismatch must not reuse an active identity";
    EXPECT_EQ(first.join(7, 1001, 111, 9001), first_slot)
        << "same publisher instance should be idempotent";

    ASSERT_TRUE(first.heartbeat(first_slot, 1001, 9001));
    EXPECT_FALSE(first.heartbeat(first_slot, 1001, 9004));
    EXPECT_TRUE(first.leave(first_slot, 1001, 9001));
    EXPECT_EQ(second.publisher_count(), 1u);
    EXPECT_FALSE(second.leave(first_slot, 1001, 9001));

    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, CoordinatorLeaseTransfersAfterExpiry)
{
    const auto name = registry_name("lease");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry first;
    PublisherRegistry second;
    ASSERT_TRUE(first.open(name));
    ASSERT_TRUE(second.open(name));
    const int first_slot = first.join(3, 2001, 111, 9101);
    const int second_slot = second.join(3, 2002, 222, 9102);
    ASSERT_GE(first_slot, 0);
    ASSERT_GE(second_slot, 0);

    ASSERT_TRUE(first.acquire_coordinator(first_slot, 2001, 9101, 1'000'000));
    EXPECT_TRUE(first.is_coordinator(first_slot, 2001, 9101));
    EXPECT_FALSE(second.acquire_coordinator(second_slot, 2002, 9102, 1'000'000));
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    ASSERT_TRUE(second.acquire_coordinator(second_slot, 2002, 9102, 1'000'000));
    EXPECT_FALSE(first.is_coordinator(first_slot, 2001, 9101));
    EXPECT_TRUE(second.is_coordinator(second_slot, 2002, 9102));

    EXPECT_TRUE(first.leave(first_slot, 2001, 9101));
    EXPECT_TRUE(second.leave(second_slot, 2002, 9102));
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, StalePublisherIsReclaimedAndSlotCanBeReused)
{
    const auto name = registry_name("reap");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry registry;
    ASSERT_TRUE(registry.open(name));
    const int slot = registry.join(1, 3001, 123, 9201);
    ASSERT_GE(slot, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(registry.reap_stale(1'000'000), 1u);
    EXPECT_EQ(registry.publisher_count(), 0u);
    const int replacement = registry.join(1, 3002, 456, 9202);
    EXPECT_EQ(replacement, slot);
    EXPECT_TRUE(registry.leave(replacement, 3002, 9202));
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, ConcurrentFirstJoinUsesOneGeneration)
{
    const auto name = registry_name("concurrent");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry observer;
    ASSERT_TRUE(observer.open(name));

    constexpr int kPublishers = 8;
    std::vector<int> slots(kPublishers, -1);
    std::vector<std::thread> threads;
    for (int i = 0; i < kPublishers; ++i)
    {
        threads.emplace_back([&, i] {
            PublisherRegistry registry;
            if (!registry.open(name))
                return;
            slots[i] = registry.join(11, 4000 + i, 700 + i, 9300 + i);
        });
    }
    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(observer.generation(), 11u);
    EXPECT_EQ(observer.publisher_count(), static_cast<std::uint32_t>(kPublishers));
    for (auto slot : slots)
        EXPECT_GE(slot, 0);
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, ExistingIncompatibleLayoutIsRejected)
{
    const auto name = registry_name("incompatible");
    ipc::shm::handle::clear_storage(name.c_str());
    {
        ipc::shm::handle old_segment;
        ASSERT_TRUE(old_segment.acquire(name.c_str(), 64,
                                        ipc::shm::create | ipc::shm::open));
        ASSERT_GE(old_segment.size(), 64u);
        PublisherRegistry registry;
        EXPECT_FALSE(registry.open(name));
    }
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, FinalLeaveBlocksJoinUntilCloseCompletes)
{
    const auto name = registry_name("closing_gate");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry closer;
    PublisherRegistry joiner;
    ASSERT_TRUE(closer.open(name));
    ASSERT_TRUE(joiner.open(name));
    const int slot = closer.join(21, 5101, 501, 9501);
    ASSERT_GE(slot, 0);

    bool empty = false;
    ASSERT_TRUE(closer.leave(slot, 5101, 9501, &empty));
    ASSERT_TRUE(empty);

    std::atomic<bool> started{false};
    std::atomic<int> joined{-2};
    std::thread racing_join([&] {
        started.store(true, std::memory_order_release);
        joined.store(joiner.join(21, 5102, 502, 9502), std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(2'100));
    EXPECT_EQ(joined.load(std::memory_order_acquire), -2)
        << "活着的最后发布者仍持有关闭闸门时，join 不能因超时被接管";

    ASSERT_TRUE(closer.finish_close());
    racing_join.join();
    ASSERT_GE(joined.load(std::memory_order_acquire), 0);

    bool replacement_empty = false;
    ASSERT_TRUE(joiner.leave(joined.load(std::memory_order_acquire), 5102, 9502,
                             &replacement_empty));
    ASSERT_TRUE(replacement_empty);
    ASSERT_TRUE(joiner.finish_close());
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, PublisherSlotLimitIsExplicitAndReusable)
{
    const auto name = registry_name("slot_limit");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry registry;
    ASSERT_TRUE(registry.open(name));
    std::vector<int> slots;
    slots.reserve(dzIPC::control_plane_shm::kMaxPublisherSlots);
    for (std::uint32_t i = 0; i < dzIPC::control_plane_shm::kMaxPublisherSlots; ++i)
    {
        const int slot = registry.join(31, 6000 + i, 600 + static_cast<int>(i), 9600 + i);
        ASSERT_GE(slot, 0);
        slots.push_back(slot);
    }
    EXPECT_EQ(registry.publisher_count(), dzIPC::control_plane_shm::kMaxPublisherSlots);
    EXPECT_EQ(registry.join(31, 7000, 700, 9700), -1);

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        bool empty = false;
        ASSERT_TRUE(registry.leave(slots[i], 6000 + i, 9600 + i, &empty));
        if (i + 1 == slots.size())
        {
            ASSERT_TRUE(empty);
            ASSERT_TRUE(registry.finish_close());
        }
        else
        {
            EXPECT_FALSE(empty);
        }
    }
    const int replacement = registry.join(31, 7000, 700, 9700);
    ASSERT_GE(replacement, 0);
    bool empty = false;
    ASSERT_TRUE(registry.leave(replacement, 7000, 9700, &empty));
    ASSERT_TRUE(empty);
    ASSERT_TRUE(registry.finish_close());
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, MutationGateDoesNotStealLiveOwner)
{
    const auto name = registry_name("live_mutation_owner");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry registry;
    ASSERT_TRUE(registry.open(name));
    ipc::shm::handle view;
    ASSERT_TRUE(view.acquire(name.c_str(), sizeof(dzIPC::control_plane_shm::PublisherControl),
                             ipc::shm::open));
    auto* control = static_cast<dzIPC::control_plane_shm::PublisherControl*>(view.get());
    ASSERT_NE(control, nullptr);
    control->mutation_owner.store(0x1234, std::memory_order_release);
    control->mutation_since_ns.store(monotonic_now_ns() - 3'000'000'000LL,
                                     std::memory_order_release);
    control->mutation_pid.store(static_cast<std::int32_t>(::getpid()),
                                std::memory_order_release);
    control->mutation_start_token.store(0, std::memory_order_release);

    std::atomic<int> joined{-2};
    std::thread waiter([&] { joined.store(registry.join(41, 8101, 81, 9801),
                                          std::memory_order_release); });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(joined.load(std::memory_order_acquire), -2)
        << "活进程持有锁时不能仅凭过期时间戳抢占";

    control->mutation_since_ns.store(0, std::memory_order_release);
    control->mutation_pid.store(0, std::memory_order_release);
    control->mutation_start_token.store(0, std::memory_order_release);
    control->mutation_owner.store(0, std::memory_order_release);
    waiter.join();
    ASSERT_GE(joined.load(std::memory_order_acquire), 0);
    bool empty = false;
    ASSERT_TRUE(registry.leave(joined.load(std::memory_order_acquire), 8101, 9801, &empty));
    ASSERT_TRUE(empty);
    ASSERT_TRUE(registry.finish_close());
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, MutationGateReclaimsDeadOwnerAndTransition)
{
    const auto name = registry_name("dead_mutation_owner");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry registry;
    ASSERT_TRUE(registry.open(name));
    ipc::shm::handle view;
    ASSERT_TRUE(view.acquire(name.c_str(), sizeof(dzIPC::control_plane_shm::PublisherControl),
                             ipc::shm::open));
    auto* control = static_cast<dzIPC::control_plane_shm::PublisherControl*>(view.get());
    ASSERT_NE(control, nullptr);
    control->mutation_owner.store(0x5678, std::memory_order_release);
    control->mutation_since_ns.store(monotonic_now_ns() - 3'000'000'000LL,
                                     std::memory_order_release);
    control->mutation_pid.store(99'999'999, std::memory_order_release);
    control->mutation_start_token.store(1, std::memory_order_release);
    const int first = registry.join(42, 8201, 82, 9901);
    ASSERT_GE(first, 0);
    bool empty = false;
    ASSERT_TRUE(registry.leave(first, 8201, 9901, &empty));
    ASSERT_TRUE(empty);
    ASSERT_TRUE(registry.finish_close());

    control->mutation_owner.store(0x4000000000000000ULL |
                                      (static_cast<std::uint64_t>(monotonic_now_ns() -
                                                                   3'000'000'000LL) &
                                       0x3fffffffffffffffULL),
                                  std::memory_order_release);
    control->mutation_since_ns.store(0, std::memory_order_release);
    control->mutation_pid.store(99'999'999, std::memory_order_release);
    control->mutation_start_token.store(1, std::memory_order_release);
    const int second = registry.join(42, 8202, 82, 9902);
    ASSERT_GE(second, 0);
    ASSERT_TRUE(registry.leave(second, 8202, 9902, &empty));
    ASSERT_TRUE(empty);
    ASSERT_TRUE(registry.finish_close());
    ipc::shm::handle::clear_storage(name.c_str());
}

TEST(ShmMpmcControlPlane, ClosingGateReclaimsDeadCloser)
{
    const auto name = registry_name("dead_closer");
    ipc::shm::handle::clear_storage(name.c_str());
    PublisherRegistry registry;
    ASSERT_TRUE(registry.open(name));
    ipc::shm::handle view;
    ASSERT_TRUE(view.acquire(name.c_str(), sizeof(dzIPC::control_plane_shm::PublisherControl),
                             ipc::shm::open));
    auto* control = static_cast<dzIPC::control_plane_shm::PublisherControl*>(view.get());
    ASSERT_NE(control, nullptr);
    control->closing_since_ns.store(monotonic_now_ns() - 3'000'000'000LL,
                                    std::memory_order_release);
    control->closing_pid.store(99'999'999, std::memory_order_release);
    control->closing_start_token.store(1, std::memory_order_release);
    control->closing.store(1, std::memory_order_release);
    const int slot = registry.join(43, 8301, 83, 9911);
    ASSERT_GE(slot, 0);
    bool empty = false;
    ASSERT_TRUE(registry.leave(slot, 8301, 9911, &empty));
    ASSERT_TRUE(empty);
    ASSERT_TRUE(registry.finish_close());
    ipc::shm::handle::clear_storage(name.c_str());
}

} // namespace
