#include "shared_net/shm_wire_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetBridge, TlvAndFlatKeepExactBytesWithoutTransportEnvelope)
{
    BusinessTopic topic;
    ShmWireBridge bridge(topic.descriptor);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    dzIPC::GenericMessage message;
    message.set_msg_id(71);
    message.set_string("payload", std::string(5000, 'x'));
    WireBlob tlv;
    ASSERT_TRUE(WireEncoder::encode(message, false, tlv));
    ASSERT_EQ(bridge.try_commit(tlv), SubmitState::Committed);
    auto sample = rx.try_recv();
    ASSERT_EQ(sample.size(), tlv.size());
    EXPECT_EQ(std::memcmp(sample.data(), tlv.view().data, tlv.size()), 0);
    auto bytes = flat_blob(40);
    WireBlob flat;
    ASSERT_TRUE(WireEncoder::encode_prebuilt(ByteView(bytes), 71, 0, flat));
    ASSERT_EQ(bridge.try_commit(flat), SubmitState::Committed);
    sample = rx.try_recv();
    ASSERT_EQ(sample.size(), bytes.size());
    EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    EXPECT_TRUE(rx.try_recv().empty());
}
TEST(SharedNetBridge, ThreePublishersTwoProcessesAndInternalRegistration)
{
    BusinessTopic topic;
    auto ordinary =
        std::make_unique<dzIPC::shm::shm_pub_ipc>(topic.generic(), topic.descriptor.topic, 0);
    ordinary->InitChannel();
    ShmWireWriter writer(topic.descriptor);
    auto bridge = std::make_unique<ShmWireBridge>(topic.descriptor);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    dzIPC::control_plane_shm::PublisherRegistry registry;
    ASSERT_TRUE(registry.open(shm_topic_publisher_registry_name(topic.descriptor.topic, 0)));
    EXPECT_EQ(registry.publisher_count(), 3u);
    std::size_t visible = 0;
    for (const auto &entry : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
        if (entry.topic_name == topic.descriptor.topic &&
            entry.kind == dzIPC::info_pool::EntryKind::ShmPub)
            ++visible;
    EXPECT_EQ(visible, 2u);
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    local::Fd read_end(ready[0]), write_end(ready[1]);
    const auto fd_arg = std::to_string(ready[1]);
    const auto name = topic.segment();
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (!pid)
    {
        execl("/proc/self/exe", "test_shared_net_shm_bridge", "--shm-reader", name.c_str(),
              fd_arg.c_str(), nullptr);
        _exit(127);
    }
    ChildGuard guard(pid);
    write_end.reset();
    char signal = 0;
    ASSERT_TRUE(local::ready(read_end.get(), POLLIN, local::monotonic_ns() + 3000000000ull));
    ASSERT_EQ(read(read_end.get(), &signal, 1), 1);
    for (unsigned sequence = 1; sequence <= 12; ++sequence)
    {
        auto bytes = flat_blob(40, sequence);
        WireBlob blob;
        ASSERT_TRUE(WireEncoder::encode_prebuilt(ByteView(bytes), 71, 0, blob));
        if (sequence % 3 == 0)
            ASSERT_TRUE(ordinary->publish_prebuilt_segment(bytes.data(), bytes.size()));
        else if (sequence % 3 == 1)
            ASSERT_EQ(writer.try_commit(blob), SubmitState::Committed);
        else
            ASSERT_EQ(bridge->try_commit(blob), SubmitState::Committed);
        auto sample = rx.try_recv();
        ASSERT_GE(sample.size(), bytes.size());
        EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
        ASSERT_TRUE(local::ready(read_end.get(), POLLIN, local::monotonic_ns() + 3000000000ull));
        ASSERT_EQ(read(read_end.get(), &signal, 1), 1);
    }
    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    guard.pid = -1;
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    bridge.reset();
    EXPECT_EQ(registry.publisher_count(), 2u);
    auto bytes = flat_blob(40, 99);
    WireBlob blob;
    ASSERT_TRUE(WireEncoder::encode_prebuilt(ByteView(bytes), 71, 0, blob));
    EXPECT_EQ(writer.try_commit(blob), SubmitState::Committed);
    auto sample = rx.try_recv();
    ASSERT_GE(sample.size(), bytes.size());
    ordinary.reset();
    writer.close_after_quiescent();
    EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
}
TEST(SharedNetBridge, BadSchemaDoesNotSubmitAndNoReceiverIsNotRequired)
{
    BusinessTopic topic;
    topic.descriptor.schema_hash = 9;
    ShmWireBridge bridge(topic.descriptor);
    auto bytes = flat_blob(40);
    WireBlob blob;
    ASSERT_TRUE(WireEncoder::encode_prebuilt(ByteView(bytes), 71, 0, blob));
    EXPECT_EQ(bridge.try_commit(blob), SubmitState::NotSubmitted);
    auto valid = topic.descriptor;
    valid.schema_hash = 0;
    ShmWireWriter writer(valid);
    EXPECT_EQ(writer.try_commit(blob), SubmitState::NotRequired);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    EXPECT_EQ(bridge.try_commit(blob), SubmitState::NotSubmitted);
    EXPECT_TRUE(rx.try_recv().empty());
    writer.close_after_quiescent();
    EXPECT_EQ(writer.try_commit(blob), SubmitState::NotSubmitted);
}
int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--shm-reader")
    {
        ipc::mpmc_channel rx(argv[2], ipc::receiver, false);
        const int fd = std::stoi(argv[3]);
        const char ready = 'r';
        if (write(fd, &ready, 1) != 1)
            return 2;
        for (unsigned sequence = 1; sequence <= 12; ++sequence)
        {
            auto sample = rx.recv(3000);
            auto bytes = flat_blob(40, sequence);
            if (sample.size() < bytes.size() ||
                std::memcmp(sample.data(), bytes.data(), bytes.size()))
                return 3;
            if (write(fd, &ready, 1) != 1)
                return 4;
        }
        return rx.try_recv().empty() ? 0 : 5;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(SharedNetBridge, ExplicitLoanLengthRejectsWrongClassAndDoesNotPublishPadding)
{
    BusinessTopic topic;
    ShmWireWriter writer(topic.descriptor);
    ipc::mpmc_channel tx(topic.segment().c_str(), ipc::sender, false),
        rx(topic.segment().c_str(), ipc::receiver, false);
    auto loan = tx.loan(1536);
    ASSERT_TRUE(loan.valid());
    ASSERT_EQ(loan.size, 2048u);
    std::memset(loan.data, 0x51, loan.size);
    EXPECT_FALSE(tx.publish_loan_size(loan, 0));
    EXPECT_FALSE(tx.publish_loan_size(loan, 100));
    EXPECT_FALSE(tx.publish_loan_size(loan, 2049));
    EXPECT_TRUE(rx.try_recv().empty());
    ASSERT_TRUE(tx.publish_loan_size(loan, 1536));
    auto sample = rx.try_recv();
    ASSERT_EQ(sample.size(), 1536u);
    EXPECT_TRUE(std::all_of(static_cast<unsigned char *>(sample.data()),
                            static_cast<unsigned char *>(sample.data()) + sample.size(),
                            [](auto value) { return value == 0x51; }));
}
