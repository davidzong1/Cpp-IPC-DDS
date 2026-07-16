/// dzIPC Performance Benchmark — simplified, robust version
/// Usage: ./ipc_benchmark [--shm|--socket|--crc-bench] [--payload=N] [--duration=S]

#include "dzIPC/dzipc.h"
#include "dzIPC/common/crc32c.h"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
static uint64_t now_ns() { return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(); }

struct Result {
    std::string transport;
    std::size_t payload_bytes = 0;
    double duration_sec = 0;
    uint64_t sent = 0, recv = 0;
    std::vector<uint64_t> lat_ns;
    double msg_per_sec() const { return duration_sec > 0 ? recv / duration_sec : 0; }
    double mbps() const { return msg_per_sec() * payload_bytes * 8.0 / 1e6; }
    void report() const;
};

void Result::report() const {
    auto s = lat_ns; std::sort(s.begin(), s.end());
    auto p = [&](double q){ return s.empty() ? 0ULL : s[std::min(s.size()-1, size_t(q*s.size()))]; };
    double mean = s.empty() ? 0 : 0; for (auto v : s) mean += v; mean /= s.size();
    auto us = [](uint64_t ns) { return ns / 1000.0; };
    std::cout << "\n===== " << transport << " | " << payload_bytes << "B | " << duration_sec << "s =====\n"
              << "  sent=" << sent << " recv=" << recv << "\n"
              << "  rate=" << std::fixed << std::setprecision(0) << msg_per_sec() << " msg/s  "
              << std::setprecision(1) << mbps() << " Mbps\n"
              << "  lat(us): min=" << us(p(0)) << " mean=" << us(uint64_t(mean)) << " p50=" << us(p(0.5))
              << " p99=" << us(p(0.99)) << " p999=" << us(p(0.999)) << " max=" << us(p(1)) << "\n"
              << "//JSON{\"transport\":\"" << transport << "\",\"payload\":" << payload_bytes
              << ",\"msg_per_sec\":" << msg_per_sec() << ",\"mbps\":" << mbps()
              << ",\"lat_us_p50\":" << us(p(0.5)) << ",\"lat_us_p99\":" << us(p(0.99)) << "}\n";
}

static std::shared_ptr<dzIPC::TopicData> make_payload(std::size_t bytes) {
    auto m = std::make_shared<dzIPC::Msg::TestMsg>();
    size_t db = std::max<size_t>(1, bytes * 6 / 10 / sizeof(double));
    size_t ib = std::max<size_t>(4, bytes * 2 / 10 / sizeof(int32_t));  // at least 4 elements for ts
    size_t sc = std::max<size_t>(1, bytes / 10 / 64);
    m->data1.assign(db, 1.0);
    m->data2.assign(ib, 0);  // all zero initially
    for (size_t i = 0; i < sc; ++i) m->data3.push_back(std::string(64, 'x'));
    m->data4 = true;
    return std::make_shared<dzIPC::TopicData>(std::move(m), 42);
}

