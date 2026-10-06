#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/gateway_runtime.h"
#include "dzIPC/net/shared_config.h"
#include <csignal>
#include <algorithm>
#include <charconv>
#include <set>
#include "dzIPC/common/channel_scope.h"
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    using namespace dzIPC::net;
    if (argc < 2 || std::string(argv[1]) == "--help")
    {
        std::cout << "用法：dzipc_gateway check-config|serve --listen-ip IPv4 --interface 网卡 "
                     "--control 绝对路径\n"
                     "dzipc_gateway status --control 绝对路径 --json [--topic 话题 --domain 域 --msg-id ID --peer-id 实例ID] [--metrics counters|quota|latency|shards]\n"
                     "serve 可选：--data-shards S --data-workers W --data-socket-cap C "
                     "--data-port-range 起始:结束 --socket-fd-fraction F "
                     "--data-rcvbuf-bytes B --data-sndbuf-bytes B --socket-buffer-budget-bytes B "
                     "（v1 pooled；W 为本机 owner 数，资源不足时拒绝启动）\n";
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (command != "check-config" && command != "serve" && command != "status")
    {
        std::cerr << "InvalidOption: 未知命令\n";
        return 2;
    }
    GatewayConfig config;
    if (const auto *path = std::getenv("DZIPC_GATEWAY_CONTROL"))
        config.control_path = path;
    if (command == "status")
    {
        std::string topic, peer_text, metric_category; std::uint64_t domain = 0, message_id = 0;
        Identity peer{}; std::set<std::string> seen;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (!seen.insert(arg).second) { std::cerr << "InvalidOption: 重复参数\n"; return 2; }
            if (arg == "--json") continue;
            if (i + 1 >= argc) { std::cerr << "InvalidOption: 缺少参数值\n"; return 2; }
            const std::string value = argv[++i];
            if (arg == "--control") config.control_path = value;
            else if (arg == "--metrics") metric_category = value;
            else if (arg == "--topic") topic = value;
            else if (arg == "--peer-id") peer_text = value;
            else if (arg == "--domain" || arg == "--msg-id") {
                auto& number = arg == "--domain" ? domain : message_id;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (arg == "--msg-id" && number > UINT32_MAX)) {
                    std::cerr << "InvalidNumber: 无效域或消息 ID\n"; return 2;
                }
            } else { std::cerr << "InvalidOption: 无效状态参数\n"; return 2; }
        }
        if (topic.empty() && (seen.count("--topic") || seen.count("--peer-id") || seen.count("--domain") || seen.count("--msg-id"))) {
            std::cerr << "InvalidOption: 明细查询需要 --topic\n"; return 2;
        }
        if (!peer_text.empty()) {
            if (peer_text.size() != 32) { std::cerr << "InvalidOption: peer-id 需要 32 位十六进制\n"; return 2; }
            for (unsigned i = 0; i < 16; ++i) {
                unsigned value = 0; const auto parsed = std::from_chars(peer_text.data() + i * 2, peer_text.data() + i * 2 + 2, value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != peer_text.data() + i * 2 + 2) { std::cerr << "InvalidOption: peer-id 无效\n"; return 2; }
                peer[i] = value;
            }
            if (!nonzero(peer)) { std::cerr << "InvalidOption: peer-id 不可全零\n"; return 2; }
        }
        const std::vector<std::string> categories{"counters", "quota", "latency", "shards"};
        const auto category = std::find(categories.begin(), categories.end(), metric_category);
        if (seen.count("--metrics") && (category == categories.end() || !topic.empty())) { std::cerr << "InvalidOption: 指标类别无效或与话题明细冲突\n"; return 2; }
        const auto status = validate_control_path(config.control_path);
        if (!status)
        {
            std::cerr << status.detail << '\n';
            return 2;
        }
        try
        {
            auto runtime = ClientRuntime::acquire(config.control_path);
            if (!metric_category.empty()) std::cout << runtime->gateway_metrics(category - categories.begin()) << '\n';
            else if (topic.empty()) std::cout << runtime->status() << '\n';
            else {
                RouteKey key; key.scope = dzIPC::common::channel_scope_token(topic, domain, dzIPC::common::ScopeKind::PubSub); key.msg_id = message_id;
                std::cout << runtime->route_status(key, peer) << '\n';
            }
            return 0;
        }
        catch (const std::exception &e)
        {
            std::cerr << "GatewayUnavailable: " << e.what() << '\n';
            return 3;
        }
    }
    auto status = parse_gateway_options(std::vector<std::string>(argv + 2, argv + argc), config);
    if (status)
        status = validate_host_interface(config);
    if (!status)
    {
        std::cerr << config_code_name(status.code) << ": " << status.detail << '\n';
        return 2;
    }
    if (command == "serve")
    {
        try
        {
            sigset_t signals;
            sigemptyset(&signals);
            sigaddset(&signals, SIGINT);
            sigaddset(&signals, SIGTERM);
            if (pthread_sigmask(SIG_BLOCK, &signals, nullptr))
                throw std::runtime_error("阻塞退出信号失败");
            GatewayRuntime gateway(config);
            while (gateway.running())
            {
                timespec timeout{0, 100000000};
                const auto signal = sigtimedwait(&signals, nullptr, &timeout);
                if (signal == SIGINT || signal == SIGTERM)
                {
                    gateway.stop();
                    return 0;
                }
            }
            std::cerr << "GatewayStopped: 网关控制循环异常退出\n";
            return 3;
        }
        catch (const std::exception &e)
        {
            std::cerr << "GatewayUnavailable: " << e.what() << '\n';
            return 3;
        }
    }
    std::cout << "配置校验通过；数据分片=" << config.data_shards << "，数据 worker=" << config.data_workers
              << "，UDP 端点预算=" << config.data_shards + 2
              << "，最大消息=" << config.limits.message_bytes << " 字节（未绑定端口）\n";
    return 0;
}
