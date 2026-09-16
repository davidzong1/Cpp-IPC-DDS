/// dzIPC Performance Benchmark — simplified, robust version
/// Usage: ./ipc_benchmark [--shm|--socket|--crc-bench|--compare[=N]] [--payload=N]
///                        [--duration=S] [--dzflat]
///
/// --dzflat        SHM 发布改走 DZFlat 平坦布局(docs/dzflat_shm.md), 不再序列化
/// --compare[=N]   同一负载下把 TLV 与 DZFlat 各跑 N 轮(默认 3)并给出比值
///
/// 为什么 --compare 要交替跑而不是"先跑完 A 再跑完 B": 本仓踩过这个坑 —— 顺序跑出来的
/// 档间差被证实是测量伪差(缓存预热、机器负载漂移), 改成交替(ABBA)后两档才一致。所以
/// 这里按轮交替并取中位数, 而不是各跑一次比大小。
///
/// 每次输出都带 dzflat=N fallback=M。这不是凑数: DZFlat 的回退是**静默**的(类型不支持 /
/// 无接收方 / chunk 池耗尽都会回落整包序列化并照常送达), 没有这两个数, "没提速"和"其实
/// 一直在回退"长得一模一样。

#include "dzIPC/dzipc.h"
#include "dzIPC/common/crc32c.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/thread_dispatch.h"
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
#include <sstream>
#if defined(__linux__)
#include <sched.h>
#endif
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

/* ------------------------------------------------------------------ 绑核与优先级
 *
 * 目的是压掉轮间噪声: 不绑核时实测同一档不同轮能差 30%, 而 TLV/DZFlat 的档间差可能只有
 * 5~40% —— 噪声与信号同量级, 比值就成了掷硬币。做法参照 test_dzipc.cpp 的
 * RequestResponseUltraHighPriorityCpu7Timing(SCHED_FIFO + 固定核)。
 *
 * **只钉基准自己的两个测量循环, 且必须在 IPC 对象建好之后钉。** 这两条都是实测踩出来的
 * (第一版把三者都钉上去, 结果 recv=0 —— 链路根本没建起来):
 *
 *   ① 亲和性会被子线程继承。在已经钉到某核的线程里创建订阅者, 库的内部线程会**全部**
 *      继承那个核 —— 于是几个线程挤在一核上;
 *   ② *IPCPtrMake 的 cpu_id 参数作用于**调用线程**, 不是它内部新建的线程。传进去只会
 *      把调用者自己挪走(实测把 main 从 CPU5 拽到了 CPU6)。
 *
 * 两条叠加, 本进程 6 个线程被压进 2 个核, 每核 3 个 SCHED_FIFO。而这里的循环是纯自旋、
 * 从不让出, 同核的其余线程直接饿死 —— 表现就是握手永不完成、一条都收不到。
 *
 * 所以: 库的 4 个内部线程保持默认调度(亲和 0-31, 由内核分配), 只把发布循环与 try_get
 * 循环各钉一核。库线程仍可能迁移, 但它们不再是主要噪声源, 而这样是安全的。
 *
 * **默认只绑核, 不上 SCHED_FIFO。** 也是实测的结果: 加 FIFO 后 p50 的轮间散布确实再降
 * 一点, 但 p99 从 ~130us 炸到 ~27ms。成因是 Linux 的 RT 限流 —— sched_rt_runtime_us
 * 默认 950000/1000000, 即每秒强制给非 RT 任务留 5%(约 50ms); 而这里的循环是纯自旋、
 * 从不主动让出, 于是每秒被掐停一次, 尾延迟就是那一下。p50 干净了而尾部成了垃圾, 不是
 * 好交易。绑核本身(消除跨核迁移与邻居争用)已经把散布从 35~73% 压到 ~2%, FIFO 的边际
 * 收益远不值这个尾巴。要 FIFO 用 --pin-rt, 但那时 p99 不可用。
 */
struct PinPlan {
    bool enable = false;      // 是否要求绑核
    bool want_rt = false;     // 是否额外要求 SCHED_FIFO(默认不要, 见上方注释)
    bool cpu_ok = false;      // 两个核都在亲和掩码里
    bool rt_ok = false;       // SCHED_FIFO 拿到了
    int pub_cpu = -1, sub_cpu = -1;
    int prio = dzIPC::DispatchPriority::ultraHighPriority;

