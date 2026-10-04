#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "libipc/ipc.h"
#include "libipc/policy.h"
#include "libipc/queue.h"
#include "libipc/sniffer.h"

namespace {

struct message {
    std::uint32_t writer = 0;
    std::uint32_t sequence = 0;

    message() = default;
    message(std::uint32_t w, std::uint32_t s) : writer(w), sequence(s) {}
};

using flag_t = ipc::wr<ipc::relat::multi, ipc::relat::multi,
                       ipc::trans::broadcast>;
using policy_t = ipc::policy::choose<ipc::circ::elem_array, flag_t>;
using queue_t = ipc::queue<message, policy_t>;
using elems_t = queue_t::elems_t;

constexpr std::uint32_t kMessagesPerWriter = 2000;
constexpr auto kDeadline = std::chrono::seconds(5);
constexpr std::uint32_t kStressMessagesPerWriter = 64;
constexpr std::uint32_t kRingSlots = 256;

std::string unique_channel_name(const char *suffix) {
    static std::atomic<unsigned> sequence{0};
    return std::string("test_shm_mpmc_") + suffix + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

void send_messages(elems_t *elems, std::uint32_t writer,
                   std::atomic<bool> *failed) {
    queue_t tx{elems};
    if (!tx.ready_sending()) {
        failed->store(true, std::memory_order_release);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    for (std::uint32_t sequence = 0; sequence < kMessagesPerWriter;
         ++sequence) {
        auto prep = [writer, sequence](void *, ipc::circ::cc_t) {
            // Force a later writer to reserve and publish a slot first.  This
            // is the interleaving that used to make ct_ move backwards.
            if (writer == 0 && sequence == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return true;
        };
        while (!tx.push(prep, writer, sequence)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                failed->store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    }
}

bool receive_messages(queue_t *rx, std::size_t expected,
                      std::vector<std::vector<std::uint32_t>> *counts,
                      std::atomic<bool> *failed) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    std::size_t count = 0;
    std::vector<std::uint32_t> next_sequence(counts->size());
    while (count < expected && std::chrono::steady_clock::now() < deadline) {
        message msg;
        if (!rx->pop(msg)) {
            std::this_thread::yield();
            continue;
        }
        if (msg.writer >= counts->size() ||
            msg.sequence >= kMessagesPerWriter) {
            failed->store(true, std::memory_order_release);
            continue;
        }
        if (msg.sequence != next_sequence[msg.writer]) {
            failed->store(true, std::memory_order_release);
        } else {
            ++next_sequence[msg.writer];
        }
        ++(*counts)[msg.writer][msg.sequence];
        ++count;
    }
    if (count != expected) {
        failed->store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void send_stress_messages(elems_t *elems, std::uint32_t writer,
                          std::atomic<bool> *failed) {
    queue_t tx{elems};
    if (!tx.ready_sending()) {
        failed->store(true, std::memory_order_release);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    auto prep = [](void *, ipc::circ::cc_t) { return true; };
    for (std::uint32_t sequence = 0; sequence < kStressMessagesPerWriter;
         ++sequence) {
        while (!tx.push(prep, writer, sequence)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                failed->store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    }
}

bool receive_stress_messages(queue_t *rx, std::size_t expected,
                             std::size_t writers,
                             std::atomic<bool> *failed) {
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    std::vector<std::uint32_t> next_sequence(writers);
    std::vector<std::vector<std::uint32_t>> counts(
        writers, std::vector<std::uint32_t>(kStressMessagesPerWriter));
    std::size_t count = 0;
    while (count < expected && std::chrono::steady_clock::now() < deadline) {
        message msg;
        if (!rx->pop(msg)) {
            std::this_thread::yield();
            continue;
        }
        if (msg.writer >= writers || msg.sequence >= kStressMessagesPerWriter ||
            msg.sequence != next_sequence[msg.writer]) {
            failed->store(true, std::memory_order_release);
        } else {
            ++next_sequence[msg.writer];
        }
        if (msg.writer < writers && msg.sequence < kStressMessagesPerWriter)
            ++counts[msg.writer][msg.sequence];
        ++count;
    }
    if (count != expected)
        failed->store(true, std::memory_order_release);
    for (const auto &writer : counts)
        for (auto seen : writer)
            if (seen != 1)
                failed->store(true, std::memory_order_release);
    return count == expected;
}

TEST(ShmMpmcChannel, TwoWritersOneReader) {
    elems_t elems{};
    queue_t rx{&elems};
    ASSERT_TRUE(rx.connect());

    std::atomic<bool> failed{false};
    std::vector<std::vector<std::uint32_t>> received(
        2, std::vector<std::uint32_t>(kMessagesPerWriter));
    std::vector<std::thread> writers;
    writers.emplace_back(send_messages, &elems, 0, &failed);
    writers.emplace_back(send_messages, &elems, 1, &failed);

    const bool received_all = receive_messages(
        &rx, 2 * kMessagesPerWriter, &received, &failed);
    for (auto &writer : writers) writer.join();
    ASSERT_TRUE(received_all);
    ASSERT_FALSE(failed.load(std::memory_order_acquire));
    for (auto const &writer : received)
        for (auto count : writer) EXPECT_EQ(count, 1u);
    EXPECT_TRUE(rx.disconnect());
}

TEST(ShmMpmcChannel, TwoWritersTwoReadersBroadcast) {
    elems_t elems{};
    queue_t rx0{&elems};
    queue_t rx1{&elems};
    ASSERT_TRUE(rx0.connect());
    ASSERT_TRUE(rx1.connect());

    std::atomic<bool> failed{false};
    std::vector<std::vector<std::uint32_t>> received0(
        2, std::vector<std::uint32_t>(kMessagesPerWriter));
    std::vector<std::vector<std::uint32_t>> received1(
        2, std::vector<std::uint32_t>(kMessagesPerWriter));

    std::thread reader0([&] {
        receive_messages(&rx0, 2 * kMessagesPerWriter, &received0, &failed);
    });
    std::thread reader1([&] {
        receive_messages(&rx1, 2 * kMessagesPerWriter, &received1, &failed);
    });

    std::thread writer0(send_messages, &elems, 0, &failed);
    std::thread writer1(send_messages, &elems, 1, &failed);
    writer0.join();
    writer1.join();
    reader0.join();
    reader1.join();

    ASSERT_FALSE(failed.load(std::memory_order_acquire));
    for (auto const *received : {&received0, &received1})
        for (auto const &writer : *received)
            for (auto count : writer) EXPECT_EQ(count, 1u);
    EXPECT_TRUE(rx0.disconnect());
    EXPECT_TRUE(rx1.disconnect());
}

TEST(ShmMpmcChannel, EightAndThirtyTwoWritersKeepPerWriterOrder) {
    for (const std::size_t writer_count : {std::size_t{8}, std::size_t{32}}) {
        elems_t elems{};
        queue_t rx{&elems};
        ASSERT_TRUE(rx.connect());
        std::atomic<bool> failed{false};
        std::vector<std::thread> writers;
        writers.reserve(writer_count);
        for (std::size_t writer = 0; writer < writer_count; ++writer)
            writers.emplace_back(send_stress_messages, &elems,
                                 static_cast<std::uint32_t>(writer), &failed);
        const bool received = receive_stress_messages(
            &rx, writer_count * kStressMessagesPerWriter, writer_count, &failed);
        for (auto &writer : writers)
            writer.join();
        ASSERT_TRUE(received);
        ASSERT_FALSE(failed.load(std::memory_order_acquire));
        EXPECT_TRUE(rx.disconnect());
    }
}

TEST(ShmMpmcChannel, FullRingRejectsAndReusesReleasedSlot) {
    elems_t elems{};
    queue_t tx{&elems};
    queue_t rx{&elems};
    ASSERT_TRUE(rx.connect());
    ASSERT_TRUE(tx.ready_sending());

    auto prep = [](void *, ipc::circ::cc_t) { return true; };
    for (std::uint32_t sequence = 0; sequence < kRingSlots; ++sequence)
        ASSERT_TRUE(tx.push(prep, 7u, sequence));
    EXPECT_FALSE(tx.push(prep, 7u, kRingSlots));

    message first;
    ASSERT_TRUE(rx.pop(first));
    EXPECT_EQ(first.writer, 7u);
    EXPECT_EQ(first.sequence, 0u);
    EXPECT_TRUE(tx.push(prep, 7u, kRingSlots));
    EXPECT_TRUE(rx.disconnect());
}

TEST(ShmMpmcChannel, SenderWaitsForReaderConnection) {
    const auto name = unique_channel_name("delayed_reader");
    ipc::mpmc_channel::clear_storage(name.c_str());
    ipc::mpmc_channel sender{name.c_str(), ipc::sender, false};
    ASSERT_TRUE(sender.valid());
    std::atomic<bool> failed{false};
    std::thread reader([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ipc::mpmc_channel receiver{name.c_str(), ipc::receiver, false};
        if (!receiver.valid()) {
            failed.store(true, std::memory_order_release);
            return;
        }
        const auto received = receiver.recv(2000);
        if (received.size() != sizeof(std::uint32_t) * 2) {
            failed.store(true, std::memory_order_release);
            return;
        }
        std::array<std::uint32_t, 2> payload{};
        std::memcpy(payload.data(), received.data(), sizeof(payload));
        if (payload != std::array<std::uint32_t, 2>{7, 11})
            failed.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(sender.wait_for_recv(1, 2000));
    const std::array<std::uint32_t, 2> payload{7, 11};
    ASSERT_TRUE(sender.try_send(payload.data(), sizeof(payload), 1000));
    reader.join();
    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    ipc::mpmc_channel::clear_storage(name.c_str());
}

TEST(ShmMpmcChannel, SharedMemoryTwoSendersTwoReaders) {
    const auto name = unique_channel_name("broadcast");
    ipc::mpmc_channel::clear_storage(name.c_str());

    ipc::mpmc_channel sender0{name.c_str(), ipc::sender, false};
    ipc::mpmc_channel sender1{name.c_str(), ipc::sender, false};
    ASSERT_TRUE(sender0.valid());
    ASSERT_TRUE(sender1.valid());
    ipc::mpmc_channel reader0{name.c_str(), ipc::receiver, false};
    ipc::mpmc_channel reader1{name.c_str(), ipc::receiver, false};
    ASSERT_TRUE(reader0.valid());
    ASSERT_TRUE(reader1.valid());
    ASSERT_EQ(sender0.recv_count(), 2u);

    constexpr std::uint32_t kPerSender = 100;
    std::atomic<bool> failed{false};
    auto send = [&](ipc::mpmc_channel *sender, std::uint32_t writer) {
        for (std::uint32_t sequence = 0; sequence < kPerSender; ++sequence) {
            std::array<std::uint32_t, 2> payload{writer, sequence};
            const auto deadline = std::chrono::steady_clock::now() + kDeadline;
            while (!sender->try_send(payload.data(), sizeof(payload), 100)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    };
    auto receive = [&](ipc::mpmc_channel *reader) {
        std::vector<std::vector<std::uint32_t>> seen(
            2, std::vector<std::uint32_t>(kPerSender));
        std::vector<std::uint32_t> next(2);
        std::size_t total = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (total < 2 * kPerSender &&
               std::chrono::steady_clock::now() < deadline) {
            auto payload = reader->try_recv();
            if (payload.empty()) {
                std::this_thread::yield();
                continue;
            }
            if (payload.size() != sizeof(std::uint32_t) * 2) {
                failed.store(true, std::memory_order_release);
                continue;
            }
            std::array<std::uint32_t, 2> value{};
            std::memcpy(value.data(), payload.data(), sizeof(value));
            if (value[0] >= 2 || value[1] >= kPerSender ||
                value[1] != next[value[0]]) {
                failed.store(true, std::memory_order_release);
            } else {
                ++next[value[0]];
            }
            ++seen[value[0]][value[1]];
            ++total;
        }
        if (total != 2 * kPerSender)
            failed.store(true, std::memory_order_release);
        for (auto const &writer : seen)
            for (auto count : writer)
                if (count != 1) failed.store(true, std::memory_order_release);
    };

    std::thread reader_thread0(receive, &reader0);
    std::thread reader_thread1(receive, &reader1);
    std::thread sender_thread0(send, &sender0, 0);
    std::thread sender_thread1(send, &sender1, 1);
    sender_thread0.join();
    sender_thread1.join();
    reader_thread0.join();
    reader_thread1.join();
    EXPECT_FALSE(failed.load(std::memory_order_acquire));

    ipc::mpmc_channel::clear_storage(name.c_str());
}

TEST(ShmMpmcChannel, SharedMemoryLoanPublishAndDiscard) {
    const auto name = unique_channel_name("loan");
    ipc::mpmc_channel::clear_storage(name.c_str());
    ipc::mpmc_channel sender{name.c_str(), ipc::sender, false};
    ipc::mpmc_channel reader{name.c_str(), ipc::receiver, false};
    ASSERT_TRUE(sender.valid());
    ASSERT_TRUE(reader.valid());

    auto discarded = sender.loan(32);
    ASSERT_TRUE(discarded.valid());
    sender.discard_loan(discarded);

    auto loan = sender.loan(32);
    ASSERT_TRUE(loan.valid());
    const char text[] = "mpmc-loan";
    std::memcpy(loan.data, text, sizeof(text));
    ASSERT_TRUE(sender.publish_loan(loan, 1000));
    auto received = reader.recv(1000);
    ASSERT_EQ(received.size(), loan.size);
    EXPECT_EQ(std::memcmp(received.data(), text, sizeof(text)), 0);

    ipc::mpmc_channel::clear_storage(name.c_str());
}

TEST(ShmMpmcChannel, SnifferReadsVersionedMpmcChannel)
{
    const auto name = unique_channel_name("sniffer");
    ipc::mpmc_channel::clear_storage(name.c_str());
    ipc::mpmc_channel sender0{name.c_str(), ipc::sender, false};
    ipc::mpmc_channel sender1{name.c_str(), ipc::sender, false};
    ASSERT_TRUE(sender0.valid());
    ASSERT_TRUE(sender1.valid());

    ipc::sniffer observer;
    ASSERT_TRUE(observer.open(name.c_str(), ipc::sniffer::topology::channel));
    EXPECT_EQ(observer.receiver_connections(), 0u)
        << "sniffer 必须是被动观察者，不应占用 MPMC receiver 槽位";
    observer.skip_to_latest();
    EXPECT_TRUE(observer.try_recv().empty());

    const std::array<std::uint32_t, 2> first{11, 1};
    const std::array<std::uint32_t, 2> second{22, 1};
    ASSERT_TRUE(sender0.no_member_try_send(first.data(), sizeof(first), 1000));
    ASSERT_TRUE(sender1.no_member_try_send(second.data(), sizeof(second), 1000));

    std::set<std::uint32_t> writers;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writers.size() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        auto payload = observer.try_recv();
        if (payload.size() != sizeof(first))
        {
            std::this_thread::yield();
            continue;
        }
        std::array<std::uint32_t, 2> value{};
        std::memcpy(value.data(), payload.data(), sizeof(value));
        EXPECT_EQ(value[1], 1u);
        writers.insert(value[0]);
    }
    EXPECT_EQ(writers, (std::set<std::uint32_t>{11, 22}));
    EXPECT_EQ(observer.receiver_connections(), 0u);
    ipc::mpmc_channel::clear_storage(name.c_str());
}

} // namespace
