#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "dzIPC/common/loaned_message.h"
#include "dzIPC/shm_route_session.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"

namespace {

std::string unique_name(const char* suffix)
{
    static std::atomic<unsigned> sequence{0};
    return std::string("test_shm_mpmc_types_") + suffix + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

TEST(ShmMpmcTypes, RouteSessionUsesMpmcChannel)
{
    const auto name = unique_name("session");
    ipc::mpmc_channel::clear_storage(name.c_str());

    {
        dzIPC::shm::MpmcRouteSession session;
        session.begin_rebuild(1, [&] {
            return std::make_shared<ipc::mpmc_channel>(name.c_str(), ipc::receiver, false);
        });
        ASSERT_EQ(session.generation(), 1u);
        ASSERT_TRUE(session.current_route());

        ipc::mpmc_channel sender{name.c_str(), ipc::sender, false};
        ASSERT_TRUE(sender.valid());
        ASSERT_TRUE(sender.wait_for_recv(1, 1000));

        const std::array<std::uint32_t, 2> payload{17, 23};
        ASSERT_TRUE(sender.try_send(payload.data(), sizeof(payload), 1000));

        auto lease = session.acquire_receive();
        ASSERT_TRUE(lease.has_value());
        auto received = lease->route->recv(1000);
        ASSERT_EQ(received.size(), sizeof(payload));
        std::array<std::uint32_t, 2> decoded{};
        std::memcpy(decoded.data(), received.data(), sizeof(decoded));
        EXPECT_EQ(decoded, payload);
        session.release_receive();

        session.stop_and_wake();
        session.wait_quiescent();
    }

    ipc::mpmc_channel::clear_storage(name.c_str());
}

TEST(ShmMpmcTypes, LoanedMessageUsesMpmcChannel)
{
    using Flat = dzIPC::Msg::StdImageFlat;
    const auto name = unique_name("loan");
    ipc::mpmc_channel::clear_storage(name.c_str());

    {
        auto sender = std::make_shared<ipc::mpmc_channel>(name.c_str(), ipc::sender, false);
        ipc::mpmc_channel receiver{name.c_str(), ipc::receiver, false};
        ASSERT_TRUE(sender->valid());
        ASSERT_TRUE(receiver.valid());
        ASSERT_TRUE(sender->wait_for_recv(1, 1000));

        auto raw_loan = sender->loan(Flat::loan_size(128));
        ASSERT_TRUE(raw_loan.valid());
        dzIPC::MpmcLoanedMessage<Flat> message{sender, raw_loan, 71};
        ASSERT_TRUE(message.valid());
        message->set_width(2);
        message->set_height(2);
        message->set_step(6);
        ASSERT_TRUE(message->set_encoding("rgb8"));
        auto bytes = message->alloc_data(12);
        ASSERT_EQ(bytes.size(), 12u);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(i);
        ASSERT_TRUE(message.finalize());
        const auto published_loan = message.loan();
        message.release();
        ASSERT_TRUE(sender->publish_loan(published_loan, 1000));

        auto received = receiver.recv(1000);
        ASSERT_FALSE(received.empty());
        const auto view = dzIPC::Msg::StdImageView::bind(received.data(), received.size());
        ASSERT_TRUE(view.valid());
        EXPECT_EQ(view.width(), 2u);
        EXPECT_EQ(view.height(), 2u);
        ASSERT_EQ(view.data().size(), 12u);
        EXPECT_EQ(view.data()[0], static_cast<std::uint8_t>(0));
        EXPECT_EQ(view.data()[11], static_cast<std::uint8_t>(11));
    }

    ipc::mpmc_channel::clear_storage(name.c_str());
}

} // namespace
