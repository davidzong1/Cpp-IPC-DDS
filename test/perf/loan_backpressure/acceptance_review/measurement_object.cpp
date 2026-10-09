#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/measure/monotonic_clock.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/def.h"
#include "libipc/shm.h"

#ifndef DZIPC_LOAN_PERF_BASELINE
#define DZIPC_LOAN_PERF_BASELINE 0
#endif

namespace {
using namespace std::chrono_literals;
using dzIPC::measure::monotonic_now_ns;

struct Stats {
    std::vector<std::uint64_t> v;
    void add(std::uint64_t x) { v.push_back(x); }
    void merge(const Stats& s) { v.insert(v.end(), s.v.begin(), s.v.end()); }
    std::uint64_t pct(double p) const {
        if (v.empty()) return 0;
        auto c = v; std::sort(c.begin(), c.end());
        auto i = static_cast<std::size_t>(p * static_cast<double>(c.size() - 1));
        return c[i];
    }
    std::uint64_t max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

struct Args {
    int pubs = 1;
    int subs = 1;
    int windows = 1;
    std::size_t msgs = 2000;
    std::size_t payload = 11000;
    std::uint64_t rate = 0;
    std::uint64_t timeout = 100;
    bool require_all = false;
    bool raw = false;
    std::string csv_dir;
    std::string consume_mode = "mixed";
    std::uint64_t hold_us = 0;
    std::uint64_t slow_start_us = 0;
    bool metrics = true;
};

bool parse(int argc, char** argv, Args& a) {
    try { for (int i = 1; i < argc; ++i) {
        std::string s(argv[i]);
        auto eq = s.find('='); if (eq == std::string::npos) return false;
        auto k = s.substr(0, eq), val = s.substr(eq + 1);
        if (k != "--csv-dir" && k != "--transport" && k != "--consume-mode" &&
            (val.empty() || val.find_first_not_of("0123456789") != std::string::npos)) return false;
        if (k == "--pubs") a.pubs = std::stoi(val);
        else if (k == "--subs") a.subs = std::stoi(val);
        else if (k == "--windows") a.windows = std::stoi(val);
        else if (k == "--msgs") a.msgs = std::stoull(val);
        else if (k == "--payload") a.payload = std::stoull(val);
        else if (k == "--rate") a.rate = std::stoull(val);
        else if (k == "--timeout") a.timeout = std::stoull(val);
        else if (k == "--require-all") {
            if (val != "0" && val != "1") return false;
            a.require_all = (val == "1");
        }
        else if (k == "--transport") {
            if (val != "raw" && val != "dzflat") return false;
            a.raw = (val == "raw");
        }
        else if (k == "--csv-dir") a.csv_dir = val;
        else if (k == "--consume-mode") a.consume_mode = val;
        else if (k == "--hold-us") a.hold_us = std::stoull(val);
        else if (k == "--slow-start-us") a.slow_start_us = std::stoull(val);
        else if (k == "--metrics") {
            if (val != "0" && val != "1") return false;
            a.metrics = (val == "1");
        }
        else return false;
    } } catch (const std::exception&) { return false; }
    return a.pubs > 0 && a.pubs <= 32 && a.subs > 0 && a.subs <= 8 &&
           a.windows > 0 && a.msgs > 0 && a.msgs <= std::numeric_limits<std::uint32_t>::max() &&
           a.payload <= 16 * 1024 * 1024 && a.rate <= 1000000000 &&
           a.timeout <= 60000 &&
           a.hold_us <= 1000000 && a.slow_start_us <= 1000000 &&
           (a.consume_mode == "zero_copy" || a.consume_mode == "copy" || a.consume_mode == "mixed");
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

std::uint8_t payload_byte(std::size_t index, std::size_t offset) {
    return static_cast<std::uint8_t>((index * 131 + offset * 17) ^ (index >> 8));
}

struct Received {
    Stats recv, destroy, residence, hold, latency, app_work;
    std::vector<std::size_t> seen;
    std::size_t count = 0, flat = 0, tlv = 0, duplicate = 0, corrupt = 0, out_of_order = 0;
    std::uint64_t copied_bytes = 0;
    std::uint64_t completed_ns = 0;
};

struct ProcessUsage {
    std::uint64_t user_ns = 0;
    std::uint64_t system_ns = 0;
    std::uint64_t max_rss_kb = 0;
};

ProcessUsage process_usage() {
    ProcessUsage result;
#if !defined(_WIN32)
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
        result.user_ns = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000000ULL +
                         static_cast<std::uint64_t>(usage.ru_utime.tv_usec) * 1000ULL;
        result.system_ns = static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000000ULL +
                           static_cast<std::uint64_t>(usage.ru_stime.tv_usec) * 1000ULL;
        // Linux 的 ru_maxrss 单位为 KiB，macOS 为字节。
#if defined(__APPLE__)
        result.max_rss_kb = static_cast<std::uint64_t>(usage.ru_maxrss) / 1024ULL;
#else
        result.max_rss_kb = static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
    }
#endif
    return result;
}

void print_runtime(const ProcessUsage& before, const ProcessUsage& after,
                   std::size_t published, std::int64_t producer_elapsed_ns,
                   std::int64_t drain_elapsed_ns, std::size_t payload) {
    const auto user_ns = after.user_ns >= before.user_ns ? after.user_ns - before.user_ns : 0;
    const auto system_ns = after.system_ns >= before.system_ns ? after.system_ns - before.system_ns : 0;
    const auto cpu_ns = user_ns + system_ns;
    const double producer_seconds = producer_elapsed_ns > 0 ? producer_elapsed_ns / 1e9 : 0.0;
    const double drain_seconds = drain_elapsed_ns > 0 ? drain_elapsed_ns / 1e9 : 0.0;
    const double bytes = static_cast<double>(published) * static_cast<double>(payload);
    std::printf("cpu_user_ns=%llu cpu_system_ns=%llu cpu_total_ns=%llu rss_peak_kb=%llu "
                "throughput_msgs_per_s=%.3f throughput_bytes_per_s=%.3f "
                "drain_throughput_msgs_per_s=%.3f drain_throughput_bytes_per_s=%.3f ",
                (unsigned long long)user_ns, (unsigned long long)system_ns,
                (unsigned long long)cpu_ns, (unsigned long long)after.max_rss_kb,
                producer_seconds > 0.0 ? static_cast<double>(published) / producer_seconds : 0.0,
                producer_seconds > 0.0 ? bytes / producer_seconds : 0.0,
                drain_seconds > 0.0 ? static_cast<double>(published) / drain_seconds : 0.0,
                drain_seconds > 0.0 ? bytes / drain_seconds : 0.0);
}

// 接收/发布线程均完成分配后才开始计时，排空吞吐不包含 20 ms 静默核验。
std::uint64_t data_completed_ns(const std::vector<Received>& received, std::uint64_t publish_end) {
    for (const auto& r : received) publish_end = std::max(publish_end, r.completed_ns);
    return publish_end;
}

void wait_for_consumption(const std::vector<std::atomic<std::size_t>>& counts, std::size_t published) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::all_of(counts.begin(), counts.end(), [&](const auto& count) {
        return count.load(std::memory_order_acquire) >= published;
    }) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}

struct PublishFailures {
    std::size_t pool_exhausted = 0, other = 0;
    void add(ipc::loan_status reason) {
        if (reason == ipc::loan_status::pool_exhausted) ++pool_exhausted;
        else ++other;
    }
    void merge(const PublishFailures& value) {
        pool_exhausted += value.pool_exhausted;
        other += value.other;
    }
};

ipc::loan_t loan_for_publish(ipc::mpmc_channel& publisher, std::size_t size,
                             std::uint64_t timeout, ipc::loan_status& reason) {
#if DZIPC_LOAN_PERF_BASELINE
    // 修改前没有有界 loan；仅对同一显式 loan 入口作有截止时间的 yield 重试。
    // 数据构造和 try_publish_loan 与优化版本完全相同，不使用 try_send 的拷贝入口。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do {
        auto loan = publisher.loan(size, reason);
        if (loan.valid() || reason != ipc::loan_status::pool_exhausted) return loan;
        if (!timeout || std::chrono::steady_clock::now() >= deadline) return {};
        std::this_thread::yield();
    } while (true);
#else
    return publisher.loan(size, timeout, reason);
#endif
}

