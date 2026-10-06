#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dzIPC::net
{

enum class Backend
{
    Legacy,
    SharedV1
};
enum class ConfigCode
{
    Ok,
    InvalidBackend,
    UnsupportedPlatform,
    BackendNotBuilt,
    MpmcRequired,
    InvalidOption,
    InvalidNumber,
    InvalidAddress,
    InvalidInterface,
    InvalidControlPath,
    PortConflict,
    InvalidLimit,
    NotImplemented
};
const char *config_code_name(ConfigCode code) noexcept;
struct ConfigStatus
{
    ConfigCode code = ConfigCode::Ok;
    std::string detail;
    explicit operator bool() const noexcept
    {
        return code == ConfigCode::Ok;
    }
};
class ConfigError : public std::runtime_error
{
  public:
    explicit ConfigError(ConfigStatus status);
    ConfigCode code() const noexcept
    {
        return code_;
    }

  private:
    ConfigCode code_;
};

// 不读取环境的解析入口；运行时在首次网络对象构造时统一采样。
ConfigStatus parse_backend(const char *value, Backend &out);
ConfigStatus backend_availability(Backend backend, bool supported, bool built, bool mpmc);
bool shared_net_supported() noexcept;
bool shared_net_built() noexcept;
struct ProcessConfig
{
    Backend backend = Backend::Legacy;
    bool receive_assist = true;
    std::string control_path;
    ConfigStatus status;
};
const ProcessConfig &process_config();
void require_network_backend(bool socket_only);

inline constexpr std::uint64_t kMiB = 1024 * 1024;
struct Limits
{
    std::uint64_t topics = 4096, sessions = 128, peers = 128;
    std::uint64_t handles = 16384, session_handles = 8192;
    std::uint64_t message_bytes = 16 * kMiB;
    std::uint64_t reassembly_bytes = 256 * kMiB, peer_reassembly_bytes = 64 * kMiB;
    std::uint64_t route_reassembly_bytes = 32 * kMiB, assemblies = 4096;
    std::uint64_t send_bytes = 256 * kMiB, send_records = 4096;
    std::uint64_t publisher_reliable = 64, target_states = 32768;
    std::uint64_t commit_pending_bytes = 64 * kMiB;
    std::uint64_t streams = 16384, stream_window = 4096;
    std::uint64_t receipts = 65536, peer_receipts = 8192;
    std::uint64_t outbox_bytes = 32 * kMiB, outbox_records = 256;
    std::uint64_t session_send_bytes = 32 * kMiB, session_send_records = 512;
    std::uint64_t initial_send_bytes = kMiB, initial_send_records = 16;
    std::uint64_t result_history = 256, pending_credit_requests = 1, init_tasks = 64;
    std::uint64_t candidate_bytes = 64 * kMiB, peer_candidate_bytes = 8 * kMiB;
    std::uint64_t directory_bytes = 64 * kMiB, old_directory_bytes = 64 * kMiB;
    std::uint64_t peer_history = 16384, gateway_history = 256;
    std::uint64_t session_control_bytes = 256 * 1024, local_control_bytes = 8 * kMiB;
    std::uint64_t network_control_bytes = 8 * kMiB;
    // 跨线程队列仅存命令/引用，payload 另记原账本；两者都有限额。
    std::uint64_t command_records = 4096, command_bytes = 8 * kMiB;
};
struct GatewayConfig
{
    std::string listen_ip, interface, control_path;
    std::string discovery_group = "239.255.250.251";
    std::uint64_t data_base_port = 24000, data_shards = 4, data_workers = 4;
    std::uint64_t control_port = 24004, discovery_port = 24005;
    std::uint64_t io_batch_max = 32, nack_delay_ms = 2, nack_interval_ms = 2;
    std::uint64_t io_round_packets = 64, io_round_bytes = 64 * 1024, io_round_us = 200;
    std::uint64_t retry_initial_ms = 2, retry_max_ms = 100;
    std::uint64_t control_rate = 10000, peer_control_rate = 1000, control_burst = 64;
    Limits limits;
};
ConfigStatus validate_control_path(const std::string &path);
ConfigStatus validate_config(const GatewayConfig &config);
// 格式 --key value；拒绝重复/未知选项；失败不修改 out。
ConfigStatus parse_gateway_options(const std::vector<std::string> &args, GatewayConfig &out);
// Linux 实机检查网卡与本机 IPv4 归属，不 bind、不改变网络状态。
ConfigStatus validate_host_interface(const GatewayConfig &config);

} // namespace dzIPC::net
