// 同一源码分别链接 f066a82 与当前库；采样循环使用两版本共有API。
// 当前版本的阶段诊断仅在采样结束后导出，不改变基线热路径。
#include "dzIPC/dzipc.h"
#if __has_include("dzIPC/net/metrics.h")
#include "dzIPC/net/client_runtime.h"
#define DZIPC_BENCH_STAGE_METRICS 1
#endif
#include "dzIPC/common/sample_message.h"
#include "dzIPC/detail/shm_sub_seam.h"
#if __has_include("libipc/detail/publish_trace.h")
#include "libipc/detail/publish_trace.h"
#define DZIPC_BENCH_PUBLISH_TRACE 1
#endif
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static std::uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
namespace {
constexpr std::size_t trace_capacity = 65536;
struct ReceiveTrace {
    std::atomic<std::uint64_t> begin{0}, received{0}, enqueue{0}, dequeue{0}, handoff{0};
    std::atomic<unsigned> assisted{0};
};
std::unique_ptr<ReceiveTrace[]> traces;
std::atomic<std::uint64_t> trace_overflow{0};
thread_local std::uint64_t recv_begin = 0;
thread_local std::uint64_t receive_sequence = UINT64_MAX;
thread_local std::uint64_t handoff_start = 0, handoff_elapsed = 0;
#ifdef DZIPC_BENCH_PUBLISH_TRACE
thread_local std::array<std::uint64_t, static_cast<unsigned>(ipc::detail::PublishPoint::Count)> publish_stamps{};
thread_local bool publish_active = false;
void publish_trace(ipc::detail::PublishPoint point) noexcept {
    if (publish_active) publish_stamps[static_cast<unsigned>(point)] = now();
}
#endif
void receive_trace(const dzIPC::detail::SeamEvent& event) noexcept {
    // 数值属于内部缝的稳定编号，同一源码也能链接尚无2/3/4打点的f066a82。
    const auto point = static_cast<int>(event.point);
    if (point == 8) { handoff_start = now(); return; }
    if (point == 9) { handoff_elapsed = now() - handoff_start; return; }
    if (point == 5) { if (receive_sequence < trace_capacity) traces[receive_sequence].assisted.store(1); return; }
    if (point != 0 && point != 2 && point != 3 && point != 4) return;
    const auto stamp = now();
    if (point == 2) { recv_begin = stamp; receive_sequence = UINT64_MAX; return; }
    if (!event.data || !event.size) return;
    const auto view = dzIPC::Msg::StdImageFlat::view_t::bind(event.data, event.size);
    if (!view.valid() || view.data().size() < 64) return;
    std::uint64_t sequence = 0;
    std::memcpy(&sequence, view.data().data() + 8, 8);
    if (sequence >= trace_capacity) { trace_overflow.fetch_add(1, std::memory_order_relaxed); return; }
    auto& trace = traces[sequence];
    if (point == 0) { receive_sequence = sequence; trace.begin.store(recv_begin, std::memory_order_relaxed); trace.received.store(stamp, std::memory_order_relaxed); }
    if (point == 3) trace.enqueue.store(stamp, std::memory_order_relaxed);
    if (point == 4) { trace.dequeue.store(stamp, std::memory_order_relaxed); trace.handoff.store(handoff_elapsed, std::memory_order_relaxed); handoff_elapsed = 0; }
}
}
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
        const bool tracing = std::getenv("DZIPC_TEST_RECEIVE_TRACE") && std::string(std::getenv("DZIPC_TEST_RECEIVE_TRACE")) == "1";
        if (tracing) { traces.reset(new ReceiveTrace[trace_capacity]); dzIPC::detail::SetSeamHook(receive_trace); }
        auto sub = dzIPC::SubscriberIPCPtrMake(model, topic, 0, 1024, transport); sub->InitChannel();
        std::atomic<bool> running{true}; std::uint64_t count = 0, invalid = 0;
        csv << "sequence,read_ns,elapsed_ns,bytes";
        if (tracing) csv << ",recv_begin_ns,recv_return_ns,enqueue_before_ns,dequeue_after_ns,assisted,handoff_ns";
        csv << '\n';
        std::thread reader([&] {
            while (running.load()) {
                dzIPC::Sample sample;
                handoff_elapsed = 0;
                if (!sub->get(sample, 20)) continue;
                const auto received = now();
                const auto view = sample.view<dzIPC::Msg::StdImageFlat>();
                if (!view.valid() || view.data().size() != size) { ++invalid; continue; }
                const auto* data = view.data().data();
                std::uint64_t stamp = 0, sequence = 0; std::memcpy(&stamp, data, 8); std::memcpy(&sequence, data + 8, 8);
                bool valid = received >= stamp;
                for (std::size_t i = 17; i < size; ++i) valid &= data[i] == 0xa5;
                if (!valid) ++invalid;
                if (data[16]) {
                    ++count; csv << sequence << ',' << received << ',' << received - stamp << ',' << sample.size();
                    if (tracing) {
                        if (sequence >= trace_capacity) { ++invalid; csv << ",0,0,0,0,0,0"; }
                        else {
                            const auto& trace = traces[sequence];
                            csv << ',' << trace.begin.load() << ',' << trace.received.load() << ',' << trace.enqueue.load() << ',' << trace.dequeue.load() << ',' << trace.assisted.load() << ',' << trace.handoff.load();
                        }
                    }
                    csv << '\n';
                }
            }
        });
        std::cout << "{\"ready\":true}" << std::endl;
        std::string command; std::getline(std::cin, command); running.store(false); reader.join(); sub.reset();
        dzIPC::detail::SetSeamHook(nullptr); csv.close();
        std::cout << "{\"received\":" << count << ",\"invalid\":" << invalid << ",\"trace_overflow\":" << trace_overflow.load() << "}" << std::endl;
        return invalid || trace_overflow.load() ? 1 : 0;
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
    const bool publish_tracing = std::getenv("DZIPC_TEST_PUBLISH_TRACE") && std::string(std::getenv("DZIPC_TEST_PUBLISH_TRACE")) == "1";
