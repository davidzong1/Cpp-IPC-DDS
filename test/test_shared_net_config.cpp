#include "dzIPC/net/shared_config.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <limits>
#if defined(__linux__)
#include <unistd.h>
#endif

using namespace dzIPC::net;
namespace
{
GatewayConfig valid_config()
{
    GatewayConfig c;
    c.listen_ip = "127.0.0.1";
    c.interface = "lo";
    c.control_path = "/tmp/dzipc-test/control.sock";
    return c;
}
} // namespace
TEST(SharedNetConfig, ExactBackendAndAvailability)
{
    Backend out = Backend::SharedV1;
    EXPECT_TRUE(parse_backend(nullptr, out));
    EXPECT_EQ(out, Backend::Legacy);
    EXPECT_TRUE(parse_backend("legacy", out));
    EXPECT_TRUE(parse_backend("shared_v1", out));
    EXPECT_EQ(out, Backend::SharedV1);
    for (const auto *bad : {"", "Legacy", "shared", " shared_v1", "shared_v1 ", "1"})
    {
        EXPECT_EQ(parse_backend(bad, out).code, ConfigCode::InvalidBackend);
        EXPECT_EQ(out, Backend::SharedV1);
    }
    EXPECT_TRUE(backend_availability(Backend::Legacy, false, false, false));
    EXPECT_EQ(backend_availability(out, false, true, true).code, ConfigCode::UnsupportedPlatform);
    EXPECT_EQ(backend_availability(out, true, false, true).code, ConfigCode::BackendNotBuilt);
    EXPECT_EQ(backend_availability(out, true, true, false).code, ConfigCode::MpmcRequired);
    EXPECT_TRUE(backend_availability(out, true, true, true));
    EXPECT_EQ(shared_net_built(), DZIPC_TEST_SHARED_NET_BUILT != 0);
}
TEST(SharedNetConfig, DefaultsAndPortBoundaries)
{
    auto c = valid_config();
    ASSERT_TRUE(validate_config(c));
    for (auto bad : std::vector<std::uint64_t>{0, 17, UINT64_MAX})
    {
        c.data_shards = bad;
        EXPECT_FALSE(validate_config(c));
    }
    c = valid_config();
    c.data_base_port = 65535;
    EXPECT_FALSE(validate_config(c));
    c.data_shards = 1;
    c.data_port_range = "65535:65535";
    EXPECT_TRUE(validate_config(c));
    c.control_port = 65535;
    EXPECT_EQ(validate_config(c).code, ConfigCode::PortConflict);
    c = valid_config();
    c.discovery_port = c.control_port;
    EXPECT_EQ(validate_config(c).code, ConfigCode::PortConflict);
    c = valid_config();
    c.data_shards = 16;
    c.control_port = 24016;
    c.discovery_port = 24017;
    EXPECT_TRUE(validate_config(c));
    c.data_workers = 1;
    EXPECT_TRUE(validate_config(c));
    c.data_workers = 65;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.data_socket_cap = 0;
    EXPECT_EQ(validate_config(c).code, ConfigCode::SocketCap);
    c = valid_config();
    c.data_socket_cap = 3;
    EXPECT_EQ(validate_config(c).code, ConfigCode::SocketCap);
    c = valid_config();
    c.data_port_range = "24000:24002";
    EXPECT_EQ(validate_config(c).code, ConfigCode::PortBudget);
    c = valid_config();
    c.data_port_range = "30000:49999";
    EXPECT_TRUE(validate_config(c));
    c = valid_config();
    c.socket_fd_fraction = 0.500001;
    EXPECT_EQ(validate_config(c).code, ConfigCode::FdBudget);
    c.socket_fd_fraction = 0.0;
    EXPECT_EQ(validate_config(c).code, ConfigCode::FdBudget);
    c = valid_config();
    c.socket_buffer_budget_bytes = 0;
    EXPECT_EQ(validate_config(c).code, ConfigCode::BufferBudget);
}
TEST(SharedNetConfig, StrictAddressesAndPaths)
{
    for (const auto *address : {"0.0.0.0", "::1", "239.1.2.3", "255.255.255.255", "127.1",
                                "127.00.0.1", "127.0.0.256", "127.0.0.1."})
    {
        auto c = valid_config();
        c.listen_ip = address;
        EXPECT_EQ(validate_config(c).code, ConfigCode::InvalidAddress) << address;
    }
    auto c = valid_config();
    c.discovery_group = "127.0.0.1";
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.interface = std::string(16, 'x');
    EXPECT_FALSE(validate_config(c));
    for (const auto *path :
         {"", "relative.sock", "/tmp/", "/tmp/../a", "/tmp/./a", "/tmp//a", "/tmp/.."})
        EXPECT_EQ(validate_control_path(path).code, ConfigCode::InvalidControlPath);
    EXPECT_TRUE(validate_control_path("/" + std::string(106, 'x')));
    EXPECT_FALSE(validate_control_path("/" + std::string(107, 'x')));
    EXPECT_FALSE(validate_control_path(std::string("/tmp/a\0x", 8)));
}
TEST(SharedNetConfig, AtomicParsingAndOverflow)
{
    auto c = valid_config();
    for (const auto &args :
         std::vector<std::vector<std::string>>{{"--data-shards"},
                                               {"--data-shards", "2", "--data-shards", "3"},
                                               {"--data-shard", "2"},
                                               {"--data-shards", "-1"},
                                               {"--data-workers", "0"},
                                               {"--data-workers", "65"},
                                               {"--data-shards", "+1"},
                                               {"--data-shards", "18446744073709551616"},
                                               {"--send-bytes", "18446744073709551615"}})
    {
        EXPECT_FALSE(parse_gateway_options(args, c));
        EXPECT_EQ(c.data_shards, 4u);
    }
    EXPECT_TRUE(parse_gateway_options(
        {"--data-shards", "2", "--data-workers", "1", "--io-batch-max", "64", "--nack-delay-ms", "1"}, c));
    EXPECT_EQ(c.data_shards, 2u);
    EXPECT_EQ(c.data_workers, 1u);
    EXPECT_EQ(c.io_batch_max, 64u);
    EXPECT_TRUE(parse_gateway_options({"--network-version", "2", "--data-port-range", "22000:25000",
                                       "--socket-fd-fraction", "0.25",
                                       "--data-socket-cap", "8",
                                       "--data-rcvbuf-bytes", "131072",
                                       "--data-sndbuf-bytes", "131072",
                                       "--socket-buffer-budget-bytes", "1048576"}, c));
    EXPECT_EQ(c.data_port_range, "22000:25000");
    EXPECT_DOUBLE_EQ(c.socket_fd_fraction, 0.25);
    EXPECT_EQ(c.data_socket_cap, 8u);
    EXPECT_EQ(c.data_rcvbuf_bytes, 131072u);
    EXPECT_EQ(c.data_sndbuf_bytes, 131072u);
    EXPECT_EQ(c.socket_buffer_budget_bytes, 1048576u);
}
TEST(SharedNetConfig, PerTopicInternalPoolAndPolicyModes)
{
    auto c = valid_config();
    ASSERT_TRUE(parse_gateway_options({"--network-version", "2", "--data-mode", "per-topic",
                                       "--data-port-range", "22000:25000", "--data-workers", "1"}, c));
    c.data_shards = 0;
    EXPECT_TRUE(validate_config(c));
    c.data_shards = 17;
    EXPECT_EQ(validate_config(c).code, ConfigCode::InvalidNumber);
    c = valid_config();
    c.network_version = NetworkVersion::V2;
    c.data_mode = DataMode::PerTopic;
    c.data_shards_explicit = true;
    EXPECT_EQ(validate_config(c).code, ConfigCode::InvalidOption);
}
TEST(SharedNetConfig, CapacityAndMetadataLimits)
{
    auto c = valid_config();
    c.limits.outbox_bytes = 16 * kMiB + 112;
    EXPECT_EQ(validate_config(c).code, ConfigCode::InvalidLimit); // 实际 loan 为 32 MiB
    c = valid_config();
    c.limits.session_send_bytes = 16 * kMiB - 1;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.limits.peer_history = 255;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.limits.command_bytes = 4096;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.limits.old_directory_bytes = UINT64_MAX;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.limits.target_states = 0;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.limits.initial_send_bytes = 0;
    c.limits.initial_send_records = 0;
    EXPECT_TRUE(validate_config(c));
}
TEST(SharedNetConfig, TimerAndControlBounds)
{
    auto c = valid_config();
    c.retry_initial_ms = 20;
    c.retry_max_ms = 19;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.io_batch_max = 65;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.nack_interval_ms = 0;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.peer_control_rate = c.control_rate + 1;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.control_rate = UINT64_MAX;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.io_round_packets = 0;
    EXPECT_FALSE(validate_config(c));
    c = valid_config();
    c.io_round_us = 1000001;
    EXPECT_FALSE(validate_config(c));
}
#if defined(__linux__)
TEST(SharedNetConfig, ResourceAuditIsReadOnlyAndSeparatesBudgets)
{
    auto c = valid_config();
    const auto audit = endpoint_resource_audit(c, c.data_shards + 2);
    EXPECT_GT(audit.fd_soft_limit, 0u);
    EXPECT_GE(audit.fd_budget_limit, audit.fd_count);
    EXPECT_EQ(audit.candidate_ports, 29998u);
    EXPECT_EQ(audit.reserved_ports, 6u);
    EXPECT_EQ(audit.buffer_budget_bytes, c.socket_buffer_budget_bytes);
}
TEST(SharedNetConfig, RealInterfaceCheckDoesNotBind)
{
    auto c = valid_config();
    EXPECT_TRUE(validate_host_interface(c));
    c.interface = "dz_no_such_if";
    EXPECT_EQ(validate_host_interface(c).code, ConfigCode::InvalidInterface);
    c = valid_config();
    c.listen_ip = "192.0.2.1";
    EXPECT_FALSE(validate_host_interface(c));
}
TEST(SharedNetConfig, EnvironmentSampledOnceInIsolatedChild)
{
    EXPECT_EXIT(
        {
            setenv("DZIPC_NET_BACKEND", "legacy", 1);
            const auto &first = process_config();
            setenv("DZIPC_NET_BACKEND", "typo", 1);
            const auto &second = process_config();
            _exit(first.status && second.status && second.backend == Backend::Legacy ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}
TEST(SharedNetConfig, SharedNeverSilentlyFallsBack)
{
    EXPECT_EXIT(
        {
            setenv("DZIPC_NET_BACKEND", "shared_v1", 1);
            setenv("DZIPC_SHM_MPMC", "1", 1);
            setenv("DZIPC_GATEWAY_CONTROL", "/tmp/dzipc-test/control.sock", 1);
            try
            {
                require_network_backend(false);
                _exit(shared_net_built() ? 0 : 3);
            }
            catch (const ConfigError &e)
            {
                const auto expected = ConfigCode::BackendNotBuilt;
                _exit(e.code() == expected ? 0 : 2);
            }
            _exit(1);
        },
        ::testing::ExitedWithCode(0), "");
}
TEST(SharedNetConfig, SocketOnlyIgnoresSharedButNotTypos)
{
    EXPECT_EXIT(
        {
            setenv("DZIPC_NET_BACKEND", "shared_v1", 1);
            try
            {
                require_network_backend(true);
                _exit(0);
            }
            catch (...)
            {
                _exit(1);
            }
        },
        ::testing::ExitedWithCode(0), "");
    EXPECT_EXIT(
        {
            setenv("DZIPC_NET_BACKEND", "typo", 1);
            try
            {
                require_network_backend(true);
            }
            catch (const ConfigError &e)
            {
                _exit(e.code() == ConfigCode::InvalidBackend ? 0 : 2);
            }
            _exit(1);
        },
        ::testing::ExitedWithCode(0), "");
}
#endif
