#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/gateway_runtime.h"
#include "dzIPC/net/shared_config.h"
#include <csignal>
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
                     "dzipc_gateway status --control 绝对路径 --json\n";
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
        bool control = false, json = false;
        for (int i = 2; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--json" && !json)
                json = true;
            else if (arg == "--control" && !control && i + 1 < argc)
            {
                config.control_path = argv[++i];
                control = true;
            }
            else
            {
                std::cerr << "InvalidOption: 无效或重复的状态参数\n";
                return 2;
            }
        }
        const auto status = validate_control_path(config.control_path);
        if (!status)
        {
            std::cerr << status.detail << '\n';
            return 2;
        }
        try
        {
            std::cout << ClientRuntime::acquire(config.control_path)->status() << '\n';
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
    std::cout << "配置校验通过；数据分片=" << config.data_shards
              << "，UDP 端点预算=" << config.data_shards + 2
              << "，最大消息=" << config.limits.message_bytes << " 字节（未绑定端口）\n";
    return 0;
}
