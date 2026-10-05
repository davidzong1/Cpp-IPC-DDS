// 同一源码分别链接 f066a82 与当前库；只使用两版本已有的公共 API。
#include "dzIPC/dzipc.h"
#include "dzIPC/common/sample_message.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static std::uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv) try {
    if (argc != 7) { std::cerr << "用法：benchmark pub|sub|scale shm|socket topic bytes output rate\n"; return 2; }
    const std::string role = argv[1], topic = argv[3], output = argv[5];
    const auto size = std::stoull(argv[4]); const auto rate = std::stoul(argv[6]);
    const auto transport = std::string(argv[2]) == "shm" ? dzIPC::IPC_SHM : dzIPC::IPC_SOCKET;
    dzIPC::EnableDzFlat(true); dzIPC::EnableNodelet(false);
    auto model = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 71);
    if (role == "scale") {
        std::vector<dzIPC::PublisherIPCPtr> pubs;
        for (std::size_t i = 0; i < size; ++i) {
            auto pub = dzIPC::PublisherIPCPtrMake(model, topic + std::to_string(i), 0, transport);
            pub->InitChannel(); pubs.push_back(std::move(pub));
        }
        std::cout << "{\"ready\":true,\"topics\":" << pubs.size() << "}" << std::endl;
        std::string command; std::getline(std::cin, command); pubs.clear();
        std::cout << "{\"closed\":true}" << std::endl; std::getline(std::cin, command); return 0;
    }
    if (size < 64 || size > 16 * 1024 * 1024 || !rate) return 2;
    std::ofstream csv(output); if (!csv) throw std::runtime_error("不能创建样本文件");
    if (role == "sub") {
        auto sub = dzIPC::SubscriberIPCPtrMake(model, topic, 0, 1024, transport); sub->InitChannel();
        std::atomic<bool> running{true}; std::uint64_t count = 0, invalid = 0;
        csv << "sequence,read_ns,elapsed_ns,bytes\n";
        std::thread reader([&] {
            while (running.load()) {
                dzIPC::Sample sample;
                if (!sub->get(sample, 20)) continue;
                const auto received = now();
                const auto view = sample.view<dzIPC::Msg::StdImageFlat>();
                if (!view.valid() || view.data().size() != size) { ++invalid; continue; }
                const auto* data = view.data().data();
                std::uint64_t stamp = 0, sequence = 0; std::memcpy(&stamp, data, 8); std::memcpy(&sequence, data + 8, 8);
                bool valid = received >= stamp;
                for (std::size_t i = 17; i < size; ++i) valid &= data[i] == 0xa5;
                if (!valid) ++invalid;
                if (data[16]) { ++count; csv << sequence << ',' << received << ',' << received - stamp << ',' << sample.size() << '\n'; }
            }
        });
        std::cout << "{\"ready\":true}" << std::endl;
        std::string command; std::getline(std::cin, command); running.store(false); reader.join(); csv.close();
        std::cout << "{\"received\":" << count << ",\"invalid\":" << invalid << "}" << std::endl;
        return invalid ? 1 : 0;
    }
    if (role != "pub") return 2;
    auto pub = dzIPC::PublisherIPCPtrMake(model, topic, 0, transport); pub->InitChannel();
    dzIPC::Msg::StdImage message; message.set_msg_id(71); message.width = size; message.height = 1; message.data.assign(size, 0xa5);
    std::vector<unsigned char> blob(message.dzflat_size());
    if (!message.dzflat_write(blob.data(), blob.size())) throw std::runtime_error("基准编码失败");
    const auto view = dzIPC::Msg::StdImageFlat::view_t::bind(blob.data(), blob.size());
    const auto offset = view.data().data() - blob.data();
    std::cout << "{\"ready\":true}" << std::endl;
    unsigned seconds = 0; std::cin >> seconds;
    const auto start = Clock::now(); const auto warmup = 2u; const auto total = std::uint64_t(seconds + warmup) * rate;
    std::uint64_t accepted = 0, rejected = 0;
    csv << "sequence,start_ns,elapsed_ns,success,bytes\n";
    for (std::uint64_t sequence = 0; sequence < total; ++sequence) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(sequence * 1000000000ull / rate));
        const auto stamp = now(); const bool measured = sequence >= std::uint64_t(warmup) * rate;
        std::memcpy(blob.data() + offset, &stamp, 8); std::memcpy(blob.data() + offset + 8, &sequence, 8); blob[offset + 16] = measured;
        const auto success = pub->publish_prebuilt_segment(blob.data(), blob.size()); const auto returned = now();
        if (measured) {
            success ? ++accepted : ++rejected;
            csv << sequence << ',' << stamp << ',' << returned - stamp << ',' << success << ',' << blob.size() << '\n';
        }
    }
    std::this_thread::sleep_until(start + std::chrono::seconds(seconds + warmup));
    csv.close();
    std::cout << "{\"accepted\":" << accepted << ",\"rejected\":" << rejected << ",\"window_seconds\":" << seconds << "}" << std::endl;
    std::string command; std::getline(std::cin, command); std::getline(std::cin, command);
    return 0;
} catch (const std::exception& e) { std::cerr << "基准失败：" << e.what() << '\n'; return 1; }