    bool active() const { return enable && cpu_ok; }
    int  eff_prio() const { return (want_rt && rt_ok) ? prio : 0; }   // 0 = 不动优先级
    /* p99 在 FIFO 下会被 RT 限流打成 ~27ms 的尖刺, 不可用。 */
    bool tail_usable() const { return !(want_rt && rt_ok); }
    std::string describe() const {
        if (!enable) return "未绑核(默认调度)";
        if (!cpu_ok) return "绑核请求失败: 目标核不在进程亲和掩码内 —— 退回默认调度";
        std::ostringstream o;
        o << "已绑核 pub=CPU" << pub_cpu << " sub=CPU" << sub_cpu
          << " (库内部 4 线程保持默认调度, 见上方注释)";
        if (!want_rt) {
            o << "  默认调度(未上 SCHED_FIFO: 会因 RT 限流产生 ~27ms 尾刺)";
        } else if (rt_ok) {
            o << "  SCHED_FIFO prio=" << prio << "  [注意] p99 受 RT 限流污染, 不可用";
        } else {
            o << "  [请求了 SCHED_FIFO 但未取得, 仅绑核]";
        }
        return o.str();
    }
};

#if defined(__linux__)
static bool cpu_allowed(int cpu_id) {
    cpu_set_t set; CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return false;
    return cpu_id >= 0 && cpu_id < CPU_SETSIZE && CPU_ISSET(cpu_id, &set);
}
/* 真起一个线程试一次 —— 能否拿到 SCHED_FIFO 取决于 rtprio rlimit / CAP_SYS_NICE,
 * 只看 uid 判断不准(本机非 root 也能拿到)。 */
static bool probe_rt(int cpu, int prio) {
    std::atomic<bool> go{false};
    std::thread w([&]{ while (!go.load(std::memory_order_acquire)) std::this_thread::yield(); });
    auto opt = dzIPC::ThreadDispatch::make_realtime_options(true, cpu, prio);
    const bool ok = dzIPC::ThreadDispatch::apply_thread_options(&w, opt, false, "rt_probe");
    go.store(true, std::memory_order_release); w.join();
    return ok;
}
#else
static bool cpu_allowed(int) { return false; }
static bool probe_rt(int, int) { return false; }
#endif

static PinPlan resolve_pin(bool want, int base, bool want_rt) {
    PinPlan p;
    p.enable = want;
    p.want_rt = want_rt;
    if (!want) return p;
    p.pub_cpu = base; p.sub_cpu = base + 1;
    p.cpu_ok = cpu_allowed(p.pub_cpu) && cpu_allowed(p.sub_cpu);
    if (p.cpu_ok && want_rt) p.rt_ok = probe_rt(p.sub_cpu, p.prio);
    return p;
}
static uint64_t now_ns() { return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(); }

struct Result {
    std::string transport;
    std::size_t payload_bytes = 0;
    double duration_sec = 0;
    uint64_t sent = 0, recv = 0;
    std::vector<uint64_t> lat_ns;
    /* DZFlat 实际生效了多少条。回退是静默的, 所以这两个数是解读结果的前提。 */
    uint64_t dzflat = 0, fallback = 0;
    /* 接收侧的真实缺陷计数(结构不符 / 段头坏 / TLV 越界)。非 0 表示这轮的数不干净。 */
    uint64_t rx_defects = 0;
    /* 0 = 开环打满; >0 = 按此速率限速。限速档的延迟才反映单条成本。 */
    double target_rate = 0;
    /* 限速是否真的达成。没达成说明目标速率超过了本档上限, 延迟又变回队列主导。 */
    bool paced_ok() const {
        return target_rate <= 0 || msg_per_sec() >= 0.9 * target_rate;
    }
    double msg_per_sec() const { return duration_sec > 0 ? recv / duration_sec : 0; }
    double mbps() const { return msg_per_sec() * payload_bytes * 8.0 / 1e6; }
    double p(double q) const {
        if (lat_ns.empty()) return 0;
        auto v = lat_ns; std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(q * v.size()))] / 1000.0;
    }
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
              << "  wire: dzflat=" << dzflat << " fallback=" << fallback
              << "  rx_defects=" << rx_defects << "\n"
              << "//JSON{\"transport\":\"" << transport << "\",\"payload\":" << payload_bytes
              << ",\"msg_per_sec\":" << msg_per_sec() << ",\"mbps\":" << mbps()
              << ",\"lat_us_p50\":" << us(p(0.5)) << ",\"lat_us_p99\":" << us(p(0.99))
              << ",\"dzflat\":" << dzflat << ",\"fallback\":" << fallback
              << ",\"rx_defects\":" << rx_defects << "}\n";
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
/* target_rate > 0 时按该速率**限速**发布(闭环); 0 = 全速打满(开环)。
 *
 * 为什么必须有这两档: 开环打满时发布端总是跑在能力上限, 队列积深, 端到端延迟测到的是
 * **队列深度**而不是每条消息的 wire 成本 —— 吞吐更高的一档延迟反而更差(Little 定律),
 * 于是"哪种 wire 更快"这个问题在开环数据里根本回答不了。
 *   开环 → 回答"吞吐上限是多少"
 *   闭环 → 回答"单条消息快多少"
 * 两问都要答, 所以两档都要跑。 */
