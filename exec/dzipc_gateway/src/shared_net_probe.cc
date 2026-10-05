#include "dzIPC/net/publisher_endpoint.h"
#include "dzIPC/common/channel_scope.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "libipc/ipc.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

using namespace dzIPC::net;
namespace {
Bytes payload(std::size_t size, std::uint64_t tag, std::uint64_t seed) {
    if (size < 48 || size > kMaxMessageBytes) throw std::invalid_argument("载荷范围为 48B～16MiB");
    Bytes out(size); dzflat::SegHeader h{dzflat::kMagic, 0xabcdef01, 32, 16, static_cast<std::uint32_t>(size), 1, 0, 71, 0};
    std::memcpy(out.data(), &h, sizeof(h)); std::memcpy(out.data() + 32, &tag, 8); std::memcpy(out.data() + 40, &seed, 8);
    for (std::size_t i = 48; i < size; ++i) out[i] = static_cast<unsigned char>((i * 31 + tag * 17 + seed) % 251);
    return out;
}
void put32(Bytes& b, std::uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(v >> (i * 8)); }
std::uint32_t read32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
}
int main(int argc, char** argv) try {
    if (argc != 4) { std::cerr << "用法：shared_net_probe 控制路径 话题 pub|sub|both\n"; return 2; }
    const std::string role = argv[3]; if (role != "pub" && role != "sub" && role != "both") return 2;
    RouteDescriptor descriptor; descriptor.topic = argv[2]; descriptor.key.msg_id = 71; descriptor.schema_hash = 0xabcdef01;
    descriptor.key.scope = dzIPC::common::channel_scope_token(descriptor.topic, 0, dzIPC::common::ScopeKind::PubSub);
    auto runtime = ClientRuntime::acquire(argv[1]);
    std::unique_ptr<PublisherEndpoint> publisher;
    if (role != "sub") publisher = std::make_unique<PublisherEndpoint>(runtime, descriptor);
    Identity sub{}; std::unique_ptr<ipc::mpmc_channel> receiver;
    if (role != "pub") {
        // probe 句柄只在本进程/会话使用；业务公共订阅封装在 T11 接入。
        const auto value = std::chrono::steady_clock::now().time_since_epoch().count(); std::memcpy(sub.data(), &value, 8); sub[15] = 2;
        auto registered = runtime->request(LocalKind::RegisterSub, registration_body(sub, descriptor));
        if (registered.header.kind != LocalKind::SubRegistered) throw std::runtime_error("probe 订阅登记失败");
        const auto generation = read32(registered.body.data() + 16);
        receiver = std::make_unique<ipc::mpmc_channel>(shm_topic_mpmc_segment_name(descriptor.topic, 0).c_str(), ipc::receiver, false);
        dzIPC::control_plane_shm::TopicControlPlane control;
        if (!control.open(shm_topic_mpmc_control_name(descriptor.topic, 0)) || control.generation() != generation) throw std::runtime_error("probe SHM generation 不匹配");
        Bytes body(sub.begin(), sub.end()); put32(body, generation);
        if (runtime->request(LocalKind::SubReady, body).header.kind != LocalKind::SubReadyAck) throw std::runtime_error("probe Ready 失败");
    }
    std::cout << "{\"ready\":true}" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line); std::string command; input >> command;
        if (command == "quit") break;
        if (command == "state") {
            const auto hint = publisher ? runtime->route_state(publisher->publisher_id()) : RouteStateBody{};
            std::cout << "{\"remote\":" << hint.remote_ready_count << ",\"synchronized\":" << (hint.synchronized ? "true" : "false") << ",\"healthy\":" << (runtime->healthy() ? "true" : "false") << "}" << std::endl;
        } else if (command == "status") std::cout << runtime->status() << std::endl;
        else if (command == "send" && publisher) {
            std::size_t size; std::uint64_t tag, seed; unsigned reliable = 0; input >> size >> tag >> seed >> reliable;
            auto bytes = payload(size, tag, seed); const auto result = publisher->prebuilt(ByteView(bytes), reliable ? Delivery::Reliable : Delivery::BestEffort, reliable ? 5000 : 0);
            std::cout << "{\"success\":" << (result.success ? "true" : "false") << ",\"local\":" << static_cast<int>(result.local)
                      << ",\"network\":" << static_cast<int>(result.network) << ",\"sequence\":" << result.sequence
                      << ",\"result\":" << static_cast<unsigned>(result.remote.result) << ",\"crc\":" << crc32c(ByteView(bytes)) << "}" << std::endl;
        } else if (command == "recv" && receiver) {
            unsigned count, timeout; input >> count >> timeout;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            std::cout << "{\"received\":["; unsigned received = 0;
            while (received < count && std::chrono::steady_clock::now() < deadline) {
                auto sample = receiver->try_recv(); if (sample.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(100)); continue; }
                std::uint64_t tag = 0, seed = 0; bool valid = sample.size() >= 48 && sample.size() <= kMaxMessageBytes;
                if (valid) { std::memcpy(&tag, static_cast<const std::uint8_t*>(sample.data()) + 32, 8); std::memcpy(&seed, static_cast<const std::uint8_t*>(sample.data()) + 40, 8);
                    const auto expected = payload(sample.size(), tag, seed); valid = !std::memcmp(expected.data(), sample.data(), sample.size()); }
                if (received++) std::cout << ',';
                std::cout << "{\"tag\":" << tag << ",\"seed\":" << seed << ",\"size\":" << sample.size() << ",\"crc\":" << crc32c({sample.data(), sample.size()}) << ",\"valid\":" << (valid ? "true" : "false") << '}';
            }
            std::cout << "]}" << std::endl;
        } else throw std::invalid_argument("probe 命令无效");
    }
    if (receiver) { Bytes body(sub.begin(), sub.end()); body.push_back(2); if (runtime->healthy()) runtime->request(LocalKind::Unregister, body); receiver.reset(); }
    return 0;
} catch (const std::exception& e) { std::cerr << "probe 失败：" << e.what() << '\n'; return 1; }