// --- SHM benchmark ---
static Result bench_shm(std::string topic, int domain, std::size_t payload_bytes, double dur_sec) {
    std::system(("rm -f /dev/shm/*" + topic + "* /dev/shm/__IPC_SHM__*" + topic + "* 2>/dev/null").c_str());
    Result r; r.transport = "shm"; r.payload_bytes = payload_bytes;

    std::atomic<bool> sub_ready{false}, run{true};
    std::atomic<uint32_t> start_seq{UINT32_MAX};  // first measurement seq
    std::mutex mtx; std::vector<uint64_t> lats;

    auto tpl = make_payload(payload_bytes);

    // subscriber
    std::thread st([&]{
        auto sub = dzIPC::SubscriberIPCPtrMake(tpl, topic, domain, 256, dzIPC::IPC_SHM, false);
        sub->InitChannel("bench"); sub_ready.store(true);
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
        while (run.load(std::memory_order_acquire)) {
            if (sub->try_get(rcv)) {
                auto* d = static_cast<dzIPC::Msg::TestMsg*>(rcv->topic().get());
                if (d->data2.size() < 4) continue;
                uint32_t seq = static_cast<uint32_t>(d->data2[0]);
                if (seq < start_seq.load(std::memory_order_acquire)) continue;  // filter warmup
                uint64_t sts = (uint64_t(static_cast<uint32_t>(d->data2[1])) << 32)
                             | uint64_t(static_cast<uint32_t>(d->data2[2]));
                if (sts > 0) {
                    std::lock_guard<std::mutex> lk(mtx);
                    lats.push_back(now_ns() - sts);
                }
            }
        }
    });

    while (!sub_ready.load()) std::this_thread::sleep_for(milliseconds(5));

    auto pub = dzIPC::PublisherIPCPtrMake(tpl, topic, domain, dzIPC::IPC_SHM, false);
    pub->InitChannel("bench");
    { auto dl = steady_clock::now() + seconds(5);
      while (!pub->has_subscribed() && steady_clock::now() < dl) std::this_thread::sleep_for(milliseconds(10)); }

    auto pmsg = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
    { auto* s = static_cast<dzIPC::Msg::TestMsg*>(tpl->topic().get());
      auto* d = static_cast<dzIPC::Msg::TestMsg*>(pmsg->topic().get());
      d->data1 = s->data1; d->data2 = s->data2; d->data3 = s->data3; d->data4 = s->data4; }
    auto* pd = static_cast<dzIPC::Msg::TestMsg*>(pmsg->topic().get());

    // warmup: 0.3s at 20K msg/s (zero timestamps, seq < start_seq)
    auto wu = steady_clock::now() + milliseconds(300);
    uint32_t wseq = 0;
    while (steady_clock::now() < wu) {
        pd->data2[0] = static_cast<int32_t>(wseq++); pd->data2[1] = 0; pd->data2[2] = 0;
        pub->publish_best_effort(pmsg->topic());
        std::this_thread::sleep_for(microseconds(50));
    }
    // drain consumer
    std::this_thread::sleep_for(milliseconds(100));

    // measurement
    uint32_t mseq = ++wseq;
    start_seq.store(mseq, std::memory_order_release);
    auto t0 = steady_clock::now();
    auto deadline = t0 + duration<double>(dur_sec);
    while (steady_clock::now() < deadline) {
        uint64_t ts = now_ns();
        pd->data2[0] = static_cast<int32_t>(mseq++);
        pd->data2[1] = static_cast<int32_t>(ts >> 32);
        pd->data2[2] = static_cast<int32_t>(ts & 0xFFFF'FFFFu);
        pub->publish_blocking(pmsg->topic(), 100);
        ++r.sent;
    }
    auto dt = steady_clock::now() - t0;
    std::this_thread::sleep_for(milliseconds(200));
    run.store(false); st.join();

    r.recv = lats.size(); r.lat_ns = std::move(lats);
    r.duration_sec = duration<double>(dt).count();
    return r;
}

// --- Socket benchmark (best-effort, localhost loopback) ---
static Result bench_socket(std::string topic, int domain, std::size_t payload_bytes, double dur_sec) {
    Result r; r.transport = "socket"; r.payload_bytes = payload_bytes;

    std::atomic<bool> sub_ready{false}, run{true};
    std::atomic<uint32_t> start_seq{UINT32_MAX};
    std::mutex mtx; std::vector<uint64_t> lats;

    auto tpl = make_payload(payload_bytes);

    std::thread st([&]{
        auto sub = dzIPC::SubscriberIPCPtrMake(tpl, topic, domain, 256, dzIPC::IPC_SOCKET, false);
        sub->InitChannel("bench"); sub_ready.store(true);
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
        while (run.load(std::memory_order_acquire)) {
            if (sub->try_get(rcv)) {
                auto* d = static_cast<dzIPC::Msg::TestMsg*>(rcv->topic().get());
                if (d->data2.size() < 4) continue;
                uint32_t seq = static_cast<uint32_t>(d->data2[0]);
                if (seq < start_seq.load(std::memory_order_acquire)) continue;
                uint64_t sts = (uint64_t(static_cast<uint32_t>(d->data2[1])) << 32)
                             | uint64_t(static_cast<uint32_t>(d->data2[2]));
                if (sts > 0) { std::lock_guard<std::mutex> lk(mtx); lats.push_back(now_ns() - sts); }
            }
        }
    });

    while (!sub_ready.load()) std::this_thread::sleep_for(milliseconds(5));

    auto pub = dzIPC::PublisherIPCPtrMake(tpl, topic, domain, dzIPC::IPC_SOCKET, false);
    pub->InitChannel("bench");
    { auto dl = steady_clock::now() + seconds(10);
      while (!pub->has_subscribed() && steady_clock::now() < dl) std::this_thread::sleep_for(milliseconds(50)); }
    std::this_thread::sleep_for(milliseconds(500));

    auto pmsg = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
    { auto* s = static_cast<dzIPC::Msg::TestMsg*>(tpl->topic().get());
      auto* d = static_cast<dzIPC::Msg::TestMsg*>(pmsg->topic().get());
      d->data1 = s->data1; d->data2 = s->data2; d->data3 = s->data3; d->data4 = s->data4; }
    auto* pd = static_cast<dzIPC::Msg::TestMsg*>(pmsg->topic().get());

    uint32_t wseq = 0;
    auto wu = steady_clock::now() + milliseconds(500);
    while (steady_clock::now() < wu) {
        pd->data2[0] = static_cast<int32_t>(wseq++); pd->data2[1] = 0; pd->data2[2] = 0;
        pub->publish_best_effort(pmsg->topic());
        std::this_thread::sleep_for(milliseconds(1));
    }

    uint32_t mseq = ++wseq;
    start_seq.store(mseq, std::memory_order_release);
    auto t0 = steady_clock::now();
    auto deadline = t0 + duration<double>(dur_sec);
    while (steady_clock::now() < deadline) {
        uint64_t ts = now_ns();
        pd->data2[0] = static_cast<int32_t>(mseq++);
        pd->data2[1] = static_cast<int32_t>(ts >> 32);
        pd->data2[2] = static_cast<int32_t>(ts & 0xFFFF'FFFFu);
        pub->publish_best_effort(pmsg->topic());
        ++r.sent;
    }
    auto dt = steady_clock::now() - t0;
    std::this_thread::sleep_for(milliseconds(300));
    run.store(false); st.join();

    r.recv = lats.size(); r.lat_ns = std::move(lats);
    r.duration_sec = duration<double>(dt).count();
    return r;
}

// --- CRC32C micro-benchmark ---
static void crc_bench() {
    std::cout << "\n--- CRC32C Micro-Benchmark (SSE4.2 hw enabled) ---\n";
    size_t sizes[] = {64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};
    for (auto sz : sizes) {
        std::vector<uint8_t> d(sz, 0xAB);
        int n = sz <= 4096 ? 100000 : sz <= 65536 ? 10000 : 1000;
        auto t0 = now_ns(); uint32_t sum = 0;
        for (int i = 0; i < n; ++i) sum ^= dzIPC::common::crc32c(d.data(), sz);
        double ns_op = double(now_ns() - t0) / n;
        double gbps = double(sz) * n * 8.0 / double(now_ns() - t0);
        std::cout << "  " << std::setw(8) << sz << "B: " << std::setw(8) << std::fixed << std::setprecision(1)
                  << ns_op << " ns/op  " << gbps << " Gbps  sum=" << std::hex << sum << std::dec << "\n";
    }
}

int main(int argc, char** argv) {
    std::string mode = "shm"; size_t payload = 256; double dur = 2.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shm") mode = "shm";
        else if (a == "--socket") mode = "socket";
        else if (a == "--crc-bench") { crc_bench(); return 0; }
        else if (a.rfind("--payload=",0)==0) payload = std::stoul(a.substr(10));
        else if (a.rfind("--duration=",0)==0) dur = std::stod(a.substr(11));
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: ipc_benchmark [--shm|--socket|--crc-bench] [--payload=N] [--duration=S]\n"; return 0;
        }
    }
    std::cout << "=== dzIPC Bench | " << mode << " | " << payload << "B | " << dur << "s ===\n";
    Result r = (mode == "shm") ? bench_shm("bench_01", 77, payload, dur)
                               : bench_socket("bench_01", 77, payload, dur);
    r.report();
    return 0;
}