#if !DZIPC_LOAN_PERF_BASELINE
struct PoolRecorder {
    struct Row { std::uint64_t time; ipc::pool_snapshot snapshot; };
    std::function<bool(ipc::pool_snapshot&)> inspect;
    std::vector<Row> rows;
    std::atomic<bool> stop{false};
    std::thread sampler;
    bool valid = true, finished = false;
    PoolRecorder(std::function<bool(ipc::pool_snapshot&)> read, bool metrics) : inspect(std::move(read)) {
        sample();
        if (!metrics) return;
        sampler = std::thread([this] {
            auto next = std::chrono::steady_clock::now();
            while (!stop.load(std::memory_order_acquire)) {
                sample(); next += 2ms; std::this_thread::sleep_until(next);
            }
        });
    }
    ~PoolRecorder() { finish(); }
    void sample() {
        ipc::pool_snapshot s;
        if (inspect(s)) { valid = valid && s.consistent; rows.push_back({monotonic_now_ns(), s}); }
        else valid = false;
    }
    void finish() {
        if (finished) return;
        if (sampler.joinable()) { stop.store(true, std::memory_order_release); sampler.join(); }
        sample();
        finished = true;
    }
    bool report(const Args& a, int window) {
        finish();
        if (rows.empty()) return false;
        const auto& s = rows.back().snapshot;
        Stats used; for (const auto& row : rows) used.add(row.snapshot.used);
        std::printf("POOL transport=%s window=%d pubs=%d subs=%d payload=%zu rate=%llu generation=%llu "
                    "capacity=%u publisher_cap=%u final_free=%u waiters=%u consistent=%d used_min=%llu used_p50=%llu used_p95=%llu used_max=%u "
                    "loan_attempt=%llu loan_success=%llu loan_reject=%llu loan_wait_p50_ns=%llu loan_wait_p95_ns=%llu "
                    "loan_wait_p99_ns=%llu loan_wait_max_ns=%llu duplicate_return=%llu invalid_storage_id=%llu pool_chain_corrupt=%llu\n",
                    a.raw ? "raw" : "dzflat", window, a.pubs, a.subs, a.payload, (unsigned long long)a.rate,
                    (unsigned long long)s.generation, s.capacity, s.publisher_cap, s.free, s.waiters, s.consistent && valid,
                    (unsigned long long)*std::min_element(used.v.begin(), used.v.end()),
                    (unsigned long long)used.pct(.5), (unsigned long long)used.pct(.95), s.high_watermark,
                    (unsigned long long)s.loan_attempt, (unsigned long long)s.loan_success, (unsigned long long)s.loan_reject,
                    (unsigned long long)s.loan_wait_p50_ns, (unsigned long long)s.loan_wait_p95_ns,
                    (unsigned long long)s.loan_wait_p99_ns, (unsigned long long)s.loan_wait_max_ns,
                    (unsigned long long)s.duplicate_return, (unsigned long long)s.invalid_storage_id,
                    (unsigned long long)s.pool_chain_corrupt);
        if (!a.csv_dir.empty()) {
            std::filesystem::create_directories(a.csv_dir);
            const auto filename = std::string(a.raw ? "raw" : "dzflat") + "_p" + std::to_string(a.pubs) +
                "_s" + std::to_string(a.subs) + "_b" + std::to_string(a.payload) + "_r" + std::to_string(a.rate) +
                "_" + a.consume_mode + "_h" + std::to_string(a.hold_us) +
                "_q" + std::to_string(a.slow_start_us) + "_w" + std::to_string(window) + ".csv";
            std::ofstream file(std::filesystem::path(a.csv_dir) / filename);
            file << "monotonic_ns,generation,capacity,free,used,waiters,loan_attempt,loan_success,loan_reject,consistent\n";
            for (const auto& row : rows) {
                const auto& v = row.snapshot;
                file << row.time << ',' << v.generation << ',' << v.capacity << ',' << v.free << ',' << v.used << ','
                     << v.waiters << ',' << v.loan_attempt << ',' << v.loan_success << ',' << v.loan_reject << ',' << v.consistent << '\n';
            }
            if (!file.good()) valid = false;
        }
        return valid && s.consistent && s.capacity == ipc::topic_msg_cache && s.free == s.capacity && !s.waiters &&
            !s.duplicate_return && !s.invalid_storage_id && !s.pool_chain_corrupt;
    }
};
#endif

