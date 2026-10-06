#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "dzIPC/net/local_protocol.h"
#include "dzIPC/common/channel_scope.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace dzIPC::net;

namespace {
RouteDescriptor route(const std::string &prefix, std::size_t i) {
    RouteDescriptor d;
    d.topic = prefix + std::to_string(i);
    d.key.msg_id = 71;
    d.schema_hash = 0xabcdef01;
    d.key.scope = dzIPC::common::channel_scope_token(d.topic, 0, dzIPC::common::ScopeKind::PubSub);
    return d;
}
}

int main(int argc, char **argv) try {
    if (argc < 4 || argc > 5) {
        std::cerr << "用法：shared_net_scale_probe 控制路径 话题数 释放后等待毫秒 [话题前缀]\n";
        return 2;
    }
    const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
    const auto wait_ms = std::chrono::milliseconds(std::stoul(argv[3]));
    const std::string prefix = argc == 5 ? argv[4] : "scale_dedicated_";
    auto runtime = ClientRuntime::acquire(argv[1]);
    std::vector<std::unique_ptr<PublisherEndpoint>> publishers;
    publishers.reserve(count);
    std::size_t failed = 0;
    const auto requested = count;
    std::string first_error;
    for (std::size_t i = 0; i < count; ++i) {
        try {
            publishers.push_back(std::make_unique<PublisherEndpoint>(runtime, route(prefix, i)));
        } catch (const std::exception &e) {
            ++failed;
            if (first_error.empty()) first_error = e.what();
            break;
        }
    }
    std::cout << "{\"registered\":" << publishers.size()
              << ",\"failed\":" << failed
              << ",\"first_error\":\"";
    for (const char c : first_error) {
        if (c == '\\' || c == '"') std::cout << '\\';
        if (c == '\n' || c == '\r') continue;
        std::cout << c;
    }
    std::cout << "\"}\n" << std::flush;
    std::string command;
    if (!std::getline(std::cin, command) || command != "release") {
        std::cerr << "未收到 release 命令\n";
        return 2;
    }
    const auto registered = publishers.size();
    publishers.clear();
    std::this_thread::sleep_for(wait_ms);
    std::cout << "{\"phase\":\"released\",\"status\":" << runtime->status() << "}\n" << std::flush;
    if (!std::getline(std::cin, command) || command != "register-again") {
        std::cerr << "未收到 register-again 命令\n";
        return 2;
    }
    auto recovery = std::make_unique<PublisherEndpoint>(runtime, route(prefix, requested + 1));
    std::cout << "{\"phase\":\"reregistered\",\"status\":" << runtime->status() << "}\n" << std::flush;
    if (!std::getline(std::cin, command) || command != "close-again") {
        std::cerr << "未收到 close-again 命令\n";
        return 2;
    }
    recovery->close();
    recovery.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "{\"phase\":\"closed-again\",\"status\":" << runtime->status() << "}\n" << std::flush;
    return (failed && registered == requested) ? 1 : 0;
} catch (const std::exception &e) {
    std::cerr << "probe 失败：" << e.what() << '\n';
    return 1;
}
