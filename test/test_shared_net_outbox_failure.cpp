#include "libipc/mutex.h"
#include "shared_net/outbox_fixture.h"
#include "gtest/gtest.h"
#include <csignal>
#include <fcntl.h>
#include <future>
#include <sys/wait.h>
using namespace shared_net_test;
TEST(SharedNetOutboxFailure, NoReceiverAndPoolExhaustionNeverPublish)
{
    OutboxFixture f;
    f.sender->ready();
    const auto data = flat_blob(40);
    EXPECT_EQ(f.sender->submit(outbox_header(), ByteView(data)).code, OutboxCode::LoanUnavailable);
    EXPECT_EQ(f.sender->usage().used.records, 0u);
    f.attach();
    for (unsigned i = 1; i <= 10; ++i)
        ASSERT_EQ(f.sender->submit(outbox_header(i), ByteView(data)).state, SubmitState::Committed);
    EXPECT_EQ(f.sender->submit(outbox_header(11), ByteView(data)).code,
              OutboxCode::LoanUnavailable);
    EXPECT_EQ(f.sender->usage().reserved.records, 0u);
    EXPECT_EQ(f.sender->usage().used.records, 10u);
    for (unsigned i = 1; i <= 10; ++i)
    {
        OutboxRecord r;
        ASSERT_TRUE(f.receiver->pull(r));
        EXPECT_EQ(r.header.sequence, i);
    }
    OutboxRecord empty;
    EXPECT_FALSE(f.receiver->pull(empty));
}
TEST(SharedNetOutboxFailure, FullQueueStrictPublishFailsWithoutOverwriting)
{
    OutboxFixture f;
    // 直接底层通道，绕过上层信用限制验证严格提交的失败语义。
    f.sender.reset();
    ipc::route tx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::sender,
                  false);
    ipc::route rx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::receiver,
                  false);
    for (unsigned i = 0; i < 256; ++i)
    {
        auto loan = tx.loan((i / 10 + 1) * 1024);
        ASSERT_TRUE(loan.valid());
        std::memset(loan.data, static_cast<unsigned char>(i), loan.size);
        ASSERT_TRUE(tx.try_publish_loan(loan));
    }
    auto extra = tx.loan(30 * 1024);
    ASSERT_TRUE(extra.valid());
    EXPECT_FALSE(tx.try_publish_loan(extra));
    for (unsigned i = 0; i < 256; ++i)
    {
        auto sample = rx.try_recv();
        ASSERT_FALSE(sample.empty());
        const auto *data = static_cast<const unsigned char *>(sample.data());
        EXPECT_TRUE(std::all_of(data, data + sample.size(), [&](auto value) {
            return value == static_cast<unsigned char>(i);
        }));
    }
    EXPECT_TRUE(rx.try_recv().empty());
    auto again = tx.loan(30 * 1024);
    EXPECT_TRUE(again.valid());
}
TEST(SharedNetOutboxFailure, BorrowThenReceiverLeavesIsDefinitelyNotSubmitted)
{
    OutboxFixture f;
    f.sender.reset();
    ipc::route tx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::sender,
                  false);
    auto rx = std::make_unique<ipc::route>(ipc::prefix{f.welcome.tx_name.c_str()},
                                           f.welcome.tx_name.c_str(), ipc::receiver, false);
    auto loan = tx.loan(2048);
    ASSERT_TRUE(loan.valid());
    rx.reset();
    EXPECT_FALSE(tx.try_publish_loan(loan));
    rx = std::make_unique<ipc::route>(ipc::prefix{f.welcome.tx_name.c_str()},
                                      f.welcome.tx_name.c_str(), ipc::receiver, false);
    EXPECT_TRUE(rx->try_recv().empty());
}
TEST(SharedNetOutboxFailure, ProducerKilledDuringFillCannotExposePartialRecord)
{
    OutboxFixture f;
    f.sender.reset();
    ipc::route rx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::receiver,
                  false);
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    local::Fd ready(pipefd[0]), signal(pipefd[1]);
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (!pid)
    {
        ready.reset();
        ipc::route tx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(),
                      ipc::sender, false);
        auto loan = tx.loan(1024 * 1024);
        if (!loan.valid())
            _exit(2);
        std::memset(loan.data, 0x77, 5000);
        const char byte = 'x';
        if (write(signal.get(), &byte, 1) != 1)
            _exit(3);
        for (;;)
            pause();
    }
    ChildGuard guard(pid);
    signal.reset();
    char byte = 0;
    ASSERT_EQ(read(ready.get(), &byte, 1), 1);
    EXPECT_TRUE(rx.try_recv().empty());
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    int state = 0;
    ASSERT_EQ(waitpid(pid, &state, 0), pid);
    guard.pid = -1;
    EXPECT_TRUE(WIFSIGNALED(state));
    EXPECT_TRUE(rx.try_recv().empty());
}
TEST(SharedNetOutboxFailure, InvalidDescriptorAndOverclaimedProgressFailClosed)
{
    OutboxFixture f;
    local::Fd file(open("/dev/null", O_RDONLY | O_CLOEXEC));
    EXPECT_THROW(OutboxReceiver receiver(f.welcome, 1, 2, file.get(), f.account),
                 std::invalid_argument);
    f.attach();
    EXPECT_THROW(f.sender->progress({1024, 1}), std::runtime_error);
    EXPECT_EQ(f.sender->submit(outbox_header(), ByteView(flat_blob(40))).state,
              SubmitState::NotSubmitted);
}
TEST(SharedNetOutboxFailure, GlobalAndSessionBudgetCapsAreConserved)
{
    SendBudget budget({1024, 4}, {512, 2});
    auto a = budget.open({512, 2}), b = budget.open({512, 2}), c = budget.open({512, 2});
    EXPECT_EQ(c->granted().bytes, 0u);
    EXPECT_FALSE(a->grant({1, 0}));
    EXPECT_FALSE(c->grant({1, 0}));
    ASSERT_TRUE(a->take({128, 1}));
    a->close();
    EXPECT_EQ(budget.occupied().bytes, 640u);
    EXPECT_TRUE(c->grant({384, 1}));
    a->finish({128, 1});
    EXPECT_EQ(budget.occupied().bytes, 896u);
    b->close();
    c->close();
    EXPECT_EQ(budget.occupied().bytes, 0u);
    EXPECT_EQ(budget.occupied().records, 0u);
}

