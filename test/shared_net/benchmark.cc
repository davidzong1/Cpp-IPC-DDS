// 同一源码分别链接 f066a82 与当前库；采样循环使用两版本共有API。
// 当前版本的阶段诊断仅在采样结束后导出，不改变基线热路径。
#include "dzIPC/dzipc.h"
#if __has_include("dzIPC/net/metrics.h")
#include "dzIPC/net/client_runtime.h"
#define DZIPC_BENCH_STAGE_METRICS 1
#endif
#include "dzIPC/common/sample_message.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "libipc/recv_wait_set.h"
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
#include <string_view>
#if defined(__linux__)
#include <sched.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static std::uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
namespace {
static std::uint64_t thread_cpu_now() noexcept {
#if defined(__linux__)
    timespec value{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return std::uint64_t(value.tv_sec) * 1000000000ull + value.tv_nsec;
#endif
    return 0;
}
constexpr std::size_t trace_capacity = 65536;
constexpr std::size_t wait_trace_capacity = 32768;
struct ReceiveTrace {
    std::atomic<std::uint64_t> begin{0}, received{0}, enqueue{0}, dequeue{0}, handoff{0};
    std::atomic<unsigned> assisted{0};
    std::atomic<std::uint64_t> wait_begin{0}, wait_end{0}, wait_cpu{0}, recv_cpu{0}, process_cpu{0}, enqueue_after{0}, acquire_ns{0};
    std::atomic<std::uint64_t> receiver_tid{0}, receiver_wait_begin{0}, receiver_wait_end{0};
    std::atomic<int> receiver_cpu{-1};
    std::atomic<std::uint32_t> generation{0};
};
struct WaitTraceRecord {
    std::uint64_t timestamp_ns{0};
    std::uint64_t route_key{0};
    std::uint32_t pid{0}, tid{0}, subscribers{0}, payload_bytes{0}, subscriber_id{0}, generation{0};
    int cpu{-1};
    ipc::detail::recv_wait_trace_event event{};
};
std::unique_ptr<ReceiveTrace[]> traces;
std::unique_ptr<WaitTraceRecord[]> wait_traces;
std::atomic<std::uint64_t> trace_overflow{0};
std::atomic<std::uint64_t> wait_trace_count{0}, wait_trace_overflow{0};
std::atomic<std::uint32_t> wait_trace_generation{0};
std::atomic<bool> receive_trace_enabled{false};
std::uint64_t wait_trace_route_key = 0;
std::uint32_t wait_trace_subscribers = 0, wait_trace_payload_bytes = 0, wait_trace_subscriber_id = UINT32_MAX;
std::string wait_trace_mode;
thread_local std::uint64_t recv_begin = 0;
thread_local std::uint64_t receive_sequence = UINT64_MAX;
thread_local std::uint64_t handoff_start = 0, handoff_elapsed = 0;
thread_local std::uint64_t wait_begin = 0, wait_end = 0, wait_cpu_begin = 0, wait_cpu = 0, recv_cpu_begin = 0, received_cpu = 0, acquire_begin = 0, acquire_ns = 0;
thread_local std::uint64_t receiver_wait_begin = 0, receiver_wait_end = 0;
thread_local bool receiver_wait_active = false;
std::uint64_t route_key(std::string_view name, std::uint32_t domain) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : name) { hash ^= c; hash *= 1099511628211ull; }
    for (int i = 0; i < 4; ++i) { hash ^= (domain >> (8 * i)) & 0xffu; hash *= 1099511628211ull; }
    return hash;
}
std::uint32_t subscriber_id_from_path(const std::string& path) noexcept {
    const auto marker = path.rfind("sub");
    if (marker == std::string::npos) return UINT32_MAX;
    char* end = nullptr;
    const auto value = std::strtoul(path.c_str() + marker + 3, &end, 10);
    return end != path.c_str() + marker + 3 ? static_cast<std::uint32_t>(value) : UINT32_MAX;
}
void wait_trace(const ipc::detail::recv_wait_trace_event& event) noexcept {
    using Point = ipc::detail::recv_wait_trace_point;
    if (event.point == Point::set_wait_begin || event.point == Point::local_wait_begin) {
        receiver_wait_begin = now();
        receiver_wait_end = 0;
        receiver_wait_active = true;
    } else if (event.point == Point::set_wait_end || event.point == Point::local_wait_end) {
        if (receiver_wait_active) receiver_wait_end = now();
        else receiver_wait_begin = receiver_wait_end = 0;
        receiver_wait_active = false;
    }
    if (!wait_traces) return;
    const auto index = wait_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= wait_trace_capacity) { wait_trace_overflow.fetch_add(1, std::memory_order_relaxed); return; }
    auto& record = wait_traces[index];
    record.timestamp_ns = now();
    record.route_key = wait_trace_route_key;
    record.pid =