bool run(const Args& a, int window) {
    const auto topic = topic_name();
    auto topic_data = [] {
        return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 31);
    };
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    for (int i = 0; i < a.pubs; ++i) {
        pubs.emplace_back(std::make_unique<dzIPC::shm::shm_pub_ipc>(topic_data(), topic, 0));
        pubs.back()->InitChannel();
    }
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (int i = 0; i < a.subs; ++i) {
        subs.emplace_back(std::make_unique<dzIPC::shm::shm_sub_ipc>(topic_data(), topic, 0, ipc::topic_msg_cache));
#if !DZIPC_LOAN_PERF_BASELINE
        if (a.metrics) subs.back()->enable_pool_metrics();
#endif
        subs.back()->InitChannel();
    }
    dzIPC::Msg::StdImage prototype; prototype.data.resize(a.payload);
    const auto loan_size = prototype.dzflat_size();
    const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::all_of(pubs.begin(), pubs.end(), [&](const auto& p) {
        if (!p->channel_ready()) return false;
#if DZIPC_LOAN_PERF_BASELINE
        return std::all_of(subs.begin(), subs.end(), [](const auto& s) {
            return s->connected_generation() != 0;
        });
#else
        return p->recv_count() == static_cast<std::size_t>(a.subs);
#endif
    })) {
        if (std::chrono::steady_clock::now() >= ready_deadline) {
            std::fprintf(stderr, "channel readiness timed out\n");
            return false;
        }
        std::this_thread::yield();
    }
