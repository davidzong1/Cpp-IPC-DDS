// 只用安装目录的 include/lib 编译；供安装与回滚演练使用。
#include "dzIPC/dzipc.h"
#include "dzIPC/net/shared_config.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
int main() try {
    // legacy blocking 固定使用 TLV；两模式均以对象接口读取相同消息。
    dzIPC::EnableDzFlat(false); dzIPC::EnableNodelet(false);
    const auto topic = "installed_roundtrip_" + std::to_string(getpid());
    auto model = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 71);
    auto sub = dzIPC::SubscriberIPCPtrMake(model, topic, 0, 8, dzIPC::IPC_SOCKET); sub->InitChannel();
    auto pub = dzIPC::PublisherIPCPtrMake(model, topic, 0, dzIPC::IPC_SOCKET); pub->InitChannel();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!pub->has_subscribed() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto message = std::make_shared<dzIPC::Msg::StdImage>(); message->set_msg_id(71); message->width = 64; message->height = 1; message->data.assign(64, 0x7b);
    if (!pub->publish_blocking(message, 1000)) { std::cerr << "blocking 返回失败，订阅提示=" << pub->has_subscribed() << '\n'; return 2; }
    auto received = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 71);
    const auto receive_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool got = false;
    while (!(got = sub->try_get_clone(received)) && std::chrono::steady_clock::now() < receive_end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!got) { std::cerr << "未读取到 TLV 对象\n"; return 3; }
    if (received->topic()->msgcast<dzIPC::Msg::StdImage>()->data != message->data) return 4;
    const bool shared = dzIPC::net::process_config().backend == dzIPC::net::Backend::SharedV1;
    std::cout << "{\"roundtrip\":true,\"backend\":\"" << (shared ? "shared_v1" : "legacy") << "\"}" << std::endl;
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
