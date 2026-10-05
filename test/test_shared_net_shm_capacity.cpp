#include "gtest/gtest.h"
#if defined(__linux__)
#include "libipc/ipc.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace
{
struct IsolatedChannel
{
    std::string name;
    IsolatedChannel()
    {
        static std::atomic<unsigned> serial{0};
        name = "shared_net_capacity_" + std::to_string(getpid()) + "_" + std::to_string(serial++);
    }
    ~IsolatedChannel()
    {
        ipc::mpmc_channel::clear_storage(ipc::prefix{name.c_str()}, name.c_str());
    }
    std::unique_ptr<ipc::mpmc_channel> open(unsigned mode)
    {
        return std::make_unique<ipc::mpmc_channel>(ipc::prefix{name.c_str()}, name.c_str(), mode,
                                                   false);
    }
};
std::uint64_t mapped_bytes(const void *ptr)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
    {
        unsigned long long first, last;
        if (std::sscanf(line.c_str(), "%llx-%llx", &first, &last) == 2 &&
            reinterpret_cast<std::uintptr_t>(ptr) >= first &&
            reinterpret_cast<std::uintptr_t>(ptr) < last)
            return last - first;
    }
    return 0;
}
} // namespace
TEST(SharedNetShmCapacity, EnvelopeCrossesActualLoanClass)
{
    IsolatedChannel owner;
    auto tx = owner.open(ipc::sender);
    auto rx = owner.open(ipc::receiver);
    ASSERT_TRUE(tx->wait_for_recv(1, 1000));
    constexpr std::size_t message = 16 * 1024 * 1024;
    auto business = tx->loan(message);
    ASSERT_TRUE(business.valid());
    EXPECT_EQ(business.size, message);
    tx->discard_loan(business);
    auto outbox = tx->loan(message + 112);
    ASSERT_TRUE(outbox.valid());
    EXPECT_EQ(outbox.size, 32u * 1024 * 1024);
    const auto mapping = mapped_bytes(outbox.data);
    EXPECT_GT(mapping, outbox.size);
    std::cout << "最大载荷=" << message << "，出站请求=" << message + 112
              << "，实际 loan=" << outbox.size << "，池映射=" << mapping << '\n';
    auto *bytes = static_cast<unsigned char *>(outbox.data);
    bytes[0] = 0x31;
    bytes[message + 111] = 0x7f;
    ASSERT_TRUE(tx->publish_loan(outbox, 0));
    auto sample = rx->recv(1000);
    ASSERT_EQ(sample.size(), outbox.size);
    EXPECT_EQ(static_cast<const unsigned char *>(sample.data())[message + 111], 0x7f);
}
TEST(SharedNetShmCapacity, LoanAndSampleRetainMappingAfterOwnerClose)
{
    IsolatedChannel owner;
    auto tx = owner.open(ipc::sender);
    auto survivor = owner.open(ipc::sender);
    auto rx = owner.open(ipc::receiver);
    ASSERT_TRUE(tx->wait_for_recv(1, 1000));
    auto pending = tx->loan(112 + 1024);
    ASSERT_TRUE(pending.valid());
    static_cast<unsigned char *>(pending.data)[0] = 91;
    tx.reset();
    EXPECT_EQ(static_cast<unsigned char *>(pending.data)[0], 91);
    pending = {}; // 未提交 loan 的 RAII 归还仍可使用原池
    auto loan = survivor->loan(2048);
    ASSERT_TRUE(loan.valid());
    static_cast<unsigned char *>(loan.data)[0] = 42;
    ASSERT_TRUE(survivor->publish_loan(loan, 0));
    auto sample = rx->recv(1000);
    ASSERT_FALSE(sample.empty());
    survivor.reset();
    rx.reset();
    EXPECT_EQ(static_cast<const unsigned char *>(sample.data())[0], 42);
}
TEST(SharedNetShmCapacity, NoReceiverIsDefinitelyNotSubmitted)
{
    IsolatedChannel owner;
    auto tx = owner.open(ipc::sender);
    ipc::loan_status status{};
    auto loan = tx->loan(112 + 1024, status);
    EXPECT_FALSE(loan.valid());
    EXPECT_EQ(status, ipc::loan_status::no_receiver);
}
#endif