#if !DZIPC_LOAN_PERF_BASELINE
    PoolRecorder pools([&](auto& out) { return pubs[0]->inspect_pool(loan_size, out); }, a.metrics);
#endif

    std::vector<Stats> pub_stats(a.pubs);
    std::vector<PublishFailures> pub_failures(a.pubs);
    std::vector<Received> received(a.subs);
    std::vector<std::uint8_t> accepted(a.msgs, 0);
    std::vector<std::atomic<std::size_t>> consumed(a.subs);
    for (auto& count : consumed) count.store(0);
    std::atomic<std::size_t> published{0};
    std::atomic<bool> done{false}, start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> consumers;
    for (int s = 0; s < a.subs; ++s) consumers.emplace_back([&, s] {
        auto& r = received[s];
        r.seen.resize(a.msgs, 0);
        for (auto* stats : {&r.recv, &r.destroy, &r.residence, &r.hold, &r.latency, &r.app_work})
            stats->v.reserve(a.msgs);
        // 两条应用队列分别保序；混用两个 API 不能恢复分流前的总顺序。
        std::vector<std::size_t> last(a.pubs * 2, 0);
        std::vector<bool> have_last(a.pubs * 2, false);
        auto materialized = topic_data();
        dzIPC::Msg::StdImage copied;
        if (a.consume_mode == "copy") copied.data.reserve(a.payload);
        const auto consume = [&](std::uint32_t id, std::uint32_t publisher, std::uint32_t index,
                                 std::uint32_t step, const auto& payload, int lane) {
            ++r.count;
            if (id != 31 || publisher >= static_cast<std::uint32_t>(a.pubs) || index >= a.msgs ||
                index % a.pubs != publisher || step != a.payload || payload.size() != a.payload) {
                ++r.corrupt;
                return;
            }
            if (++r.seen[index] != 1) ++r.duplicate;
            const auto key = publisher + lane * a.pubs;
            if (have_last[key] && index <= last[key]) ++r.out_of_order;
            have_last[key] = true;
            last[key] = index;
            for (std::size_t i = 0; i < payload.size(); ++i) {
                if (payload[i] != payload_byte(index, i)) { ++r.corrupt; break; }
            }
        };
        bool draining = false;
        auto drain_deadline = std::chrono::steady_clock::time_point::max();
        auto quiet_since = std::chrono::steady_clock::now();
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        // 只减慢第 0 个应用消费者，传输接收线程保持持续 drain。
        if (s == 0 && a.slow_start_us) std::this_thread::sleep_for(std::chrono::microseconds(a.slow_start_us));
        while (true) {
            bool progress = false;
            dzIPC::Sample sample;
            const auto r0 = monotonic_now_ns();
            if (subs[s]->try_get(sample)) {
                const auto acquired = monotonic_now_ns();
                r.recv.add(acquired - r0);
#if !DZIPC_LOAN_PERF_BASELINE
                if (sample.enqueue_ns()) r.residence.add(acquired - sample.enqueue_ns());
#endif
                ++r.flat;
                const auto view = sample.view<dzIPC::Msg::StdImageFlat>();
                const auto id = sample.msg_id();
                const bool copy = a.consume_mode == "copy" && view.valid();
                const auto app_begin = monotonic_now_ns();
                if (copy) {
                    view.copy_to(copied);
                    r.copied_bytes += copied.data.size();
                } else {
                    if (!view.valid()) { ++r.count; ++r.corrupt; }
                    else consume(id, view.height(), view.width(), view.step(), view.data(), 0);
                    if (s == 0 && a.hold_us) std::this_thread::sleep_for(std::chrono::microseconds(a.hold_us));
                }
                const auto d0 = monotonic_now_ns();
                r.hold.add(d0 - acquired);
                sample = {};
                r.destroy.add(monotonic_now_ns() - d0);
                if (copy) {
                    // 此处只读 owning 对象。校验及应用处理均在共享样本释放之后。
                    consume(id, copied.height, copied.width, copied.step, copied.data, 0);
                    if (s == 0 && a.hold_us) std::this_thread::sleep_for(std::chrono::microseconds(a.hold_us));
                }
                r.app_work.add(monotonic_now_ns() - app_begin);
                progress = true;
            }
            if (subs[s]->try_get_clone(materialized)) {
                const auto m = materialized->topic()->msgcast<dzIPC::Msg::StdImage>();
                ++r.tlv;
                consume(m->msg_id(), m->height, m->width, m->step, m->data, 1);
                progress = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (progress) {
                quiet_since = now;
                r.completed_ns = monotonic_now_ns();
                consumed[s].store(r.count, std::memory_order_release);
            }
            if (done.load(std::memory_order_acquire)) {
                if (!draining) { draining = true; drain_deadline = now + 5s; }
                if ((r.count >= published.load(std::memory_order_relaxed) && now - quiet_since >= 20ms) ||
                    now >= drain_deadline) break;
            }
            if (!progress) {
                if (draining && r.count >= published.load(std::memory_order_relaxed))
                    std::this_thread::sleep_for(1ms);
                else std::this_thread::yield();
            }
        }
    });
    std::chrono::steady_clock::time_point begin;
    std::vector<std::thread> workers;
    for (int p = 0; p < a.pubs; ++p) workers.emplace_back([&, p] {
        auto& stats = pub_stats[p];
        stats.v.reserve((a.msgs + a.pubs - 1) / a.pubs);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t n = p; n < a.msgs; n += a.pubs) {
            if (a.rate) {
                const auto scheduled = begin + std::chrono::nanoseconds(n * (1000000000 / a.rate));
                std::this_thread::sleep_until(scheduled);
            }
            auto m = std::make_shared<dzIPC::Msg::StdImage>();
            m->set_msg_id(31);
            m->header.stamp = monotonic_now_ns();
            m->height = p;
            m->width = n;
            m->step = a.payload;
            m->data.resize(a.payload);
            for (std::size_t i = 0; i < a.payload; ++i) m->data[i] = payload_byte(n, i);
            const auto t0 = monotonic_now_ns();
            ipc::loan_status reason = ipc::loan_status::ok;
#if DZIPC_LOAN_PERF_BASELINE
            const bool okay = pubs[p]->publish_blocking(m, a.timeout);
#else
            const bool okay = pubs[p]->publish_blocking(m, a.timeout, reason);
#endif
            stats.add(monotonic_now_ns() - t0);
            if (okay) {
                accepted[n] = 1;
                published.fetch_add(1, std::memory_order_relaxed);
            } else pub_failures[p].add(reason);
        }
    });
    while (ready.load(std::memory_order_acquire) != a.pubs + a.subs) std::this_thread::yield();
    const auto usage_before = process_usage();
    begin = std::chrono::steady_clock::now();
    const auto begin_ns = monotonic_now_ns();
    start.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();
    const auto publish_end_ns = monotonic_now_ns();
    const auto elapsed = static_cast<std::int64_t>(publish_end_ns - begin_ns);
    done.store(true, std::memory_order_release);
    wait_for_consumption(consumed, published.load());
    const auto usage_after = process_usage();
    for (auto& t : consumers) t.join();
    const auto drain_elapsed = data_completed_ns(received, publish_end_ns) - begin_ns;

    Stats pub;
    PublishFailures failures;
    for (const auto& s : pub_stats) pub.merge(s);
    for (const auto& f : pub_failures) failures.merge(f);
    bool okay = !a.require_all || published.load() == a.msgs;
