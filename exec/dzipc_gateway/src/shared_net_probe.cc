#include "dzIPC/net/publisher_endpoint.h"
#include "dzIPC/common/channel_scope.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "libipc/ipc.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <set>
#include <algorithm>
#include <numeric>

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
std::vector<std::string> split_topics(const std::string& text) {
    std::vector<std::string> topics; std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin); const auto length = (end == std::string::npos ? text.size() : end) - begin;
        if (!length) throw std::invalid_argument("话题列表包含空项");
        topics.push_back(text.substr(begin, length));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return topics;
}
struct LoadResult { std::uint64_t sent = 0, failed = 0; std::vector<std::uint64_t> latency; };
std::uint64_t nearest_rank(const std::vector<std::uint64_t>& values, double percentile) {
    if (values.empty()) return 0;
    const auto rank = static_cast<std::size_t>(std::ceil(values.size() * percentile));
    return values[std::max<std::size_t>(1, rank) - 1];
}
void run_load(PublisherEndpoint& publisher, unsigned seconds, double rate, unsigned size,
              bool reliable, std::uint64_t tag,
              std::chrono::steady_clock::time_point start, LoadResult& result) {
    const auto end = start + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end && result.sent < 600000) {
        if (rate > 0) {
            const auto due = start + std::chrono::nanoseconds(
                static_cast<std::uint64_t>(result.sent * 1000000000.0 / rate));
            std::this_thread::sleep_until(due);
        }
        if (std::chrono::steady_clock::now() >= end) break;
        const auto began = std::chrono::steady_clock::now();
        ++result.sent;
        std::uint64_t identity = 0;
        const auto publisher_id = publisher.publisher_id();
        std::memcpy(&identity, publisher_id.data(), sizeof(identity));
        const auto seed = (identity & 0xffffffff00000000ull) | (result.sent & 0xffffffffull);
        auto bytes = payload(size, tag, seed);
        const auto outcome = publisher.prebuilt(ByteView(bytes),
            reliable ? Delivery::Reliable : Delivery::BestEffort, reliable ? 5000 : 0);
        result.failed += !outcome.success;
        result.latency.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - began).count());
    }
    std::sort(result.latency.begin(), result.latency.end());
}
void print_load_result(const LoadResult& result, bool include_samples) {
    const auto mean = result.latency.empty() ? 0.0 :
        static_cast<double>(std::accumulate(result.latency.begin(), result.latency.end(), std::uint64_t{0})) / result.latency.size();
    std::cout << "{\"sent\":" << result.sent << ",\"failed\":" << result.failed
              << ",\"mean_ns\":" << mean << ",\"p50_ns\":" << nearest_rank(result.latency, .50)
              << ",\"p95_ns\":" << nearest_rank(result.latency, .95)
              << ",\"p99_ns\":" << nearest_rank(result.latency, .99)
              << ",\"max_ns\":" << nearest_rank(result.latency, 1.0);
    if (include_samples) {
        std::cout << ",\"latency_samples_ns\":[";
        for (std::size_t i = 0; i < result.latency.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << result.latency[i];
        }
        std::cout << ']';
    }
    std::cout << '}';
}
}
int main(int argc, char** argv) try {
    if (argc != 4) { std::cerr << "用法：shared_net_probe 控制路径 话题 pub|sub|both\n"; return 2; }
    const std::string role = argv[3];
    if (role != "pub" && role != "sub" && role != "both" && role != "pubset" &&
        role != "bothset" && role != "subset") return 2;
    const auto topics = split_topics(argv[2]);
    if (role != "pubset" && role != "bothset" && role != "subset" && topics.size() != 1) return 2;
    const bool is_publisher = role == "pub" || role == "both" || role == "pubset" || role == "bothset";
    const bool is_subscriber = role == "sub" || role == "both" || role == "bothset" || role == "subset";
    std::vector<RouteDescriptor> descriptors;
    for (const auto& topic : topics) {
        RouteDescriptor descriptor; descriptor.topic = topic; descriptor.key.msg_id = 71; descriptor.schema_hash = 0xabcdef01;
        descriptor.key.scope = dzIPC::common::channel_scope_token(descriptor.topic, 0, dzIPC::common::ScopeKind::PubSub);
        descriptors.push_back(std::move(descriptor));
    }
    const auto& descriptor = descriptors.front();
    auto runtime = ClientRuntime::acquire(argv[1]);
    std::vector<std::unique_ptr<PublisherEndpoint>> publishers;
    if (is_publisher) for (const auto& route : descriptors)
        publishers.push_back(std::make_unique<PublisherEndpoint>(runtime, route));
    std::vector<Identity> subscribers;
    std::vector<std::unique_ptr<ipc::mpmc_channel>> receivers;
    if (is_subscriber) {
        const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t i = 0; i < descriptors.size(); ++i) {
            Identity sub{}; std::memcpy(sub.data(), &value, 8); std::memcpy(sub.data() + 8, &i, 4); sub[15] = 2;
            auto registered = runtime->request(LocalKind::RegisterSub, registration_body(sub, descriptors[i]));
            if (registered.header.kind != LocalKind::SubRegistered) throw std::runtime_error("probe 订阅登记失败");
            const auto generation = read32(registered.body.data() + 16);
            auto receiver = std::make_unique<ipc::mpmc_channel>(shm_topic_mpmc_segment_name(descriptors[i].topic, 0).c_str(), ipc::receiver, false);
            dzIPC::control_plane_shm::TopicControlPlane control;
            if (!control.open(shm_topic_mpmc_control_name(descriptors[i].topic, 0)) || control.generation() != generation) throw std::runtime_error("probe SHM generation 不匹配");
            Bytes body(sub.begin(), sub.end()); put32(body, generation);
            if (runtime->request(LocalKind::SubReady, body).header.kind != LocalKind::SubReadyAck) throw std::runtime_error("probe Ready 失败");
            subscribers.push_back(sub); receivers.push_back(std::move(receiver));
        }
    }
    std::cout << "{\"ready\":true}" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line); std::string command; input >> command;
        if (command == "quit") break;
        if (command == "state") {
            std::cout << "{\"remote\":" << (publishers.empty() ? 0 : runtime->route_state(publishers.front()->publisher_id()).remote_ready_count)
                      << ",\"synchronized\":" << (publishers.empty() || runtime->route_state(publishers.front()->publisher_id()).synchronized ? "true" : "false")
                      << ",\"healthy\":" << (runtime->healthy() ? "true" : "false") << ",\"routes\":[";
            for (std::size_t i = 0; i < publishers.size(); ++i) {
                if (i) std::cout << ',';
                const auto hint = runtime->route_state(publishers[i]->publisher_id());
                std::cout << "{\"remote\":" << hint.remote_ready_count << ",\"synchronized\":" << (hint.synchronized ? "true" : "false") << '}';
            }
            std::cout << "]}" << std::endl;
        } else if (command == "diagnostics") std::cout << runtime->diagnostics_json() << std::endl;
        else if (command == "trace") {
            std::cout << "{\"pages\":["; std::uint32_t offset = 0; bool first = true;
            for (;;) {
                const auto page = runtime->metrics().trace_page(offset);
                if (!first) std::cout << ','; first = false; std::cout << page.json;
                if (page.complete) break;
                if (page.next_offset <= offset) throw std::runtime_error("应用 trace 分页未前进");
                offset = page.next_offset;
            }
            std::cout << "]}" << std::endl;
        }
        else if (command == "status") std::cout << runtime->status() << std::endl;
        else if (command == "send" && is_publisher) {
            std::size_t publisher_index = 0, size; std::uint64_t tag, seed; unsigned reliable = 0;
            if (publishers.size() > 1) input >> publisher_index;
            input >> size >> tag >> seed >> reliable;
            if (publisher_index >= publishers.size()) throw std::invalid_argument("发布者下标越界");
            auto bytes = payload(size, tag, seed); const auto result = publishers[publisher_index]->prebuilt(ByteView(bytes), reliable ? Delivery::Reliable : Delivery::BestEffort, reliable ? 5000 : 0);
            std::cout << "{\"success\":" << (result.success ? "true" : "false") << ",\"local\":" << static_cast<int>(result.local)
                      << ",\"network\":" << static_cast<int>(result.network) << ",\"sequence\":" << result.sequence
                      << ",\"publisher_index\":" << publisher_index << ",\"result\":" << static_cast<unsigned>(result.remote.result)
                      << ",\"crc\":" << crc32c(ByteView(bytes)) << "}" << std::endl;
        } else if (command == "load" && is_publisher) {
            unsigned seconds, size, reliable = 0; double rate; input >> seconds >> rate >> size >> reliable;
            if (!seconds || seconds > 60 || rate < 0 || rate > 10000) throw std::invalid_argument("负载参数无效");
            const auto start = std::chrono::steady_clock::now(), end = start + std::chrono::seconds(seconds);
            std::uint64_t count = 0, failed = 0; std::vector<std::uint64_t> latency;
            while (std::chrono::steady_clock::now() < end && count < 600000) {
                if (rate) std::this_thread::sleep_until(start + std::chrono::nanoseconds(static_cast<std::uint64_t>(count * 1000000000.0 / rate)));
                if (std::chrono::steady_clock::now() >= end) break;
                ++count;
                const auto began = std::chrono::steady_clock::now();
                const auto publisher_index = (count - 1) % publishers.size();
                std::uint64_t identity = 0;
                const auto publisher_id = publishers[publisher_index]->publisher_id();
                std::memcpy(&identity, publisher_id.data(), sizeof(identity));
                const auto seed = (identity & 0xffffffff00000000ull) | (count & 0xffffffffull);
                auto bytes = payload(size, 1, seed);
                const auto outcome = publishers[publisher_index]->prebuilt(ByteView(bytes),
                    reliable ? Delivery::Reliable : Delivery::BestEffort, reliable ? 5000 : 0);
                failed += !outcome.success;
                latency.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
            }
            std::sort(latency.begin(), latency.end());
            const auto percentile = [&](double p) { return latency.empty() ? 0ull : static_cast<unsigned long long>(latency[std::min(latency.size()-1, static_cast<std::size_t>(latency.size()*p))]); };
            const auto mean = latency.empty() ? 0.0 : static_cast<double>(std::accumulate(latency.begin(), latency.end(), std::uint64_t{0})) / latency.size();
            std::cout << "{\"sent\":" << count << ",\"failed\":" << failed << ",\"mean_ns\":" << mean << ",\"p50_ns\":" << percentile(.50)
                      << ",\"p95_ns\":" << percentile(.95) << ",\"p99_ns\":" << percentile(.99) << ",\"max_ns\":" << percentile(1);
            if (std::getenv("DZIPC_TEST_LATENCY_SAMPLES")) {
                std::cout << ",\"latency_samples_ns\":[";
                for (std::size_t i = 0; i < latency.size(); ++i) { if (i) std::cout << ','; std::cout << latency[i]; }
                std::cout << ']';
            }
            std::cout << "}" << std::endl;
        } else if ((command == "loadset" || command == "loadsetmix") &&
                   (role == "pubset" || role == "bothset")) {
            unsigned seconds, size, reliable = 0, hot_topics = publishers.size();
            double rate, cold_rate = 0;
            if (command == "loadsetmix") input >> seconds >> rate >> cold_rate >> size >> reliable >> hot_topics;
            else input >> seconds >> rate >> size >> reliable;
            if (!seconds || seconds > 60 || rate < 0 || rate > 10000 || cold_rate < 0 || cold_rate > 10000 ||
                !hot_topics || hot_topics > publishers.size()) throw std::invalid_argument("负载参数无效");
            std::vector<LoadResult> results(publishers.size());
            std::vector<std::thread> workers;
            const auto start = std::chrono::steady_clock::now();
            const bool include_samples = std::getenv("DZIPC_TEST_LATENCY_SAMPLES") != nullptr;
            for (std::size_t i = 0; i < publishers.size(); ++i) {
                workers.emplace_back([&, i] {
                    const auto topic_rate = command == "loadsetmix" && i >= hot_topics ? cold_rate : rate;
                    try { run_load(*publishers[i], seconds, topic_rate, size, reliable != 0, i + 1, start, results[i]); }
                    catch (...) { ++results[i].failed; }
                });
            }
            for (auto& worker : workers) worker.join();
            std::cout << "[";
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (i) std::cout << ',';
                print_load_result(results[i], include_samples);
            }
            std::cout << "]" << std::endl;
        } else if (command == "drainset" && (role == "bothset" || role == "subset")) {
            unsigned seconds; input >> seconds; if (!seconds || seconds > 65) throw std::invalid_argument("接收窗口无效");
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            struct DrainResult { std::uint64_t received = 0, invalid = 0, duplicates = 0; std::set<std::uint64_t> seen; };
            std::vector<DrainResult> results(receivers.size());
            const auto drain_workers = std::min<std::size_t>(4, receivers.size());
            std::vector<std::thread> workers;
            for (std::size_t worker = 0; worker < drain_workers; ++worker) {
                workers.emplace_back([&, worker] {
                    while (std::chrono::steady_clock::now() < end) {
                        bool progressed = false;
                        for (std::size_t i = worker; i < receivers.size(); i += drain_workers) {
                            auto sample = receivers[i]->try_recv(); if (sample.empty()) continue;
                            progressed = true; auto &result = results[i]; ++result.received;
                            if (sample.size() < 48 || sample.size() > kMaxMessageBytes) { ++result.invalid; continue; }
                            std::uint64_t tag = 0, sequence = 0;
                            std::memcpy(&tag, static_cast<const std::uint8_t*>(sample.data()) + 32, 8);
                            std::memcpy(&sequence, static_cast<const std::uint8_t*>(sample.data()) + 40, 8);
                            if (tag != i + 1) ++result.invalid;
                            if (!result.seen.insert(sequence).second) ++result.duplicates;
                            const auto expected = payload(sample.size(), tag, sequence);
                            result.invalid += std::memcmp(expected.data(), sample.data(), sample.size()) != 0;
                        }
                        if (!progressed) std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
            }
            for (auto &worker : workers) worker.join();
            std::cout << "[";
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (i) std::cout << ',';
                std::cout << "{\"topic\":\"" << descriptors[i].topic << "\",\"received\":" << results[i].received
                          << ",\"invalid\":" << results[i].invalid << ",\"duplicates\":" << results[i].duplicates << '}';
            }
            std::cout << "]\n";
        } else if (command == "drain" && !receivers.empty()) {
            unsigned seconds; input >> seconds; if (!seconds || seconds > 65) throw std::invalid_argument("接收窗口无效");
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            std::uint64_t count = 0, bad = 0, duplicates = 0, gap = 0; std::set<std::uint64_t> seen;
            auto last = std::chrono::steady_clock::time_point{};
            while (std::chrono::steady_clock::now() < end) {
                auto sample = receivers.front()->try_recv(); if (sample.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(100)); continue; }
                const auto now = std::chrono::steady_clock::now();
                if (last != std::chrono::steady_clock::time_point{}) gap = std::max(gap, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now-last).count())); last = now;
                ++count; if (sample.size() < 48 || sample.size() > kMaxMessageBytes) { ++bad; continue; }
                std::uint64_t tag, sequence; std::memcpy(&tag, static_cast<const std::uint8_t*>(sample.data())+32, 8); std::memcpy(&sequence, static_cast<const std::uint8_t*>(sample.data())+40, 8);
                if (!seen.insert(sequence).second) ++duplicates;
                const auto expected = payload(sample.size(),tag,sequence); bad += std::memcmp(expected.data(),sample.data(),sample.size()) != 0;
            }
            std::cout << "{\"received\":" << count << ",\"invalid\":" << bad << ",\"duplicates\":" << duplicates << ",\"max_gap_ns\":" << gap << "}" << std::endl;
        } else if (command == "recv" && !receivers.empty()) {
            unsigned count, timeout; input >> count >> timeout;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            std::cout << "{\"received\":["; unsigned received = 0;
            while (received < count && std::chrono::steady_clock::now() < deadline) {
                auto sample = receivers.front()->try_recv(); if (sample.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(100)); continue; }
                std::uint64_t tag = 0, seed = 0; bool valid = sample.size() >= 48 && sample.size() <= kMaxMessageBytes;
                if (valid) { std::memcpy(&tag, static_cast<const std::uint8_t*>(sample.data()) + 32, 8); std::memcpy(&seed, static_cast<const std::uint8_t*>(sample.data()) + 40, 8);
                    const auto expected = payload(sample.size(), tag, seed); valid = !std::memcmp(expected.data(), sample.data(), sample.size()); }
                if (received++) std::cout << ',';
                std::cout << "{\"tag\":" << tag << ",\"seed\":" << seed << ",\"size\":" << sample.size() << ",\"crc\":" << crc32c({sample.data(), sample.size()}) << ",\"valid\":" << (valid ? "true" : "false") << '}';
            }
            std::cout << "]}" << std::endl;
        } else throw std::invalid_argument("probe 命令无效");
    }
    receivers.clear();
    if (runtime->healthy()) for (const auto &sub : subscribers) {
        Bytes body(sub.begin(), sub.end()); body.push_back(2);
        try { runtime->request(LocalKind::Unregister, body); } catch (...) {}
    }
    return 0;
} catch (const std::exception& e) { std::cerr << "probe 失败：" << e.what() << '\n'; return 1; }