#ifdef DZIPC_BENCH_PUBLISH_TRACE
    if (publish_tracing) ipc::detail::set_publish_hook(publish_trace);
#else
    if (publish_tracing) throw std::runtime_error("该库没有发布分段诊断能力");
#endif
    csv << "sequence,start_ns,elapsed_ns,success,bytes";
    if (publish_tracing) csv << ",loan_begin_ns,loan_end_ns,copy_begin_ns,copy_end_ns,commit_begin_ns,notify_begin_ns,notify_end_ns,commit_end_ns";
    csv << '\n';
    for (std::uint64_t sequence = 0; sequence < total; ++sequence) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(sequence * 1000000000ull / rate));
        const auto stamp = now(); const bool measured = sequence >= std::uint64_t(warmup) * rate;
        std::memcpy(blob.data() + offset, &stamp, 8); std::memcpy(blob.data() + offset + 8, &sequence, 8); blob[offset + 16] = measured;
#ifdef DZIPC_BENCH_PUBLISH_TRACE
        if (publish_tracing) { publish_stamps.fill(0); publish_active = true; }
#endif
        const auto success = pub->publish_prebuilt_segment(blob.data(), blob.size()); const auto returned = now();
#ifdef DZIPC_BENCH_PUBLISH_TRACE
        publish_active = false;
#endif
        if (measured) {
            success ? ++accepted : ++rejected;
            csv << sequence << ',' << stamp << ',' << returned - stamp << ',' << success << ',' << blob.size();
#ifdef DZIPC_BENCH_PUBLISH_TRACE
            if (publish_tracing) for (const auto stamp : publish_stamps) csv << ',' << stamp;
#endif
            csv << '\n';
        }
    }
    std::this_thread::sleep_until(start + std::chrono::seconds(seconds + warmup));
    csv.close();
#ifdef DZIPC_BENCH_PUBLISH_TRACE
    ipc::detail::set_publish_hook(nullptr);
#endif
    std::cout << "{\"accepted\":" << accepted << ",\"rejected\":" << rejected << ",\"window_seconds\":" << seconds;
#ifdef DZIPC_BENCH_STAGE_METRICS
    if (transport == dzIPC::IPC_SOCKET) std::cout << ",\"diagnostics\":" << dzIPC::net::ClientRuntime::acquire(std::getenv("DZIPC_GATEWAY_CONTROL"))->diagnostics_json();
#endif
    std::cout << "}" << std::endl;
    std::string command; std::getline(std::cin, command); std::getline(std::cin, command);
    return 0;
} catch (const std::exception& e) { std::cerr << "基准失败：" << e.what() << '\n'; return 1; }
