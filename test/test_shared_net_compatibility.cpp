#include "shared_net/public_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetCompatibility, MissingGatewayFailsFactoryWithoutLegacyFallbackOrFdLeak) {
    PublicFixture f; f.gateway->stop(); const auto before = fd_count();
    EXPECT_THROW((dzIPC::pimpl::publisher_ipc_impl(f.model(), f.topic.descriptor.topic, 0, dzIPC::IPCType::Socket)), std::exception);
    EXPECT_THROW((dzIPC::pimpl::subscriber_ipc_impl(f.model(), f.topic.descriptor.topic, 0, 8, dzIPC::IPCType::Socket)), std::exception);
    EXPECT_EQ(fd_count(), before);
}
TEST(SharedNetCompatibility, ExplicitShmAndSocketOnlyDoNotNeedGateway) {
    PublicFixture f; f.gateway->stop();
    EXPECT_NO_THROW((dzIPC::pimpl::publisher_ipc_impl(f.model(), f.topic.descriptor.topic, 0, dzIPC::IPCType::Shm)));
    EXPECT_NO_THROW((dzIPC::pimpl::subscriber_ipc_impl(f.model(), f.topic.descriptor.topic, 0, 8, dzIPC::IPCType::Shm)));
    EXPECT_NO_THROW((dzIPC::pimpl::publisher_ipc_impl(f.model(), f.topic.descriptor.topic, 0, dzIPC::IPCType::SocketOnly)));
    EXPECT_NO_THROW((dzIPC::pimpl::subscriber_ipc_impl(f.model(), f.topic.descriptor.topic, 0, 8, dzIPC::IPCType::SocketOnly)));
}