#if !DZIPC_LOAN_PERF_BASELINE
    okay = pools.report(a, window) && okay;
    okay = okay && pools.rows.back().snapshot.loan_reject == failures.pool_exhausted;
#endif
    okay = okay && published.load() > 0 && !failures.other &&
        published.load() + failures.pool_exhausted == a.msgs;
    for (int s = 0; s < a.subs; ++s) {
        const auto& r = received[s];
        std::size_t missing = 0, unexpected = 0;
        for (std::size_t n = 0; n < a.msgs; ++n) {
            if (accepted[n] && !r.seen[n]) ++missing;
            if (!accepted[n] && r.seen[n]) ++unexpected;
        }
        okay = okay && r.count == published.load() && !missing && !unexpected &&
            !r.duplicate && !r.corrupt && !r.out_of_order;
        okay = okay && r.copied_bytes == (a.consume_mode == "copy" ? r.count * a.payload : 0);
        std::printf("RESULT transport=dzflat window=%d pubs=%d subs=%d subscriber=%d msgs=%zu payload=%zu rate=%llu "
                    "attempted=%zu published=%zu publish_failed=%zu pool_exhausted=%zu other_failed=%zu received=%zu flat=%zu tlv=%zu "
                    "missing=%zu unexpected=%zu duplicate=%zu corrupt=%zu out_of_order=%zu elapsed_ns=%lld ",
                    window, a.pubs, a.subs, s, a.msgs, a.payload, (unsigned long long)a.rate,
                    a.msgs, published.load(), a.msgs - published.load(), failures.pool_exhausted, failures.other,
                    r.count, r.flat, r.tlv,
                    missing, unexpected, r.duplicate, r.corrupt, r.out_of_order, (long long)elapsed);
        std::printf("consume_mode=%s hold_us=%llu slow_start_us=%llu metrics=%d baseline=%d "
                    "copied_bytes=%llu drain_elapsed_ns=%llu ", a.consume_mode.c_str(),
                    (unsigned long long)a.hold_us, (unsigned long long)a.slow_start_us, a.metrics,
                    DZIPC_LOAN_PERF_BASELINE, (unsigned long long)r.copied_bytes, (unsigned long long)drain_elapsed);
        print_runtime(usage_before, usage_after, published.load(), elapsed, drain_elapsed, a.payload);
        print_stats("publish", pub); print_stats("recv", r.recv); print_stats("destroy", r.destroy);
        print_stats("queue_residence", r.residence); print_stats("sample_hold", r.hold);
        print_stats("app_work", r.app_work);
#if !DZIPC_LOAN_PERF_BASELINE
        const auto queues = subs[s]->inspect_queues();
        std::printf("queue_high_watermark=%llu queue_evicted=%llu consumer_lag=%zu ",
                    (unsigned long long)queues.queue_high_watermark, (unsigned long long)queues.queue_evicted, queues.queued);
        okay = okay && !queues.queue_evicted && !queues.queued;
#endif
        if (a.consume_mode != "mixed") okay = okay && r.flat == r.count && !r.tlv;
        std::printf("\n");
    }
    return okay;
}