#if defined(__linux__)
        static_cast<std::uint32_t>(::getpid());
#else
        0;
#endif
    record.tid =
#if defined(__linux__)
        static_cast<std::uint32_t>(::syscall(SYS_gettid));
#else
        0;
#endif
    record.cpu =
#if defined(__linux__)
        ::sched_getcpu();
#else
        -1;
#endif
    record.subscribers = wait_trace_subscribers;
    record.payload_bytes = wait_trace_payload_bytes;
    record.subscriber_id = wait_trace_subscriber_id;
    record.generation = wait_trace_generation.load(std::memory_order_relaxed);
    record.event = event;
}
void flush_wait_trace(const std::string& output) {
    if (!wait_traces) return;
    std::ofstream file(output + ".wait_trace.csv");
    if (!file) throw std::runtime_error("不能创建recv_wait_set trace文件");
    file << "timestamp_ns,pid,tid,cpu,mode,subscribers,payload_bytes,subscriber_id,route_key,generation,point,result,flags,object,token,error_code,wake_result,sequence_before,sequence_after,expected,observed,interrupt_before,interrupt_after,waiters,entries,enabled,ready\n";
    const auto count = std::min<std::uint64_t>(wait_trace_count.load(std::memory_order_acquire), wait_trace_capacity);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto& r = wait_traces[i]; const auto& e = r.event;
        file << r.timestamp_ns << ',' << r.pid << ',' << r.tid << ',' << r.cpu << ',' << wait_trace_mode << ','
             << r.subscribers << ',' << r.payload_bytes << ',' << r.subscriber_id << ',' << r.route_key << ',' << r.generation << ','
             << static_cast<unsigned>(e.point) << ',' << static_cast<int>(e.result) << ',' << e.flags << ','
             << reinterpret_cast<std::uintptr_t>(e.object) << ',' << reinterpret_cast<std::uintptr_t>(e.token) << ','
             << e.error_code << ',' << e.wake_result << ',' << e.sequence_before << ',' << e.sequence_after << ','
             << e.expected << ',' << e.observed << ',' << e.interrupt_before << ',' << e.interrupt_after << ','
             << e.waiters << ',' << e.entries << ',' << e.enabled << ',' << e.ready << '\n';
    }
    file.close();
    if (!file) throw std::runtime_error("写入recv_wait_set trace文件失败");
}
#ifdef DZIPC_BENCH_PUBLISH_TRACE
thread_local std::array<std::uint64_t, static_cast<unsigned>(ipc::detail::PublishPoint::Count)> publish_stamps{};
thread_local bool publish_active = false;
thread_local bool publish_cpu_tracing = false;
struct CopyCpuTrace {
    std::uint64_t cpu_begin{0}, cpu_end{0}, outer_begin{0}, outer_end{0};
    int begin_cpu{-1}, end_cpu{-1};
};
thread_local CopyCpuTrace copy_cpu;
void publish_trace(ipc::detail::PublishPoint point) noexcept {
    if (!publish_active) return;
#if defined(__linux__)
    if (publish_cpu_tracing && point == ipc::detail::PublishPoint::BeforeCopy) {
        copy_cpu.begin_cpu = ::sched_getcpu();
        copy_cpu.outer_begin = now();
        copy_cpu.cpu_begin = thread_cpu_now();
        publish_stamps[static_cast<unsigned>(point)] = now();
        return;
    }
    if (publish_cpu_tracing && point == ipc::detail::PublishPoint::AfterCopy) {
        publish_stamps[static_cast<unsigned>(point)] = now();
        copy_cpu.cpu_end = thread_cpu_now();
        copy_cpu.outer_end = now();
        copy_cpu.end_cpu = ::sched_getcpu();
        return;
    }
#endif
    publish_stamps[static_cast<unsigned>(point)] = now();
}
#endif
void receive_trace(const dzIPC::detail::SeamEvent& event) noexcept {
    // 数值属于内部缝的稳定编号，同一源码也能链接尚无2/3/4打点的f066a82。
    const auto point = static_cast<int>(event.point);
    if (point == 32 || point == 33) {
        wait_trace_generation.store(event.generation, std::memory_order_relaxed);
        return;
    }
    if (!receive_trace_enabled.load(std::memory_order_relaxed)) return;
    if (point == 7) { wait_begin = now(); wait_cpu_begin = thread_cpu_now(); return; }
    if (point == 10) { wait_end = now(); wait_cpu = thread_cpu_now() - wait_cpu_begin; return; }
    if (point == 11) { acquire_begin = now(); return; }
    if (point == 12) { acquire_ns = now() - acquire_begin; return; }
    if (point == 13) { if (receive_sequence < trace_capacity) traces[receive_sequence].enqueue_after.store(now()); return; }
    if (point == 8) { handoff_start = now(); return; }
    if (point == 9) { handoff_elapsed = now() - handoff_start; return; }
    if (point == 5) { if (receive_sequence < trace_capacity) traces[receive_sequence].assisted.store(1); return; }
    if (point != 0 && point != 2 && point != 3 && point != 4) return;
    const auto stamp = now();
    if (point == 2) { recv_begin = stamp; recv_cpu_begin = thread_cpu_now(); receive_sequence = UINT64_MAX; return; }
    if (!event.data || !event.size) return;
    const auto view = dzIPC::Msg::StdImageFlat::view_t::bind(event.data, event.size);
    if (!view.valid() || view.data().size() < 64) return;
    std::uint64_t sequence = 0;
    std::memcpy(&sequence, view.data().data() + 8, 8);
    if (sequence >= trace_capacity) { trace_overflow.fetch_add(1, std::memory_order_relaxed); return; }
    auto& trace = traces[sequence];
    if (point == 0) {
        receive_sequence = sequence;
        trace.begin.store(recv_begin, std::memory_order_relaxed);
        trace.received.store(stamp, std::memory_order_relaxed);
        received_cpu = thread_cpu_now();
        trace.recv_cpu.store(received_cpu - recv_cpu_begin);
        trace.receiver_wait_begin.store(receiver_wait_begin);
        trace.receiver_wait_end.store(receiver_wait_end);
        trace.generation.store(event.generation);
#if defined(__linux__)
        trace.receiver_tid.store(::syscall(SYS_gettid));
        trace.receiver_cpu.store(::sched_getcpu());
#endif
    }
    if (point == 3) { trace.enqueue.store(stamp, std::memory_order_relaxed); trace.process_cpu.store(thread_cpu_now() - received_cpu); }
    if (point == 4) { trace.dequeue.store(stamp, std::memory_order_relaxed); trace.handoff.store(handoff_elapsed, std::memory_order_relaxed); handoff_elapsed = 0; trace.wait_begin.store(wait_begin); trace.wait_end.store(wait_end); trace.wait_cpu.store(wait_cpu); trace.acquire_ns.store(acquire_ns); }
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
        const bool wait_tracing = std::getenv("DZIPC_TEST_WAIT_TRACE") && std::string(std::getenv("DZIPC_TEST_WAIT_TRACE")) == "1";
        if (tracing) traces.reset(new ReceiveTrace[trace_capacity]);
        if (wait_tracing) {
            wait_traces.reset(new WaitTraceRecord[wait_trace_capacity]);
            wait_trace_count.store(0, std::memory_order_relaxed);
            wait_trace_overflow.store(0, std::memory_order_relaxed);
            wait_trace_route_key = route_key(topic, 0);
            wait_trace_subscribers = std::getenv("DZIPC_TEST_CASE_SUBSCRIBERS") ?
                static_cast<std::uint32_t>(std::strtoul(std::getenv("DZIPC_TEST_CASE_SUBSCRIBERS"), nullptr, 10)) : 0;
            wait_trace_payload_bytes = static_cast<std::uint32_t>(size);
            wait_trace_subscriber_id = subscriber_id_from_path(output);
            wait_trace_mode = std::getenv("DZIPC_TEST_CASE_MODE") ? std::getenv("DZIPC_TEST_CASE_MODE") : "unknown";
        }
        receive_trace_enabled.store(tracing, std::memory_order_relaxed);
        if (tracing || wait_tracing) dzIPC::detail::SetSeamHook(receive_trace);
        if (tracing || wait_tracing) ipc::detail::set_recv_wait_trace_hook(wait_trace);
        auto sub = dzIPC::SubscriberIPCPtrMake(model, topic, 0, 1024, transport); sub->InitChannel();
        std::atomic<bool> running{true}; std::uint64_t count = 0, invalid = 0;
        csv << "sequence,read_ns,elapsed_ns,bytes";
        if (tracing) csv << ",recv_begin_ns,recv_return_ns,enqueue_before_ns,dequeue_after_ns,assisted,handoff_ns,wait_begin_ns,wait_end_ns,wait_cpu_ns,recv_cpu_ns,process_cpu_ns,enqueue_after_ns,assist_acquire_ns,get_cpu_ns,reader_tid,receiver_tid,receiver_cpu,receiver_wait_begin_ns,receiver_wait_end_ns,generation";
        csv << '\n';
        std::thread reader([&] {
#if defined(__linux__)
            const auto reader_tid = tracing ? ::syscall(SYS_gettid) : 0;
#else
            const auto reader_tid = 0;
#endif
            while (running.load()) {
                dzIPC::Sample sample;
                handoff_elapsed = 0;
                wait_begin = wait_end = wait_cpu = acquire_ns = 0;
                const auto get_cpu_begin = tracing ? thread_cpu_now() : 0;
                if (!sub->get(sample, 20)) continue;
                const auto received = now();
                const auto get_cpu = tracing ? thread_cpu_now() - get_cpu_begin : 0;
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
                        if (sequence >= trace_capacity) { ++invalid; csv << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0"; }
                        else {
                            const auto& trace = traces[sequence];
                            csv << ',' << trace.begin.load() << ',' << trace.received.load() << ',' << trace.enqueue.load() << ',' << trace.dequeue.load() << ',' << trace.assisted.load() << ',' << trace.handoff.load()
                                << ',' << trace.wait_begin.load() << ',' << trace.wait_end.load() << ',' << trace.wait_cpu.load()
                                << ',' << trace.recv_cpu.load() << ',' << trace.process_cpu.load() << ',' << trace.enqueue_after.load()
                                << ',' << trace.acquire_ns.load() << ',' << get_cpu;
                        }
                        csv << ',' << reader_tid;
                        if (sequence >= trace_capacity) csv << ",0,-1,0,0,0";
                        else {
                            const auto& trace = traces[sequence];
                            csv << ',' << trace.receiver_tid.load() << ',' << trace.receiver_cpu.load()
                                << ',' << trace.receiver_wait_begin.load() << ',' << trace.receiver_wait_end.load()
                                << ',' << trace.generation.load();
                        }
                    }
                    csv << '\n';
                }
            }
        });
        std::cout << "{\"ready\":true}" << std::endl;
        std::string command; std::getline(std::cin, command); running.store(false); reader.join(); sub.reset();
        if (tracing || wait_tracing) ipc::detail::set_recv_wait_trace_hook(nullptr);
        dzIPC::detail::SetSeamHook(nullptr); receive_trace_enabled.store(false, std::memory_order_relaxed); csv.close();
        flush_wait_trace(output);
        std::cout << "{\"received\":" << count << ",\"invalid\":" << invalid << ",\"trace_overflow\":" << trace_overflow.load()
                  << ",\"wait_trace_events\":" << wait_trace_count.load() << ",\"wait_trace_overflow\":" << wait_trace_overflow.load() << "}" << std::endl;
        return invalid || trace_overflow.load() || wait_trace_overflow.load() ? 1 : 0;
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
    const bool cpu_tracing = std::getenv("DZIPC_TEST_PUBLISH_CPU_TRACE") && std::string(std::getenv("DZIPC_TEST_PUBLISH_CPU_TRACE")) == "1";
    if (cpu_tracing && !publish_tracing) throw std::runtime_error("CPU诊断必须同时开启发布分段");
