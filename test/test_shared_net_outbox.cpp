#include "shared_net/outbox_fixture.h"
#include "gtest/gtest.h"
#include <atomic>
#include <set>
#include <sys/wait.h>
using namespace shared_net_test;
TEST(SharedNetOutbox, AttachRequiredAndIndependentCredits)
{
    OutboxFixture f({64, 1});
    const auto bytes = flat_blob(40);
    EXPECT_EQ(f.sender->submit(outbox_header(), ByteView(bytes)).state, SubmitState::NotSubmitted);
    f.attach();
    ASSERT_EQ(f.sender->submit(outbox_header(), ByteView(bytes)).state, SubmitState::Committed);
    OutboxRecord record;
    ASSERT_TRUE(f.receiver->pull(record));
    f.update();
    EXPECT_EQ(f.sender->usage().released.bytes, 1024u);
    EXPECT_EQ(f.account->inflight().bytes, 64u);
    EXPECT_EQ(f.sender->submit(outbox_header(2), ByteView(bytes)).code,
              OutboxCode::CreditUnavailable);
    record.release();
    f.update();
    EXPECT_EQ(f.sender->submit(outbox_header(2), ByteView(bytes)).state, SubmitState::Committed);
    ASSERT_TRUE(f.receiver->pull(record));
    record.release();
    f.update();
    EXPECT_EQ(f.sender->usage().used.records, 2u);
    EXPECT_EQ(f.account->inflight().records, 0u);
    const auto grants = f.account->granted();
    f.sender->grant(grants);
    f.sender->grant(grants);
    EXPECT_EQ(f.sender->usage().granted.bytes, grants.bytes);
}
TEST(SharedNetOutbox, EveryLoanClassBoundaryPreservesLengthAndBytes)
{
    OutboxFixture f;
    f.attach();
    std::set<std::size_t> sizes{40, 1023, 1024, 1025, kMaxMessageBytes};
    for (std::size_t boundary = 1024; boundary <= 65536; boundary += 1024)
        for (int delta : {-1, 0, 1})
            sizes.insert(boundary - 112 + delta);
    for (std::size_t boundary = 131072; boundary <= kMaxMessageBytes; boundary *= 2)
        for (int delta : {-1, 0, 1})
            sizes.insert(boundary - 112 + delta);
    std::uint64_t sequence = 0;
    for (auto size : sizes)
    {
        const auto bytes = flat_blob(size, ++sequence);
        ASSERT_EQ(f.sender->submit(outbox_header(sequence), ByteView(bytes)).state,
                  SubmitState::Committed)
            << size;
        OutboxRecord record;
        ASSERT_TRUE(f.receiver->pull(record));
        ASSERT_EQ(record.blob.size(), bytes.size());
        EXPECT_EQ(std::memcmp(record.blob.view().data, bytes.data(), bytes.size()), 0);
        EXPECT_EQ(record.blob.capacity(), ((size + 63) / 64) * 64);
        record.release();
        f.update();
    }
    const Bytes invalid(kMaxMessageBytes + 1);
    EXPECT_EQ(f.sender->submit(outbox_header(++sequence), ByteView(invalid)).code,
              OutboxCode::Invalid);
}
TEST(SharedNetOutbox, CreditWindowNeverOverwritesUnreadRing)
{
    OutboxFixture f;
    f.attach();
    for (unsigned i = 0; i < 256; ++i)
    {
        auto blob = flat_blob((i / 10 + 1) * 1024 - 112, i + 1);
        ASSERT_EQ(f.sender->submit(outbox_header(i + 1), ByteView(blob)).state,
                  SubmitState::Committed)
            << i;
    }
    auto extra = flat_blob(30 * 1024);
    EXPECT_EQ(f.sender->submit(outbox_header(257), ByteView(extra)).code,
              OutboxCode::CreditUnavailable);
    f.receiver->consume_notification(); // 一次读取合并通知后仍须消费所有记录。
    for (unsigned i = 0; i < 256; ++i)
    {
        OutboxRecord record;
        ASSERT_TRUE(f.receiver->pull(record));
        ASSERT_EQ(record.header.sequence, i + 1);
        const auto expected = flat_blob((i / 10 + 1) * 1024 - 112, i + 1);
        ASSERT_EQ(record.blob.size(), expected.size());
        EXPECT_EQ(std::memcmp(record.blob.view().data, expected.data(), expected.size()), 0);
    }
    f.update();
    OutboxRecord empty;
    EXPECT_FALSE(f.receiver->pull(empty));
    EXPECT_EQ(f.sender->usage().released.records, 256u);
}
TEST(SharedNetOutbox, ProcessTransferIsWholeAndSingle)
{
    OutboxFixture f;
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair), 0);
    local::Fd parent(pair[0]), child(pair[1]);
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (!pid)
    {
        parent.reset();
        try
        {
            f.attach();
            const std::uint64_t ready = 0;
            ::send(child.get(), &ready, sizeof(ready), MSG_NOSIGNAL);
            for (unsigned i = 1; i <= 200; ++i)
            {
                OutboxRecord record;
                if (!until([&] { return f.receiver->pull(record); }))
                    _exit(2);
                auto expected = flat_blob(4096, i);
                if (record.header.sequence != i || record.blob.size() != expected.size() ||
                    std::memcmp(record.blob.view().data, expected.data(), expected.size()))
                    _exit(3);
                record.release();
                const auto progress = f.receiver->progress();
                ::send(child.get(), &progress, sizeof(progress), MSG_NOSIGNAL);
            }
            OutboxRecord empty;
            if (f.receiver->pull(empty))
                _exit(4);
            f.receiver.reset();
            _exit(0);
        }
        catch (...)
        {
            _exit(5);
        }
    }
    ChildGuard guard(pid);
    child.reset();
    std::uint64_t ready = 1;
    ASSERT_EQ(::recv(parent.get(), &ready, sizeof(ready), 0), sizeof(ready));
    f.sender->ready();
    for (unsigned i = 1; i <= 200; ++i)
    {
        const auto bytes = flat_blob(4096, i);
        ASSERT_EQ(f.sender->submit(outbox_header(i), ByteView(bytes)).state,
                  SubmitState::Committed);
        CreditCounters progress;
        ASSERT_EQ(::recv(parent.get(), &progress, sizeof(progress), 0), sizeof(progress));
        f.sender->progress(progress);
        f.sender->grant({f.welcome.granted_bytes + i * 4096, f.welcome.granted_records + i});
    }
    int state = 0;
    ASSERT_EQ(waitpid(pid, &state, 0), pid);
    guard.pid = -1;
    ASSERT_TRUE(WIFEXITED(state));
    EXPECT_EQ(WEXITSTATUS(state), 0);
    EXPECT_EQ(f.sender->usage().released.records, 200u);
}
TEST(SharedNetOutbox, MultiplePublisherThreadsShareOneChannel)
{
    OutboxFixture f;
    f.attach();
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < 8; ++w)
        workers.emplace_back([&, w] {
            for (unsigned n = 0; n < 30; ++n)
            {
                const auto sequence = w * 30 + n + 1;
                auto data = flat_blob(4096, sequence);
                if (!until([&] {
                        return f.sender->submit(outbox_header(sequence), ByteView(data)).state ==
                               SubmitState::Committed;
                    }))
                    ++failures;
            }
        });
    std::set<std::uint64_t> seen;
    while (seen.size() < 240)
    {
        OutboxRecord record;
        const auto arrived = until([&] { return f.receiver->pull(record); });
        if (!arrived)
        {
            ++failures;
            break;
        }
        const auto expected = flat_blob(4096, record.header.sequence);
        EXPECT_EQ(std::memcmp(record.blob.view().data, expected.data(), expected.size()), 0);
        EXPECT_TRUE(seen.insert(record.header.sequence).second);
        record.release();
        f.update();
    }
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(failures.load(), 0u);
    EXPECT_EQ(seen.size(), 240u);
}
TEST(SharedNetOutbox, RuntimeAttachZeroCreditReplenishmentAndLastProgress)
{
    Directory dir;
    auto c = configuration(dir);
    c.limits.initial_send_bytes = c.limits.initial_send_records = 0;
    GatewayRuntime gateway(c);
    auto client = ClientRuntime::acquire(dir.control());
    client->attach_outbox();
    client->attach_outbox();
    auto h = outbox_header();
    h.session_id = client->session_id();
    h.gateway_epoch = client->gateway_epoch();
    h.delivery = Delivery::Reliable;
    h.deadline_monotonic_ns = local::monotonic_ns() + 2000000000ull;
    auto ticket = client->prepare_send(h.publisher_id, h.sequence);
    h.request_id = ticket.request_id;
    const auto data = flat_blob(2 * 1024 * 1024);
    ASSERT_EQ(client->submit_outbox(h, ByteView(data)).state, SubmitState::Committed);
    // T05 尚无路由：只验证直接 DZTX 请求得到明确终结和两份信用结算。
    EXPECT_EQ(client->wait_send(ticket, h.deadline_monotonic_ns).result, SendResultCode::Rejected);
    EXPECT_TRUE(until([&] { return client->released().records == 1; }));
    EXPECT_EQ(client->released().bytes, outbox_capacity(data.size()));
    EXPECT_TRUE(client->healthy());
    const auto before = client->released().records;
    h.deadline_monotonic_ns = 0;
    EXPECT_EQ(client->submit_outbox(h, ByteView(data)).state, SubmitState::NotSubmitted);
    EXPECT_EQ(client->released().records, before);
}
TEST(SharedNetOutbox, ClosedAccountRetainsInflightUntilBlobRelease)
{
    OutboxFixture f({128, 2});
    f.attach();
    auto data = flat_blob(40);
    ASSERT_EQ(f.sender->submit(outbox_header(), ByteView(data)).state, SubmitState::Committed);
    OutboxRecord record;
    ASSERT_TRUE(f.receiver->pull(record));
    f.account->close();
    EXPECT_EQ(f.budget.occupied().bytes, 64u);
    EXPECT_EQ(f.budget.occupied().records, 1u);
    EXPECT_FALSE(f.account->grant({64, 1}));
    record.release();
    EXPECT_EQ(f.budget.occupied().bytes, 0u);
}

TEST(SharedNetOutbox, SufficientCreditReliableSubmitNeedsNoControlReservation)
{
    Directory dir;
    GatewayRuntime gateway(configuration(dir));
    auto client = ClientRuntime::acquire(dir.control());
    client->attach_outbox();
    auto h = outbox_header();
    h.session_id = client->session_id();
    h.gateway_epoch = client->gateway_epoch();
    h.delivery = Delivery::Reliable;
    h.deadline_monotonic_ns = local::monotonic_ns() + 2000000000ull;
    auto ticket = client->prepare_send(h.publisher_id, h.sequence);
    h.request_id = ticket.request_id;
    const auto data = flat_blob(4096);
    ASSERT_EQ(client->submit_outbox(h, ByteView(data)).state, SubmitState::Committed);
    EXPECT_EQ(client->wait_send(ticket, h.deadline_monotonic_ns).result, SendResultCode::Rejected);
    EXPECT_NE(client->status().find("\"credit_requests\":\"0\""), std::string::npos);
}