// 统一接收流直接检查传输顺序，不经过视图/TLV 应用队列分流。
bool run_raw(const Args& a, int window) {
    struct Header { std::uint32_t publisher, index; std::uint64_t sent; };
    auto topic = topic_name();
    topic.erase(std::remove(topic.begin(), topic.end(), '/'), topic.end());
    std::vector<std::unique_ptr<ipc::mpmc_channel>> pubs, subs;
    for (int p = 0; p < a.pubs; ++p) pubs.emplace_back(
        std::make_unique<ipc::mpmc_channel>(topic.c_str(), ipc::sender, false));
    for (int s = 0; s < a.subs; ++s) subs.emplace_back(
        std::make_unique<ipc::mpmc_channel>(topic.c_str(), ipc::receiver, false));
    if (!pubs[0]->wait_for_recv(a.subs, 5000)) return false;
#if !DZIPC_LOAN_PERF_BASELINE
    PoolRecorder pools([&](auto& out) { return pubs[0]->inspect_pool(sizeof(Header) + a.payload, out); }, a.metrics);
#endif
    std::vector<Stats> pub_stats(a.pubs);
    std::vector<PublishFailures> pub_failures(a.pubs);
    std::vector<Received> received(a.subs);
    std::vector<std::uint8_t> accepted(a.msgs, 0);
    std::vector<std::atomic<std::size_t>> consumed(a.subs);
    for (auto& count : consumed) count.store(0);
    std::atomic<std::size_t> published{0};
    std::atomic<bool> done{false}, start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> consumers;
    for (int s = 0; s < a.subs; ++s) consumers.emplace_back([&, s] {
        auto& r = received[s]; r.seen.resize(a.msgs);
        for (auto* stats : {&r.recv, &r.destroy, &r.hold, &r.latency, &r.app_work})
            stats->v.reserve(a.msgs);
        std::vector<std::size_t> last(a.pubs, 0);
        std::vector<bool> have_last(a.pubs, false);
        std::vector<std::uint8_t> copied;
        if (a.consume_mode == "copy") copied.resize(sizeof(Header) + a.payload);
        const auto consume = [&](const void* data, std::size_t size, std::uint64_t acquired) {
            ++r.count;
            Header h{};
            if (size < sizeof(h) + a.payload) { ++r.corrupt; return; }
            std::memcpy(&h, data, sizeof(h));
            if (h.sent && h.sent <= acquired) r.latency.add(acquired - h.sent);
            else ++r.corrupt;
            if (h.publisher >= static_cast<unsigned>(a.pubs) || h.index >= a.msgs ||
                h.index % a.pubs != h.publisher) { ++r.corrupt; return; }
            if (++r.seen[h.index] != 1) ++r.duplicate;
            if (have_last[h.publisher] && h.index <= last[h.publisher]) ++r.out_of_order;
            have_last[h.publisher] = true; last[h.publisher] = h.index;
            auto* bytes = static_cast<const std::uint8_t*>(data) + sizeof(h);
            for (std::size_t i = 0; i < a.payload; ++i)
                if (bytes[i] != payload_byte(h.index, i)) { ++r.corrupt; break; }
        };
        auto deadline = std::chrono::steady_clock::time_point::max();
        auto quiet_since = std::chrono::steady_clock::now();
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (s == 0 && a.slow_start_us) std::this_thread::sleep_for(std::chrono::microseconds(a.slow_start_us));
        while (true) {
            const auto before = monotonic_now_ns();
            auto b = subs[s]->try_recv();
            if (!b.empty()) {
                quiet_since = std::chrono::steady_clock::now();
                const auto acquired = monotonic_now_ns();
                r.recv.add(acquired - before);
                const auto app_begin = monotonic_now_ns();
                const auto size = b.size();
                const bool copy = a.consume_mode == "copy" && size >= copied.size();
                if (copy) {
                    // raw 接收长度包含尺寸档填充；只复制实际消息字节。
                    std::memcpy(copied.data(), b.data(), copied.size());
                    r.copied_bytes += a.payload;
                } else {
                    consume(b.data(), size, acquired);
                    if (s == 0 && a.hold_us) std::this_thread::sleep_for(std::chrono::microseconds(a.hold_us));
                }
                const auto destroy = monotonic_now_ns(); b = {};
                r.hold.add(destroy - acquired);
                r.destroy.add(monotonic_now_ns() - destroy);
                if (copy) {
                    consume(copied.data(), copied.size(), acquired);
                    if (s == 0 && a.hold_us) std::this_thread::sleep_for(std::chrono::microseconds(a.hold_us));
                }
                r.app_work.add(monotonic_now_ns() - app_begin);
                r.completed_ns = monotonic_now_ns();
                consumed[s].store(r.count, std::memory_order_release);
            } else {
                if (done.load(std::memory_order_acquire) && r.count >= published.load())
                    std::this_thread::sleep_for(1ms);
                else std::this_thread::yield();
            }
            if (done.load(std::memory_order_acquire)) {
                if (deadline == std::chrono::steady_clock::time_point::max())
                    deadline = std::chrono::steady_clock::now() + 5s;
                const auto now = std::chrono::steady_clock::now();
                if ((r.count >= published.load() && now - quiet_since >= 20ms) || now >= deadline) break;
            }
        }
    });
    std::chrono::steady_clock::time_point begin;
    std::vector<std::thread> workers;
    for (int p = 0; p < a.pubs; ++p) workers.emplace_back([&, p] {
        pub_stats[p].v.reserve((a.msgs + a.pubs - 1) / a.pubs);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t n = p; n < a.msgs; n += a.pubs) {
            if (a.rate) std::this_thread::sleep_until(begin +
                std::chrono::nanoseconds(n * (1000000000 / a.rate)));
            const auto before = monotonic_now_ns();
            ipc::loan_status reason = ipc::loan_status::ok;
            auto lo = loan_for_publish(*pubs[p], sizeof(Header) + a.payload, a.timeout, reason);
            bool okay = false;
            if (lo.valid()) {
                Header h{static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(n), 0};
                auto* bytes = static_cast<std::uint8_t*>(lo.data) + sizeof(h);
                for (std::size_t i = 0; i < a.payload; ++i) bytes[i] = payload_byte(n, i);
                h.sent = monotonic_now_ns(); std::memcpy(lo.data, &h, sizeof(h));
                okay = pubs[p]->try_publish_loan(lo);
            }
            pub_stats[p].add(monotonic_now_ns() - before);
            if (okay) { accepted[n] = 1; ++published; }
            else pub_failures[p].add(reason);
        }
    });
    while (ready.load(std::memory_order_acquire) != a.pubs + a.subs) std::this_thread::yield();
    const auto usage_before = process_usage();
    begin = std::chrono::steady_clock::now();
    const auto begin_ns = monotonic_now_ns();
    start.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();
    const auto publish_end_ns = monotonic_now_ns();
    const auto elapsed = static_cast<std::int64_t>(publish_end_ns - begin_ns);
    done.store(true, std::memory_order_release);
    wait_for_consumption(consumed, published.load());
    const auto usage_after = process_usage();
    for (auto& t : consumers) t.join();
    const auto drain_elapsed = data_completed_ns(received, publish_end_ns) - begin_ns;
    Stats pub;
    PublishFailures failures;
    for (const auto& r : pub_stats) pub.merge(r);
    for (const auto& f : pub_failures) failures.merge(f);
    bool okay = !a.require_all || published.load() == a.msgs;