TEST(SharedNetOutboxFailure, ReceiverCanConsumeBeforePublishReturns)
{
    OutboxFixture f;
    f.sender.reset();
    ipc::route tx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::sender,
                  false);
    ipc::route rx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::receiver,
                  false);
    ipc::sync::mutex barrier;
    const auto name =
        f.welcome.tx_name + "__IPC_SHM__RD_CONN__" + f.welcome.tx_name + "_WAITER_LOCK_";
    ASSERT_TRUE(barrier.open(name.c_str()));
    std::unique_lock<ipc::sync::mutex> held(barrier);
    auto loan = tx.loan(2048);
    ASSERT_TRUE(loan.valid());
    std::memset(loan.data, 0xa5, loan.size);
    auto publish = std::async(std::launch::async, [&] { return tx.try_publish_loan(loan); });
    ipc::buff_t sample;
    const bool received = until([&] {
        sample = rx.try_recv();
        return !sample.empty();
    });
    EXPECT_TRUE(received);
    EXPECT_EQ(publish.wait_for(0ms), std::future_status::timeout);
    if (received)
        EXPECT_TRUE(std::all_of(static_cast<unsigned char *>(sample.data()),
                                static_cast<unsigned char *>(sample.data()) + sample.size(),
                                [](auto value) { return value == 0xa5; }));
    sample = {};
    held.unlock();
    EXPECT_TRUE(publish.get());
    std::vector<ipc::loan_t> loans;
    for (unsigned i = 0; i < 10; ++i)
    {
        loans.push_back(tx.loan(2048));
        EXPECT_TRUE(loans.back().valid());
    }
    EXPECT_FALSE(tx.loan(2048).valid());
}
TEST(SharedNetOutboxFailure, EventfdPathNeverWaitsOnLegacyReadWriteNotifications)
{
    OutboxFixture f;
    f.attach();
    ipc::sync::mutex reader_lock, writer_lock;
    ASSERT_TRUE(reader_lock.open(
        (f.welcome.tx_name + "__IPC_SHM__RD_CONN__" + f.welcome.tx_name + "_WAITER_LOCK_")
            .c_str()));
    ASSERT_TRUE(writer_lock.open(
        (f.welcome.tx_name + "__IPC_SHM__WT_CONN__" + f.welcome.tx_name + "_WAITER_LOCK_")
            .c_str()));
    std::unique_lock<ipc::sync::mutex> reader(reader_lock), writer(writer_lock);
    const auto data = flat_blob(40);
    auto publish = std::async(std::launch::async,
                              [&] { return f.sender->submit(outbox_header(), ByteView(data)); });
    EXPECT_EQ(publish.wait_for(500ms), std::future_status::ready);
    reader.unlock();
    EXPECT_EQ(publish.get().state, SubmitState::Committed);
    OutboxRecord record;
    auto receive = std::async(std::launch::async, [&] { return f.receiver->pull(record); });
    EXPECT_EQ(receive.wait_for(500ms), std::future_status::ready);
    writer.unlock();
    EXPECT_TRUE(receive.get());
}
TEST(SharedNetOutboxFailure, StrictReceiverDistinguishesEmptyFromInvalidRecord)
{
    OutboxFixture f;
    f.sender.reset();
    ipc::route tx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::sender,
                  false);
    ipc::route rx(ipc::prefix{f.welcome.tx_name.c_str()}, f.welcome.tx_name.c_str(), ipc::receiver,
                  false);
    bool consumed = true;
    EXPECT_TRUE(rx.try_recv_loan(consumed).empty());
    EXPECT_FALSE(consumed);
    const char bytes[] = "非 loan 记录";
    ASSERT_TRUE(tx.send(bytes, sizeof(bytes), 0));
    EXPECT_TRUE(rx.try_recv_loan(consumed).empty());
    EXPECT_TRUE(consumed);
}
TEST(SharedNetOutboxFailure, GatewayKilledBeforeAndAfterCommitNeverReplaysOldSession)
{
    Directory dir;
    const auto c = configuration(dir);
    GatewayProcess gateway(c);
    ASSERT_TRUE(until([&] { return std::filesystem::exists(dir.control()); }));
    auto old = ClientRuntime::acquire(dir.control());
    old->attach_outbox();
    const auto tx_name = old->welcome().tx_name;
    ASSERT_EQ(kill(gateway.pid, SIGSTOP), 0);
    int state;
    ASSERT_EQ(waitpid(gateway.pid, &state, WUNTRACED), gateway.pid);
    ASSERT_TRUE(WIFSTOPPED(state));
    auto h = outbox_header();
    h.session_id = old->session_id();
    h.gateway_epoch = old->gateway_epoch();
    h.delivery = Delivery::Reliable;
    h.deadline_monotonic_ns = local::monotonic_ns() + 2000000000ull;
    auto ticket = old->prepare_send(h.publisher_id, h.sequence);
    h.request_id = ticket.request_id;
    const auto data = flat_blob(4096);
    ASSERT_EQ(old->submit_outbox(h, ByteView(data)).state, SubmitState::Committed);
    gateway.stop_now();
    EXPECT_EQ(old->wait_send(ticket, h.deadline_monotonic_ns).result, SendResultCode::GatewayLost);
    EXPECT_EQ(old->submit_outbox(h, ByteView(data)).state, SubmitState::NotSubmitted);
    GatewayProcess replacement(c);
    ASSERT_TRUE(until([&] {
        try
        {
            RawClient client(dir.control());
            return client.header.gateway_epoch != old->gateway_epoch();
        }
        catch (...)
        {
            return false;
        }
    }));
    auto next = ClientRuntime::acquire(dir.control());
    next->attach_outbox();
    EXPECT_NE(next->welcome().tx_name, tx_name);
    EXPECT_NE(next->status().find("\"outbox_records\":\"0\""), std::string::npos);
    old.reset();
}