#if !defined(__linux__)
    if (cpu_tracing) throw std::runtime_error("当前平台不支持复制CPU诊断");
#endif
#ifdef DZIPC_BENCH_PUBLISH_TRACE
    publish_cpu_tracing = cpu_tracing;
    if (publish_tracing) ipc::detail::set_publish_hook(publish_trace);
#else
    if (publish_tracing) throw std::runtime_error("该库没有发布分段诊断能力");
#endif
    csv << "sequence,start_ns,elapsed_ns,success,bytes";
    if (publish_tracing) csv << ",loan_begin_ns,loan_end_ns,copy_begin_ns,copy_end_ns,commit_begin_ns,notify_begin_ns,notify_end_ns,commit_end_ns";
    if (cpu_tracing) csv << ",copy_cpu_begin_ns,copy_cpu_end_ns,copy_outer_begin_ns,copy_outer_end_ns,copy_begin_cpu,copy_end_cpu";
    csv << '\n';
    for (std::uint64_t sequence = 0; sequence < total; ++sequence) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(sequence * 1000000000ull / rate));
        const auto stamp = now(); const bool measured = sequence >= std::uint64_t(warmup) * rate;
        std::memcpy(blob.data() + offset, &stamp, 8); std::memcpy(blob.data() + offset + 8, &sequence, 8); blob[offset + 16] = measured;
#ifdef DZIPC_BENCH_PUBLISH_TRACE
        if (publish_tracing) { publish_stamps.fill(0); publish_active = true; }
        if (cpu_tracing) copy_cpu = {};
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
            if (cpu_tracing) csv << ',' << copy_cpu.cpu_begin << ',' << copy_cpu.cpu_end << ',' << copy_cpu.outer_begin << ',' << copy_cpu.outer_end << ',' << copy_cpu.begin_cpu << ',' << copy_cpu.end_cpu;
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
