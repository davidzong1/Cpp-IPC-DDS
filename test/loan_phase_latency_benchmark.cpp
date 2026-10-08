#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/measure/monotonic_clock.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/def.h"
#include "libipc/shm.h"

namespace {
using namespace std::chrono_literals;
using dzIPC::measure::monotonic_now_ns;

struct Stats {
    std::vector<std::uint64_t> v;
    void add(std::uint64_t x) { v.push_back(x); }
    std::uint64_t pct(double p) const {
        if (v.empty()) return 0;
        auto c = v; std::sort(c.begin(), c.end());
        auto i = static_cast<std::size_t>(p * static_cast<double>(c.size() - 1));
        return c[i];
    }
    std::uint64_t max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

struct Args { int pubs = 1; std::size_t msgs = 2000; std::size_t payload = 11000; };

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string s(argv[i]);
        auto eq = s.find('='); if (eq == std::string::npos) return false;
        auto k = s.substr(0, eq), val = s.substr(eq + 1);
        if (k == "--pubs") a.pubs = std::stoi(val);
        else if (k == "--msgs") a.msgs = std::stoull(val);
        else if (k == "--payload") a.payload = std::stoull(val);
        else return false;
    }
    return a.pubs > 0 && a.pubs <= 16 && a.msgs > 0;
}

std::string topic_name() {
    return "/phase_latency_" + std::to_string(monotonic_now_ns());
}

void print_stats(const char* name, const Stats& s) {
    std::printf("%s_n=%zu %s_p50_ns=%llu %s_p95_ns=%llu %s_p99_ns=%llu %s_max_ns=%llu ",
                name, s.v.size(), name, (unsigned long long)s.pct(.50),
                name, (unsigned long long)s.pct(.95), name, (unsigned long long)s.pct(.99),
                name, (unsigned long long)s.max());
}
}

int main(int argc, char** argv) {
    Args a; if (!parse(argc, argv, a)) {
        std::fprintf(stderr, "usage: loan_phase_latency_benchmark --pubs=N --msgs=N --payload=N\n");
        return 2;
    }
    dzIPC::EnableDzFlat(true);
    auto topic = topic_name();
    auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 31);
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    for (int i = 0; i < a.pubs; ++i) {
        pubs.emplace_back(std::make_unique<dzIPC::shm::shm_pub_ipc>(td, topic, 0));
        pubs.back()->InitChannel();
    }
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 31);
    dzIPC::shm::shm_sub_ipc sub(sub_td, topic, 0, 10);
    sub.InitChannel();
    auto image = std::make_shared<dzIPC::Msg::StdImage>();
    image->set_msg_id(31);
    image->data.resize(a.payload, 0x5a);
    std::this_thread::sleep_for(100ms);

    Stats pub, recv, destroy; std::atomic<std::size_t> attempted{0}, published{0}, got{0};
    std::atomic<bool> stop{false};
    std::thread consumer([&] {
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline &&
               (!stop.load(std::memory_order_relaxed) || got.load() < published.load())) {
            dzIPC::Sample sample;
            auto r0 = monotonic_now_ns();
            if (!sub.try_get(sample)) { std::this_thread::yield(); continue; }
            auto r1 = monotonic_now_ns();
            recv.add(r1 - r0);
            ++got;
            auto d0 = monotonic_now_ns();
            sample = dzIPC::Sample{};
            auto d1 = monotonic_now_ns();
            destroy.add(d1 - d0);
        }
    });
    std::vector<std::thread> workers;
    for (int p = 0; p < a.pubs; ++p) workers.emplace_back([&, p] {
        while (true) {
            auto n = attempted.fetch_add(1);
            if (n >= a.msgs) break;
            auto m = std::make_shared<dzIPC::Msg::StdImage>(*image);
            auto t0 = monotonic_now_ns();
            bool ok = pubs[p]->publish(m);
            auto t1 = monotonic_now_ns();
            if (ok) { pub.add(t1 - t0); ++published; }
        }
    });
    for (auto& t : workers) t.join();
    stop.store(true);
    consumer.join();
    std::printf("RESULT pubs=%d msgs=%zu payload=%zu attempted=%zu published=%zu received=%zu ",
                a.pubs, a.msgs, a.payload, attempted.load(), published.load(), got.load());
    print_stats("publish", pub); print_stats("recv", recv); print_stats("destroy", destroy);
    std::printf("\n");
    return 0;
}