static Result bench_shm(std::string topic, int domain, std::size_t payload_bytes, double dur_sec,
                        bool use_dzflat = false, double target_rate = 0,
                        const PinPlan& pin = PinPlan{}) {
    std::system(("rm -f /dev/shm/*" + topic + "* /dev/shm/__IPC_SHM__*" + topic + "* 2>/dev/null").c_str());
    Result r; r.transport = use_dzflat ? "shm+dzflat" : "shm"; r.payload_bytes = payload_bytes;
    r.target_rate = target_rate;

    /* 开关是进程级的, 用完必须还原 —— --compare 会在同一进程里反复切换两档。 */
    const bool prev_dzflat = dzIPC::IsDzFlatEnabled();
    dzIPC::EnableDzFlat(use_dzflat);
    struct Restore {
        bool v; ~Restore() { dzIPC::EnableDzFlat(v); }
    } restore{prev_dzflat};

    std::atomic<bool> sub_ready{false}, run{true};
    std::atomic<uint32_t> start_seq{UINT32_MAX};  // first measurement seq
    std::mutex mtx; std::vector<uint64_t> lats;

    auto tpl = make_payload(payload_bytes);

    // subscriber
    std::thread st([&]{
        /* 顺序要紧: 先建对象+InitChannel(让库线程按默认亲和分布), **之后**才钉本线程。
         * 反过来会让库的内部线程继承本线程的核, 挤在一起互相饿死(见上方注释)。 */
        auto sub = dzIPC::SubscriberIPCPtrMake(tpl, topic, domain, 256, dzIPC::IPC_SHM, false);
        sub->InitChannel("bench");
        if (pin.active()) {
            dzIPC::ThreadDispatch::apply_current_thread_options(
                dzIPC::ThreadDispatch::make_realtime_options(true, pin.sub_cpu, pin.eff_prio()),
                false, "bench_sub");
        }
        sub_ready.store(true);
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
        while (run.load(std::memory_order_acquire)) {
            uint32_t seq = 0;
            uint64_t sts = 0;
            bool got = false;
            if (sub->try_get_clone(rcv)) {
                auto* d = static_cast<dzIPC::Msg::TestMsg*>(rcv->topic().get());
                if (d->data2.size() >= 4) {
                    seq = static_cast<uint32_t>(d->data2[0]);
                    sts = (uint64_t(static_cast<uint32_t>(d->data2[1])) << 32)
                          | uint64_t(static_cast<uint32_t>(d->data2[2]));
                    got = true;
                }
            } else {
                /* DZFlat 段在视图队列 —— 借样零拷贝, 经 TestMsgFlat 读 data2。 */
                dzIPC::Sample sample;
                if (sub->try_get(sample)) {
                    auto v = sample.view<dzIPC::Msg::TestMsgFlat>();
                    if (v.valid()) {
                        auto d2 = v.data2();   /* span<const int32_t> */
                        if (d2.size() >= 4) {
                            seq = static_cast<uint32_t>(d2[0]);
                            sts = (uint64_t(static_cast<uint32_t>(d2[1])) << 32)
                                  | uint64_t(static_cast<uint32_t>(d2[2]));
                            got = true;
                        }
                    }
                }
            }
            if (!got) continue;
            if (seq < start_seq.load(std::memory_order_acquire)) continue;  // filter warmup
            if (sts > 0) {
                std::lock_guard<std::mutex> lk(mtx);
                lats.push_back(now_ns() - sts);
            }
        }
    });

    while (!sub_ready.load()) std::this_thread::sleep_for(milliseconds(5));

    auto pub = dzIPC::PublisherIPCPtrMake(tpl, topic, domain, dzIPC::IPC_SHM, false);
    pub->InitChannel("bench");
    { auto dl = steady_clock::now() + seconds(5);
      while (!pub->has_subscribed() && steady_clock::now() < dl) std::this_thread::sleep_for(milliseconds(10)); }

    /* 握手完成后才钉发布循环(它跑在本调用线程上)。同样是为了不让库线程继承本核。 */
    if (pin.active()) {
        dzIPC::ThreadDispatch::apply_current_thread_options(
            dzIPC::ThreadDispatch::make_realtime_options(true, pin.pub_cpu, pin.eff_prio()),
            false, "bench_pub");
    }

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
    /* 在 warmup 之后才清计数 —— 否则 warmup 的几千条会把这一轮的 dzflat/fallback 比例冲淡。 */
    dzIPC::ResetDzFlatCounters();
    dzIPC::ResetDzFlatRxCounters();
    uint32_t mseq = ++wseq;
    start_seq.store(mseq, std::memory_order_release);
    auto t0 = steady_clock::now();
    auto deadline = t0 + duration<double>(dur_sec);
    const bool paced = target_rate > 0;
    const auto interval = duration_cast<nanoseconds>(duration<double>(paced ? 1.0 / target_rate : 0));
    auto next = t0;
    while (steady_clock::now() < deadline) {
        if (paced) {
            next += interval;
            /* 粗等 + 细旋: sleep_until 的粒度(几十 us)在这些间隔上够不着, 纯 spin 又白烧
             * 一个核。留 80us 余量交给 spin。 */
            const auto coarse = next - microseconds(80);
            if (steady_clock::now() < coarse) std::this_thread::sleep_until(coarse);
            while (steady_clock::now() < next) { /* spin */ }
            if (next > deadline) break;
        }
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

    r.dzflat = dzIPC::DzFlatPublishCount();
    r.fallback = dzIPC::DzFlatFallbackCount();
    r.rx_defects = dzIPC::DzFlatRxCounters().defects();
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
            if (sub->try_get_clone(rcv)) {
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

// --- TLV vs DZFlat 对比 ---
//
// 交替(ABBA)而非顺序: 顺序跑的话, 第一档承担全部冷启动成本(页错误、chunk 池首次分配、
// CPU 频率爬升), 差值里混进的是顺序而不是 wire。本仓有过实测教训 —— 顺序跑出来的档间差
// 后来被证实是测量伪差, 改成交替才两档一致。
//
// 取中位数而非均值: 单轮里任何一次调度抖动都会拖长尾, 而这里关心的是典型值。
static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* 一档的多轮汇总。 */
struct Arm {
    std::vector<double> rate, p50, p99;
    uint64_t dzflat = 0, fallback = 0, recv = 0;
    bool paced_ok = true;
    void add(const Result& r) {
        rate.push_back(r.msg_per_sec()); p50.push_back(r.p(0.5)); p99.push_back(r.p(0.99));
        dzflat += r.dzflat; fallback += r.fallback; recv += r.recv;
        if (!r.paced_ok()) paced_ok = false;
    }
};

/* 跑一个 regime(开环或限速)的 rounds 轮交替, 打印明细并返回两档汇总。 */
static void run_regime(const char* title, int domain, std::size_t payload, double dur,
                       int rounds, double rate, Arm& tlv, Arm& dz, uint64_t& defects,
                       int& run_id, const PinPlan& pin) {
    std::cout << "\n  --- " << title << " ---\n";
    for (int i = 0; i < rounds; ++i) {
        const bool tlv_first = (i % 2 == 0);
        for (int k = 0; k < 2; ++k) {
            const bool use_dz = tlv_first ? (k == 1) : (k == 0);
            std::string topic = "bench_cmp_" + std::to_string(run_id++);
            Result r = bench_shm(topic, domain, payload, dur, use_dz, rate, pin);
            (use_dz ? dz : tlv).add(r);
            defects += r.rx_defects;
            std::cout << "    轮" << (i + 1) << " " << std::setw(8) << std::left
                      << (use_dz ? "DZFlat" : "TLV") << std::right
                      << "  rate=" << std::setw(8) << std::fixed << std::setprecision(0)
                      << r.msg_per_sec() << "/s"
                      << "  p50=" << std::setw(7) << std::setprecision(1) << r.p(0.5) << "us"
                      << "  recv=" << std::setw(7) << r.recv
                      << "  dzflat=" << r.dzflat << " fb=" << r.fallback
                      << (r.paced_ok() ? "" : "  [限速未达成]") << "\n";
        }
    }
}

static double vmin(const std::vector<double>& v) {
    return v.empty() ? 0 : *std::min_element(v.begin(), v.end());
}
static double vmax(const std::vector<double>& v) {
    return v.empty() ? 0 : *std::max_element(v.begin(), v.end());
}
/* 两档的轮间区间是否重叠。重叠 ⇒ 差异没有超过轮间噪声, 中位数的比值不可当结论。 */
static bool overlaps(const std::vector<double>& a, const std::vector<double>& b) {
    return vmin(a) <= vmax(b) && vmin(b) <= vmax(a);
}

/* 每个量都打 中位数[min,max]。
 *
 * 只打中位数会被过度解读: 实测同一档不同轮之间能差 30%, 而档间差可能只有 5% —— 那种
 * 情况下比值是噪声, 不是结论。带上区间, 读者一眼能看出这一点; 区间重叠时下面还会明说。 */
static void print_table(const Arm& tlv, const Arm& dz, bool latency_meaningful,
                        bool tail_usable = true) {
    /* 每档只有一个**结果量**, 另一个是控制量, 不参与"差异是否超过噪声"的判定:
     *   开环 → rate 是结果, 延迟是队列深度的副产品;
     *   限速 → 延迟是结果, rate 被钉在目标值上(两档必然重叠, 标它没有意义)。 */
    const bool rate_is_outcome = !latency_meaningful;
    auto faster = [](double base, double now) { return now > 0 ? base / now : 0.0; };
    auto cell = [](const std::vector<double>& v, int prec) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(prec) << median(v)
          << "[" << vmin(v) << "," << vmax(v) << "]";
        return o.str();
    };
    const double tr = median(tlv.rate), dr = median(dz.rate);
    std::cout << "    " << std::left << std::setw(13) << "量" << std::setw(26) << "TLV"
              << std::setw(26) << "DZFlat" << "比值\n" << std::right;
    std::cout << "    " << std::left << std::setw(13) << "rate(msg/s)" << std::setw(26)
              << cell(tlv.rate, 0) << std::setw(26) << cell(dz.rate, 0)
              << std::fixed << std::setprecision(2) << (tr > 0 ? dr / tr : 0) << "x"
              << (rate_is_outcome && overlaps(tlv.rate, dz.rate) ? "  <- 区间重叠" : "")
              << "\n";
    const char* tag = latency_meaningful ? "x 更快" : "x (队列主导, 不可解读)";
    std::cout << "    " << std::left << std::setw(13) << "p50(us)" << std::setw(26)
              << cell(tlv.p50, 1) << std::setw(26) << cell(dz.p50, 1)
              << std::setprecision(2) << faster(median(tlv.p50), median(dz.p50)) << tag
              << (latency_meaningful && overlaps(tlv.p50, dz.p50) ? "  <- 区间重叠" : "") << "\n";
    std::cout << "    " << std::left << std::setw(13) << "p99(us)" << std::setw(26)
              << cell(tlv.p99, 1) << std::setw(26) << cell(dz.p99, 1)
              << std::setprecision(2) << faster(median(tlv.p99), median(dz.p99))
              << (tail_usable ? tag : "x (RT 限流污染, 不可用)")
              << (tail_usable && latency_meaningful && overlaps(tlv.p99, dz.p99) ? "  <- 区间重叠" : "")
              << "\n" << std::right;
    if ((rate_is_outcome && overlaps(tlv.rate, dz.rate))
        || (latency_meaningful && overlaps(tlv.p50, dz.p50))) {
        std::cout << "    [注意] 标了「区间重叠」的量: 两档的轮间区间互相覆盖, 差异没有超过\n"
                     "           轮间噪声。加大 --compare=N 或 --duration= 再看, 不要拿这个\n"
                     "           比值下结论。\n";
    }
}

/* DZFlat 档的可信度检查。没有这一段, 上面的比值是不可解读的。 */
static void audit_arm(const Arm& dz, uint64_t defects) {
    const uint64_t total = dz.dzflat + dz.fallback;
    std::cout << "    DZFlat 实际生效 " << dz.dzflat << " 条, 回退 " << dz.fallback << " 条";
    if (total > 0) {
        std::cout << " (回退率 " << std::fixed << std::setprecision(1)
                  << 100.0 * dz.fallback / total << "%)";
    }
    std::cout << "\n";
    if (dz.dzflat == 0) {
        std::cout << "    [警告] DZFlat 一条都没生效 —— 比值反映的不是 wire 差异。\n"
                     "           查: 类型是否由 generator 生成(dzflat_supported)、发布时\n"
                     "           是否已有接收方、chunk 池是否耗尽。\n";
    } else if (dz.fallback > dz.dzflat / 10) {
        std::cout << "    [注意] 回退占比偏高 —— DZFlat 档掺了 TLV, 提速被低估。常见成因是\n"
                     "           消费端跟不上, 把 chunk 池(每尺寸档 32 块)占满。\n";
    }
    if (defects > 0) {
        std::cout << "    [警告] 接收侧缺陷计数 " << defects << " —— 有消息被拒收, 数不干净。\n";
    }
}

static void compare_bench(int domain, std::size_t payload, double dur, int rounds, double rate,
                          const PinPlan& pin) {
    std::cout << "\n=== TLV vs DZFlat | " << payload << "B | " << dur << "s/轮 | "
              << rounds << " 轮交替(ABBA) ===\n";
    /* 把调度环境打进表头: 绑核与否会成倍改变轮间散布, 数字必须自证是哪种环境下测的。 */
    std::cout << "  调度: " << pin.describe() << "\n";
    if (pin.enable && !pin.cpu_ok) {
        std::cout << "  [警告] 轮间噪声会明显偏大, 区间重叠的量更不可解读。\n";
    }

    uint64_t defects = 0;
    int run_id = 0;

    /* regime 1: 开环打满 —— 回答"吞吐上限" */
    Arm t_sat, d_sat;
    run_regime("开环打满(测吞吐上限; 此档延迟是队列深度, 不代表单条成本)",
               domain, payload, dur, rounds, 0, t_sat, d_sat, defects, run_id, pin);
    print_table(t_sat, d_sat, /*latency_meaningful=*/false, pin.tail_usable());
    audit_arm(d_sat, 0);

    /* regime 2: 限速 —— 回答"单条快多少"。队列不积, 延迟才是 wire 成本 */
    std::ostringstream ttl;
    ttl << "限速 " << std::fixed << std::setprecision(0) << rate
        << " msg/s(测单条延迟; 队列不积压)";
    Arm t_pac, d_pac;
    run_regime(ttl.str().c_str(), domain, payload, dur, rounds, rate,
               t_pac, d_pac, defects, run_id, pin);
    const bool ok = t_pac.paced_ok && d_pac.paced_ok;
    print_table(t_pac, d_pac, ok, pin.tail_usable());
    audit_arm(d_pac, defects);
    if (!ok) {
        std::cout << "    [警告] 限速未达成 —— 目标 " << std::setprecision(0) << rate
                  << " msg/s 超过了本负载的能力上限, 于是这一档又退化成饱和态,\n"
                     "           延迟仍是队列主导。用 --rate= 调低后重跑。\n";
    }

    std::cout << "\n  怎么读: 上一档看 rate(吞吐能力), 下一档看 p50/p99(单条成本)。\n"
                 "  把两档的延迟放在一起比是错的 —— 开环下吞吐越高队列越深, 延迟必然更差。\n"
                 "\n"
                 "  射程: 本基准的消息是 TestMsg = {float64[], int32[], string[], bool},\n"
                 "  **没有嵌套消息数组**。而 DZFlat 三项成本里最大的一项(每元素一次堆分配,\n"
                 "  docs/dzflat_shm.md 1.3)只在嵌套数组上出现 —— 那一项在 10 万点云上是 40x\n"
                 "  量级。这里测到的只是另外两项(页尾交错 + 多余整包搬运), 所以数值偏小是\n"
                 "  **预期的**, 不代表 DZFlat 只值这么多。嵌套数组的对比见 dzflat_tx_benchmark\n"
                 "  (StdPointCloud)。\n";
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
    bool dzflat = false; int cmp_rounds = 0; double rate = 0; double cmp_rate = 20000;
    /* --compare 默认开绑核: 它唯一的用途就是比两档, 而不绑核时噪声与信号同量级。
     * 单跑一档时默认不开, 免得悄悄改变别人既有的测量条件。 */
    int pin_base = 5; int pin_want = -1; bool pin_rt = false;   // -1 = 未指定, 由模式决定
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shm") mode = "shm";
        else if (a == "--socket") mode = "socket";
        else if (a == "--crc-bench") { crc_bench(); return 0; }
        else if (a == "--dzflat") dzflat = true;
        else if (a == "--pin") pin_want = 1;
        else if (a == "--no-pin") pin_want = 0;
        else if (a.rfind("--pin=",0)==0) { pin_want = 1; pin_base = std::stoi(a.substr(6)); }
        else if (a == "--pin-rt") { pin_want = 1; pin_rt = true; }
        else if (a == "--compare") cmp_rounds = 3;
        else if (a.rfind("--compare=",0)==0) cmp_rounds = std::max(1, std::stoi(a.substr(10)));
        else if (a.rfind("--rate=",0)==0) { rate = std::stod(a.substr(7)); cmp_rate = rate; }
        else if (a.rfind("--payload=",0)==0) payload = std::stoul(a.substr(10));
        else if (a.rfind("--duration=",0)==0) dur = std::stod(a.substr(11));
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: ipc_benchmark [--shm|--socket|--crc-bench|--compare[=N]]\n"
                         "                     [--payload=N] [--duration=S] [--dzflat]\n"
                         "\n"
                         "  --dzflat       SHM 发布走 DZFlat 平坦布局(默认走 TLV 序列化)\n"
                         "  --compare[=N]  同一负载下 TLV 与 DZFlat 各跑 N 轮(默认 3)并给出比值。\n"
                         "                 交替(ABBA)执行: 顺序跑会把冷启动成本全压在第一档。\n"
                         "                 分两档: 开环测吞吐上限, 限速测单条延迟\n"
                         "  --rate=N       限速到 N msg/s(闭环)。0/不给 = 全速打满。\n"
                         "                 --compare 的限速档默认 20000, 用本参数覆盖\n"
                         "  --pin[=BASE]   把发布循环与 try_get 循环分别钉到 BASE/BASE+1 两核(默认\n"
                         "                 BASE=5), 实测把轮间散布从 35~73%% 压到 ~2%%。库的内部线程\n"
                         "                 保持默认调度 —— 钉它们会因亲和性继承把多个自旋线程挤到\n"
                         "                 一核互相饿死(实测 recv=0)。--compare 默认开, --no-pin 关\n"
                         "  --pin-rt       在 --pin 之上再上 SCHED_FIFO prio60。p50 略稳, 但 p99 会被\n"
                         "                 Linux RT 限流打成 ~27ms 尖刺而不可用 —— 只在只看 p50 时用\n"
                         "  socket 档不支持 DZFlat —— 它是 SHM 专属(UDP 分片格式由 data_rev 消费)\n";
            return 0;
        }
    }

    if (cmp_rounds > 0) {
        if (mode != "shm") {
            std::cout << "[ERROR] --compare 只适用于 SHM: DZFlat 是 SHM 专属的 wire\n";
            return 2;
        }
        compare_bench(77, payload, dur, cmp_rounds, cmp_rate,
                      resolve_pin(pin_want != 0, pin_base, pin_rt));
        return 0;
    }

    if (dzflat && mode != "shm") {
        std::cout << "[ERROR] --dzflat 只适用于 SHM: DZFlat 是 SHM 专属的 wire "
                     "(见 docs/dzflat_shm.md §1.4)\n";
        return 2;
    }

    const PinPlan pin = resolve_pin(pin_want == 1, pin_base, pin_rt);
    std::cout << "=== dzIPC Bench | " << mode << (dzflat ? "+dzflat" : "") << " | "
              << payload << "B | " << dur << "s ===\n";
    if (mode == "shm") std::cout << "  调度: " << pin.describe() << "\n";
    Result r = (mode == "shm") ? bench_shm("bench_01", 77, payload, dur, dzflat, rate, pin)
                               : bench_socket("bench_01", 77, payload, dur);
    r.report();
    return 0;
}
