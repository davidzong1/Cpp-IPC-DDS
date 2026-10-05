#include "dzIPC/net/shared_config.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    using namespace dzIPC::net;
    if (argc < 2 || std::string(argv[1]) == "--help")
    {
        std::cout << "用法：dzipc_gateway check-config|serve --listen-ip IPv4 --interface 网卡 "
                     "--control 绝对路径\n"
                     "可设置数据分片、端口、配额、批量与重传参数；serve/status 尚未实现。\n";
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (command != "check-config" && command != "serve" && command != "status")
    {
        std::cerr << "InvalidOption: 未知命令\n";
        return 2;
    }
    if (command == "status")
    {
        std::cerr << "NotImplemented: 网关状态接口尚未交付\n";
        return 3;
    }
    GatewayConfig config;
    if (const auto *path = std::getenv("DZIPC_GATEWAY_CONTROL"))
        config.control_path = path;
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
        std::cerr << "NotImplemented: 网关数据面尚未交付\n";
        return 3;
    }
    std::cout << "配置校验通过；数据分片=" << config.data_shards
              << "，UDP 端点预算=" << config.data_shards + 2
              << "，最大消息=" << config.limits.message_bytes << " 字节（未绑定端口）\n";
    return 0;
}