#if !DZIPC_LOAN_PERF_BASELINE
    okay = pools.report(a, window) && okay;
    okay = okay && pools.rows.back().snapshot.loan_reject == failures.pool_exhausted;
#endif
    okay = okay && published.load() > 0 && !failures.other &&
        published.load() + failures.pool_exhausted == a.msgs;
    for (int s = 0; s < a.subs; ++s) {
        const auto& r = received[s];
        std::size_t missing = 0, unexpected = 0;
        for (std::size_t n = 0; n < a.msgs; ++n) {
            if (accepted[n] && !r.seen[n]) ++missing;
            if (!accepted[n] && r.seen[n]) ++unexpected;
        }
        okay = okay && r.count == published.load() && !missing && !unexpected &&
            !r.duplicate && !r.corrupt && !r.out_of_order;
        okay = okay && r.copied_bytes == (a.consume_mode == "copy" ? r.count * a.payload : 0);
        std::printf("RESULT transport=raw window=%d pubs=%d subs=%d subscriber=%d msgs=%zu payload=%zu rate=%llu "
                    "attempted=%zu published=%zu publish_failed=%zu pool_exhausted=%zu other_failed=%zu received=%zu missing=%zu unexpected=%zu duplicate=%zu "
                    "corrupt=%zu out_of_order=%zu elapsed_ns=%lld ", window, a.pubs, a.subs, s, a.msgs,
                    a.payload, (unsigned long long)a.rate, a.msgs, published.load(), a.msgs-published.load(),
                    failures.pool_exhausted, failures.other,
                    r.count, missing, unexpected, r.duplicate, r.corrupt, r.out_of_order, (long long)elapsed);
        std::printf("consume_mode=%s hold_us=%llu slow_start_us=%llu metrics=%d baseline=%d "
                    "copied_bytes=%llu drain_elapsed_ns=%llu ", a.consume_mode.c_str(),
                    (unsigned long long)a.hold_us, (unsigned long long)a.slow_start_us, a.metrics,
                    DZIPC_LOAN_PERF_BASELINE, (unsigned long long)r.copied_bytes, (unsigned long long)drain_elapsed);
        print_runtime(usage_before, usage_after, published.load(), elapsed, drain_elapsed, a.payload);
        print_stats("publish", pub); print_stats("recv", r.recv); print_stats("destroy", r.destroy);
        print_stats("transport_latency", r.latency); print_stats("sample_hold", r.hold);
        print_stats("app_work", r.app_work);
        std::printf("\n");
    }
    return okay;
}
}

int main(int argc, char** argv) {
    Args a; if (!parse(argc, argv, a)) {
        std::fprintf(stderr, "usage: loan_phase_latency_measure --pubs=1..32 --subs=1..8 --msgs=N "
                             "--payload=N --windows=N --rate=N --timeout=N --require-all=0|1 "
                             "--transport=raw|dzflat --consume-mode=zero_copy|copy|mixed "
                             "--hold-us=N --slow-start-us=N --metrics=0|1 --csv-dir=PATH\n");
        return 2;
    }
    dzIPC::EnableNodelet(false);
    dzIPC::EnableDzFlat(true);
    ::setenv("DZIPC_SHM_MPMC", "1", 1);
    bool okay = true;
    for (int window = 1; window <= a.windows; ++window)
        okay = (a.raw ? run_raw(a, window) : run(a, window)) && okay;
    return okay ? 0 : 1;
}
