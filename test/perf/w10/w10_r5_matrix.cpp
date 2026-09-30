/* W10-R5 —— §10.2 扫描计数 + 三态空闲**正式复验矩阵**工装（t38）。
 *
 * 为什么需要它（方案 §6.2/§6.3）：
 *   t30 只做了"接线存在性 + 一次 24 组扫描"的初测；**正式复验**要求：
 *     ① 每个配置跑在**新的独立进程**里（池的 worker 数是"首个成功 start() 的调用方"
 *        一次性决定的 ⇒ 同进程跑多档会串档）；
 *     ② 1/100/500/1000 route × 三态空闲 × 诊断开/关，空闲窗口 ≥60 s；
 *     ③ 五项派生值**从同一窗口的差值**算，零分母写 `null` 且给出原因（⛔ 不写 0）；
 *     ④ 运行时门禁逐条机械断言（⛔ "rg 找到调用点"不算完成）；
 *     ⑤ 三态均记录「应用对象 / 控制项 / worker route 数」；
 *     ⑥ 状态 3 必须先让**每个 route 收到确认消息**，再静默超过 keep-alive，
 *        恢复时**不重新注册**，并记录**全部**恢复首包；
 *     ⑦ CPU 用**独立采样**（本进程给 /proc/self/stat 参考值；逐 TID 由外部采样器做，
 *        观测器开销另行对照）——⛔ `scan_time_ns_total` 是墙钟区间累计，**不得**换算成 core。
 *
 * 用法：
 *   w10_r5_matrix --n 100 --workers 32 --diag off --state 3 --window-s 60 \
 *                 --domain 8100 --out <dir> --run-id <id> [--round 1] [--load-msgs 5]
 *
 * 产物（方案 §8.1）：
 *   manifest.json   指纹/配置/判据版本/诊断开关/期限
 *   phases.csv      阶段化耗时与 note
 *   windows.csv     逐窗口：raw 计数 + 五项派生值（null 语义）+ 门禁结果
 *   counters.json   同一窗口的完整计数快照（resident + gated）
 *   threestate.json 三态必录字段（应用对象/控制项/worker route/线程退出/恢复首包）
 *   verdict.md      独立结论 + 门禁逐条
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/control_plane.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;
/* 确认帧/恢复帧的 seq 区间：与规模帧分账（本工装只用 0.. 的规模帧，故确认帧从大数起） */
static constexpr std::uint32_t kConfirmSeqBase = 1000000000u;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
    return def;
}
static const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}
static bool arg_flag(int argc, char** argv, const char* key)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

/* ---------------- 库指纹（t38：把 R0-A1 的"成对"要求做成运行时自证） ----------------
 * 为什么必须在工装里读：`RecvWorkerStats` 是**按值返回**的结构体，t30 在其尾部追加过字段。
 * 若库在工装编译后被重建（内容变了），工装读到的字段偏移可能整体错位 —— 症状是
 * "某个计数增量恒 0 而相邻计数正常"（t38 实测到 1/60 窗口出现 `Δscan_rounds=0` 而
 * `Δscanned_routes_total=660000` 这种**内部不自洽**读数）。因此把"实际加载库的哈希"
 * 在**开跑前/收尾后各记一次**，并在不一致时直接判失败。 */
static std::string loaded_lib_path()
{
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.find("libipc.so") == std::string::npos) continue;
        const auto sp = line.find('/');
        if (sp == std::string::npos) continue;
        const auto end = line.find_first_of(" \n", sp);
        return line.substr(sp, end == std::string::npos ? std::string::npos : end - sp);
    }
    return std::string();
}
static std::string sha256_of_file(const std::string& path_in)
{
    std::string path = path_in;
    if (path == "/proc/self/exe")
    {
        char b[4096] = {0};
        const ssize_t n = ::readlink("/proc/self/exe", b, sizeof b - 1);
        if (n <= 0) return std::string();
        path.assign(b, static_cast<std::size_t>(n));
    }
    std::string cmd = "sha256sum \"" + path + "\" 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return std::string();
    char buf[256] = {0};
    const std::size_t n = ::fread(buf, 1, sizeof buf - 1, p);
    ::pclose(p);
    if (n == 0) return std::string();
    std::string out(buf, n);
    const auto sp = out.find(' ');
    return sp == std::string::npos ? std::string() : out.substr(0, sp);
}

/* ---------------- 采样：CPU / 线程 / ctx ---------------- */
static double proc_cpu_ticks()
{
    std::ifstream f("/proc/self/stat");
    if (!f) return -1.0;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto rp = s.rfind(')');
    if (rp == std::string::npos) return -1.0;
    std::istringstream is(s.substr(rp + 2));
    std::vector<std::string> v;
    std::string t;
    while (is >> t) v.push_back(t);
    /* 从 after-) 起：field 13 = utime, field 14 = stime ⇒ 索引 11 / 12 */
    return std::strtod(v[11].c_str(), nullptr) + std::strtod(v[12].c_str(), nullptr);
}

struct Ctx
{
    unsigned long long vol{0}, nonvol{0};
    int unreadable{0};
};
static Ctx ctxsum()
{
    Ctx c;
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return c;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        char p[256];
        std::snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        std::ifstream f(p);
        if (!f) { ++c.unreadable; continue; }
        std::string line;
        bool got = false;
        while (std::getline(f, line))
        {
            if (line.rfind("voluntary_ctxt_switches:", 0) == 0)
            {
                c.vol += std::strtoull(line.c_str() + 25, nullptr, 10);
                got = true;
            }
            else if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
            {
                c.nonvol += std::strtoull(line.c_str() + 28, nullptr, 10);
            }
        }
        if (!got) ++c.unreadable;
    }
    ::closedir(d);
    return c;
}
static size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return 0;
    size_t n = 0;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}

/* ---------------- 计数读取：常驻面 与 门控面 ---------------- */
struct Resident
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_ready_rounds{0};
    std::uint64_t deferred_depth_last{0}, deferred_depth_max{0};
    std::uint64_t wait_timeouts{0}, wait_wakeups{0}, budget_yields{0}, deferred_drains{0};
    std::uint64_t recv_once_calls{0}, recv_errors{0}, idle_exits{0}, thread_restarts{0};
};
struct Gated
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_time_ns_total{0};
    std::uint64_t wait_timeout_count{0}, ready_observed{0};
    std::uint64_t deferred_depth_last{0}, deferred_depth_max{0};
};
static Resident snap_resident()
{
    Resident r;
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const auto st = pool.stats();
    r.scan_rounds = st.scan_rounds;
    r.scanned_routes_total = st.scanned_routes_total;
    r.scan_ready_rounds = st.scan_ready_rounds;
    r.deferred_depth_last = st.deferred_depth_last;
    r.deferred_depth_max = st.deferred_depth_max;
    r.wait_timeouts = st.wait_timeouts;
    r.wait_wakeups = st.wait_wakeups;
    r.budget_yields = st.budget_yields;
    r.deferred_drains = st.deferred_drains;
    r.recv_once_calls = st.recv_once_calls;
    r.recv_errors = st.recv_errors;
    r.idle_exits = st.idle_exits;
    r.thread_restarts = st.thread_restarts;
    return r;
}
static Gated snap_gated()
{
    Gated g;
    const auto s = dzIPC::measure::CounterRegistry::instance().snapshot();
    const auto v = [&](dzIPC::measure::CounterId id) { return static_cast<std::uint64_t>(s.get(id)); };
    g.scan_rounds = v(dzIPC::measure::CounterId::scan_rounds);
    g.scanned_routes_total = v(dzIPC::measure::CounterId::scanned_routes_total);
    g.scan_time_ns_total = v(dzIPC::measure::CounterId::scan_time_ns_total);
    g.wait_timeout_count = v(dzIPC::measure::CounterId::wait_timeout_count);
    g.ready_observed = v(dzIPC::measure::CounterId::ready_observed);
    g.deferred_depth_last = v(dzIPC::measure::CounterId::deferred_depth_last);
    g.deferred_depth_max = v(dzIPC::measure::CounterId::deferred_depth_max);
    return g;
}

/* 派生值：分母为 0 ⇒ null + 原因（⛔ 不写 0）。 */
struct Ratio
{
    bool valid{false};
    double value{0.0};
    std::string reason;
};
static Ratio ratio(std::uint64_t num, std::uint64_t den, const char* den_name, const char* uncollected_reason)
{
    Ratio r;
    if (den == 0)
    {
        r.valid = false;
        r.reason = std::string("分母 ") + den_name + " = 0";
        if (uncollected_reason && *uncollected_reason) r.reason += std::string("（") + uncollected_reason + "）";
        return r;
    }
    r.valid = true;
    r.value = static_cast<double>(num) / static_cast<double>(den);
    return r;
}
static std::string fmt(const Ratio& r)
{
    if (!r.valid) return "null";
    char b[64];
    std::snprintf(b, sizeof b, "%.6f", r.value);
    return b;
}
static std::string js(const Ratio& r)
{
    if (!r.valid)
    {
        std::string s = "null";
        return s;
    }
    char b[64];
    std::snprintf(b, sizeof b, "%.6f", r.value);
    return b;
}

struct WindowRow
{
    int idx{0};
    int state{0};
    std::string phase;          /* idle / load */
    double wall_s{0};
    size_t threads{0}, peak_threads{0};
    double cpu_cores{-1}, ctx_per_s{0};
    int ctx_unreadable{0};
    long routes{0}, control_entries{0};
    long d_idle_exits{0}, d_thread_restarts{0};   /* 三态必录：窗口内线程退出数与按需拉起数 */
    std::uint64_t d_tick{0};                      /* 窗口内控制面 tick 数 */
    double tick_per_s{0.0};                       /* 唤醒率（控制面成本的自变量） */
    double tick_max_us{0.0};
    double tick_last_us{0.0};
    std::uint64_t tick_overruns{0};
    Resident r0, r1;
    Gated g0, g1;
    /* 派生值（同一窗口差值） */
    Ratio scan_per_round, ns_per_round, ns_per_entry, ready_ratio, timeouts_per_s, resident_timeouts_per_s;
    /* 门禁 */
    std::vector<std::pair<std::string, bool>> gates;
    std::string notes;
    /* 同窗口自洽 */
    bool resident_gated_consistent{false};
};

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long n = arg_long(argc, argv, "--n", 100);
    const long workers = arg_long(argc, argv, "--workers", 32);
    const int state = static_cast<int>(arg_long(argc, argv, "--state", 3));
    const long dom = arg_long(argc, argv, "--domain", 8100);
    const long window_ms = arg_long(argc, argv, "--window-s", 60) * 1000;
    const long load_msgs = arg_long(argc, argv, "--load-msgs", 5);
    const std::string diag_s = arg_str(argc, argv, "--diag", "off");
    const bool diag = (diag_s == "on");
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "r5");
    const long round = arg_long(argc, argv, "--round", 1);
    const bool cpu_only = arg_flag(argc, argv, "--cpu-only");
    const bool no_confirm = arg_flag(argc, argv, "--no-confirm");
    const bool skip_recover = arg_flag(argc, argv, "--skip-recover");
    /* 窗口前静置秒数：让"按需归还的空闲线程"先退完，把**控制面 CPU** 与
     * "池线程退出中"的瞬态分开（t32/R6 需要干净的控制面读数）。 */
    const long settle_s = arg_long(argc, argv, "--settle-s", 2);

    /* 判据版本：与方案 §6.2/§6.3 对应；改动判据必须升版本（append-only 纪律）。 */
    const std::string criteria_version = "W10-R5/§6.2-§6.3/v1";
    const long kWaitTimeoutMs = 100;      /* 冻结预算：RecvBudget::wait_timeout */
    const long kIdleKeepAliveMs = 1000;   /* 冻结预算：idle_keep_alive */
    const long kWaitSetCap = 127;         /* recv_wait_set kMaxRoutes（Linux） */

    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); };

    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::string> phases;      /* "phase,ms,note" */
    std::vector<std::string> gates_fail;
    auto fail = [&](const std::string& s) { gates_fail.push_back(s); };
    auto phase = [&](const char* name, long long ms, const std::string& note) {
        char b[512];
        std::snprintf(b, sizeof b, "%s,%lld,\"%s\"", name, ms, note.c_str());
        phases.push_back(b);
    };

    const std::string lib_path_before = loaded_lib_path();
    const std::string lib_sha_before = lib_path_before.empty() ? std::string() : sha256_of_file(lib_path_before);
    const std::string tool_sha = sha256_of_file("/proc/self/exe");
    std::printf("fingerprint lib_path=%s lib_sha=%s tool_sha=%s\n", lib_path_before.c_str(), lib_sha_before.c_str(),
                tool_sha.c_str());
    {
        const std::string want_lib = arg_str(argc, argv, "--require-lib-sha256", "");
        if (!want_lib.empty() && lib_sha_before != want_lib)
        {
            std::printf("FINGERPRINT_MISMATCH lib: actual=%s expected=%s\n", lib_sha_before.c_str(), want_lib.c_str());
            std::printf("W10_R5_DONE verdict=FAIL failures=1\n");
            return 1;
        }
    }
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(diag);

    /* ---------------- 阶段 0：池首启（必须最先，且本进程只跑一档） ---------------- */
    auto t0 = Clock::now();
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const bool started = pool.start(static_cast<std::size_t>(workers), dzIPC::threepools::RecvBudget{});
    const std::size_t eff_workers = pool.worker_count();
    std::printf("pool_start started=%d requested_workers=%ld effective_workers=%zu running=%d diag=%d\n",
                started ? 1 : 0, workers, eff_workers, pool.running() ? 1 : 0, diag ? 1 : 0);
    {
        const auto& b = pool.budget();
        std::printf("budget msgs=%zu bytes=%zu time_us=%lld wait_ms=%lld idle_ms=%lld\n", b.max_messages_per_route,
                    b.max_bytes_per_route, (long long)b.max_processing_time_per_route.count(),
                    (long long)b.wait_timeout.count(), (long long)b.idle_keep_alive.count());
    }
    phase("pool_start", 0, "workers=" + std::to_string(eff_workers) + " diag=" + (diag ? "on" : "off"));

    for (long i = 0; i < n; ++i)
        names.push_back("r5_" + std::to_string(state) + "_" + std::to_string(dom) + "_" + std::to_string(i));

    /* ---------------- 阶段 1：按状态建对象 ---------------- */
    t0 = Clock::now();
    const bool need_pub = (state != 2);
    if (need_pub)
        for (long i = 0; i < n; ++i)
        {
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names[static_cast<std::size_t>(i)],
                                                          static_cast<std::size_t>(dom), false));
            pubs.back()->InitChannel("r5");
        }
    for (long i = 0; i < n; ++i)
    {
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names[static_cast<std::size_t>(i)],
                                                      static_cast<std::size_t>(dom), 64, false));
        subs.back()->InitChannel("r5");
    }
    const long create_ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    const long created_pubs = static_cast<long>(pubs.size());
    const long created_subs = static_cast<long>(subs.size());
    phase("register", create_ms, "state=" + std::to_string(state) + " pubs=" + std::to_string(pubs.size()) +
                                     " subs=" + std::to_string(subs.size()));

    /* ---------------- 阶段 2：握手（批量轮询；⛔ 不逐 route sleep） ---------------- */
    t0 = Clock::now();
    long attached = 0;
    if (need_pub)
    {
        const auto dl = Clock::now() + std::chrono::seconds(120);
        while (Clock::now() < dl)
        {
            attached = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)],
                                                  static_cast<std::size_t>(dom)))
                    && cp.peer_count() >= 1 && cp.state() == dzIPC::control_plane_shm::TopicState::Ready)
                    ++attached;
            }
            if (attached >= n) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    phase("handshake", static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
          "attached=" + std::to_string(attached) + "/" + std::to_string(n));
    std::printf("handshake attached=%ld/%ld\n", attached, n);

    /* ---------------- 阶段 3：状态 3 的**逐 route 确认帧**（方案 §6.3 三态必录） ----------------
     * 状态 3 的定义是"已连接、**有效订阅**、停止发布"。⛔ 不得以"线程数少/CPU 低"代替
     * 有效订阅的成绩 ⇒ 这里对**每个** route 发一条确认帧并逐条收回、逐字节校验。 */
    long confirmed = 0;
    std::vector<long> confirm_missing;
    std::vector<float> confirm_first_us;    /* 每 route 的确认首包延迟（µs） */
    t0 = Clock::now();
    if (state == 3 && !no_confirm)
    {
        for (long i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "CF" + std::to_string(i);
            if (pubs[static_cast<std::size_t>(i)]->publish(m)) { /* 发送尝试 */ }
        }
        std::vector<long long> send_us(static_cast<std::size_t>(n), -1);
        const long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                                  Clock::now().time_since_epoch()).count();
        for (long i = 0; i < n; ++i) send_us[static_cast<std::size_t>(i)] = now;
        std::vector<char> got(static_cast<std::size_t>(n), 0);
        std::vector<long long> first_us(static_cast<std::size_t>(n), -1);
        const auto dl = Clock::now() + std::chrono::seconds(30);
        while (confirmed < n && Clock::now() < dl)
        {
            for (long i = 0; i < n; ++i)
            {
                if (got[static_cast<std::size_t>(i)]) continue;
                auto sink = td();
                while (subs[static_cast<std::size_t>(i)]->try_get_clone(sink))
                {
                    auto s = sink->topic() ? sink->topic()->msgcast<dzIPC::Msg::StdString>() : nullptr;
                    if (s && s->str == ("CF" + std::to_string(i)))
                    {
                        got[static_cast<std::size_t>(i)] = 1;
                        first_us[static_cast<std::size_t>(i)] =
                            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
                        ++confirmed;
                        break;
                    }
                    sink = td();
                }
            }
            if (confirmed < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (long i = 0; i < n; ++i)
        {
            if (!got[static_cast<std::size_t>(i)]) confirm_missing.push_back(i);
            confirm_first_us.push_back(first_us[static_cast<std::size_t>(i)] >= 0
                                           ? static_cast<float>((first_us[static_cast<std::size_t>(i)] -
                                                                 send_us[static_cast<std::size_t>(i)]) / 1000.0)
                                           : -1.0f);
        }
        phase("confirm", static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
              "confirmed=" + std::to_string(confirmed) + "/" + std::to_string(n));
        std::printf("confirm confirmed=%ld/%ld missing=%zu\n", confirmed, n, confirm_missing.size());
    }
    else if (state == 3)
    {
        phase("confirm", 0, "skipped（--no-confirm）");
    }

    /* ---------------- 阶段 4：状态 1 释放全部对象（无 route） ---------------- */
    t0 = Clock::now();
    if (state == 1)
    {
        subs.clear();
        pubs.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    if (settle_s > 0) std::this_thread::sleep_for(std::chrono::seconds(settle_s));
    phase("state_settle", static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
          "state=" + std::to_string(state) + " routes=" + std::to_string(pool.route_count()) +
              " settle_s=" + std::to_string(settle_s));

    /* ---------------- 阶段 5：窗口采样（三态都采；窗口 ≥60 s） ----------------
     * state3：此时**已停止发布**且静默时间会超过 keep-alive（1000 ms）⇒ 正好落在
     * "已连接、有效订阅、停止发布"的状态 3 上。
     * 负载档（--load-msgs>0 且 state==3）在窗口内持续发布，用于"有效消息负载"对照。 */
    std::vector<WindowRow> rows;
    std::atomic<bool> stop{false};
    std::atomic<long> sent{0}, rx{0};
    std::thread feeder;
    const bool with_load = (state == 3 && load_msgs > 0);
    if (with_load)
    {
        feeder = std::thread([&] {
            long i = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                auto m = std::make_shared<dzIPC::Msg::StdString>();
                m->set_msg_id(kMsgId);
                m->str = "L" + std::to_string(i);
                if (pubs[static_cast<std::size_t>(i % n)]->publish(m)) sent.fetch_add(1);
                auto sink = td();
                if (subs[static_cast<std::size_t>(i % n)]->try_get_clone(sink)) rx.fetch_add(1);
                ++i;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    t0 = Clock::now();
    {
        const auto w0 = Clock::now();
        const size_t t_before = thread_count();
        const double c0 = proc_cpu_ticks();
        const Ctx x0 = cpu_only ? Ctx{} : ctxsum();
        const Resident r0 = snap_resident();
        const Gated g0 = snap_gated();
        const auto sch0 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
        size_t peak = t_before;
        while (std::chrono::duration<double>(Clock::now() - w0).count() * 1000.0 < static_cast<double>(window_ms))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const size_t t = thread_count();
            if (t > peak) peak = t;
        }
        const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
        const double c1 = proc_cpu_ticks();
        const Ctx x1 = cpu_only ? Ctx{} : ctxsum();
        const Gated g1 = snap_gated();
        const auto sch1 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
        const Resident r1 = snap_resident();

        WindowRow w;
        w.idx = 0;
        w.state = state;
        w.phase = with_load ? "load" : "idle";
        w.wall_s = wall;
        w.threads = t_before;
        w.peak_threads = peak;
        w.cpu_cores = (c0 >= 0 && c1 >= 0) ? (c1 - c0) / 100.0 / wall : -1.0;
        w.ctx_per_s = static_cast<double>((x1.vol - x0.vol) + (x1.nonvol - x0.nonvol)) / wall;
        w.ctx_unreadable = x0.unreadable + x1.unreadable;
        w.routes = static_cast<long>(pool.route_count());
        w.control_entries = static_cast<long>(dzIPC::shm_control::ShmControlScheduler::instance().entry_count());
        w.d_tick = sch1.tick_count - sch0.tick_count;
        w.tick_per_s = wall > 0 ? static_cast<double>(w.d_tick) / wall : 0.0;
        w.tick_max_us = static_cast<double>(sch1.tick_duration_max_ns) / 1000.0;
        w.tick_last_us = static_cast<double>(sch1.tick_duration_last_ns) / 1000.0;
        w.tick_overruns = sch1.tick_overrun_count - sch0.tick_overrun_count;
        w.d_idle_exits = static_cast<long>(r1.idle_exits - r0.idle_exits);
        w.d_thread_restarts = static_cast<long>(r1.thread_restarts - r0.thread_restarts);
        w.r0 = r0; w.r1 = r1; w.g0 = g0; w.g1 = g1;

        const std::uint64_t d_rounds = r1.scan_rounds - r0.scan_rounds;
        const std::uint64_t d_scan = r1.scanned_routes_total - r0.scanned_routes_total;
        const std::uint64_t d_ready = r1.scan_ready_rounds - r0.scan_ready_rounds;
        const std::uint64_t d_g_rounds = g1.scan_rounds - g0.scan_rounds;
        const std::uint64_t d_g_scan = g1.scanned_routes_total - g0.scanned_routes_total;
        const std::uint64_t d_g_time = g1.scan_time_ns_total - g0.scan_time_ns_total;
        const std::uint64_t d_g_ready = g1.ready_observed - g0.ready_observed;
        const std::uint64_t d_g_timeout = g1.wait_timeout_count - g0.wait_timeout_count;

        const char* uncollected = diag ? "" : "诊断关闭 ⇒ 门控量**未采集**（⛔ 不得解释为零成本）";
        w.scan_per_round = ratio(d_scan, d_rounds, "Δscan_rounds", uncollected);
        w.ns_per_round = ratio(d_g_time, d_g_rounds, "Δscan_rounds(门控)", uncollected);
        w.ns_per_entry = ratio(d_g_time, d_g_scan, "Δscanned_routes_total(门控)", uncollected);
        w.ready_ratio = ratio(d_ready, d_rounds, "Δscan_rounds", uncollected);
        /* ⛔ 自纠（t38 采集后复核发现）：方案 §6.3 的 "每秒等待超时 = Δwait_timeout_count / window_seconds"
         * 用的是**门控**计数 `wait_timeout_count`。诊断关闭时它**未采集**，因此该派生值必须是
         * `null + 原因`；首版误用 `ratio(0, window)` 得到 0.0 —— 那等于把"未采集"当成了
         * "零成本"，正是方案 §6.2 明令禁止的读法。现改为：诊断关闭 ⇒ null。
         * 同时**另给**一个用常驻孪生 `wait_timeouts` 计算的 `resident_wait_timeouts_per_s`
         *（该量恒被采集），名称里显式带 `resident_` 以免与门控量混淆。 */
        w.timeouts_per_s = diag
            ? ratio(d_g_timeout, static_cast<std::uint64_t>(std::llround(wall * 1000.0)), "window_ms", "")
            : Ratio{false, 0.0, "诊断关闭 ⇒ 门控计数 wait_timeout_count **未采集**（⛔ 非零成本；见 resident_wait_timeouts_per_s）"};
        w.resident_timeouts_per_s = ratio(r1.wait_timeouts - r0.wait_timeouts,
                                          static_cast<std::uint64_t>(std::llround(wall * 1000.0)), "window_ms", "");

        /* 常驻/门控同窗口自洽：diag=on 时两侧**必须逐值相等**；diag=off 时门控应恒 0。 */
        if (diag)
            w.resident_gated_consistent = (d_rounds == d_g_rounds) && (d_scan == d_g_scan) && (d_ready == d_g_ready) &&
                                          (static_cast<std::uint64_t>(r1.wait_timeouts - r0.wait_timeouts) == d_g_timeout);
        else
            w.resident_gated_consistent = (d_g_rounds == 0 && d_g_scan == 0 && d_g_time == 0 && d_g_ready == 0);

        /* ---- 运行时门禁（方案 §6.2）逐条机械断言 ---- */
        const bool has_routes = w.routes > 0;
        auto gate = [&](const char* name, bool ok) { w.gates.push_back({name, ok}); if (!ok) fail(std::string("门禁失败: ") + name); };
        if (has_routes)
        {
            gate("有route时 Δscan_rounds>0", d_rounds > 0);
            gate("有route时 Δscanned_routes_total>0", d_scan > 0);
            if (diag) gate("有route时诊断扫描时间有效(>0)", d_g_time > 0);
            gate("0<=Δready_observed<=Δscan_rounds(门控)",
                 diag ? (d_g_ready <= d_g_rounds) : true);
            gate("0<=Δscan_ready_rounds<=Δscan_rounds(常驻)", d_ready <= d_rounds);
            gate("deferred 深度非负", r1.deferred_depth_last >= 0 && r1.deferred_depth_max >= 0);
            gate("deferred 深度不超过在册 route 数", r1.deferred_depth_max <= static_cast<std::uint64_t>(std::max<long>(1, w.routes)));
        }
        else
        {
            gate("无route时 Δscanned_routes_total 可为 0", d_scan == 0 || d_scan > 0);
        }
        gate("常驻/门控同窗口自洽", w.resident_gated_consistent);
        if (diag) gate("诊断开启时 Δwait_timeout_count 与 wait_timeouts 同口径", d_g_timeout == (r1.wait_timeouts - r0.wait_timeouts));
        rows.push_back(w);
        phase("window", static_cast<long long>(w.wall_s * 1000), "state=" + std::to_string(state) + " phase=" + w.phase);
    }
    stop.store(true, std::memory_order_relaxed);
    if (feeder.joinable()) feeder.join();
    if (with_load)
        phase("load", 0, "sent=" + std::to_string(sent.load()) + " rx=" + std::to_string(rx.load()));

    /* ---------------- 阶段 6：状态 3 恢复（**不重新注册**） ----------------
     * 要求：静默超过 keep-alive（wait 100 ms / idle 1000 ms）后原发布者恢复发送，
     * 订阅者**不重新注册**即可收到；记录**全部**恢复首包（逐 route）。 */
    std::vector<float> recover_first_us;
    std::vector<long> recover_missing;
    long recover_ok_routes = 0;
    long recover_lost = 0;
    if (state == 3 && !skip_recover)
    {
        t0 = Clock::now();
        /* 静默 ≥ idle_keep_alive（1000 ms）已达（上一步窗口 ≥60 s）⇒ 线程应已归还。 */
        const auto st_before = pool.stats();
        const size_t th_before = thread_count();
        std::vector<long long> send_us;
        for (long i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "RC" + std::to_string(i);
            send_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                  Clock::now().time_since_epoch()).count());
            (void)pubs[static_cast<std::size_t>(i)]->publish(m);
            /* ⛔ 不重新注册：不调用任何 InitChannel / reset */
        }
        std::vector<char> got(static_cast<std::size_t>(n), 0);
        /* ⛔ 逐 route 记延迟：缺项写 -1（**全部**恢复首包都必须在数组里，不能缩短数组）。 */
        recover_first_us.assign(static_cast<std::size_t>(n), -1.0f);
        const auto dl = Clock::now() + std::chrono::seconds(30);
        while (recover_ok_routes < n && Clock::now() < dl)
        {
            for (long i = 0; i < n; ++i)
            {
                if (got[static_cast<std::size_t>(i)]) continue;
                auto sink = td();
                while (subs[static_cast<std::size_t>(i)]->try_get_clone(sink))
                {
                    auto s = sink->topic() ? sink->topic()->msgcast<dzIPC::Msg::StdString>() : nullptr;
                    if (s && s->str == ("RC" + std::to_string(i)))
                    {
                        got[static_cast<std::size_t>(i)] = 1;
                        const long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                                                  Clock::now().time_since_epoch()).count();
                        recover_first_us[static_cast<std::size_t>(i)] =
                            static_cast<float>((now - send_us[static_cast<std::size_t>(i)]) / 1000.0);
                        ++recover_ok_routes;
                        break;
                    }
                    sink = td();
                }
            }
            if (recover_ok_routes < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (long i = 0; i < n; ++i)
            if (!got[static_cast<std::size_t>(i)]) recover_missing.push_back(i);
        recover_lost = n - recover_ok_routes;
        const auto st_after = pool.stats();
        phase("recover", static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()),
              "ok=" + std::to_string(recover_ok_routes) + "/" + std::to_string(n) + " lost=" + std::to_string(recover_lost) +
                  " threads_before=" + std::to_string(th_before));
        std::printf("recover ok=%ld/%ld lost=%ld threads_before=%zu idle_exits_delta=%llu\n", recover_ok_routes, n,
                    recover_lost, th_before,
                    static_cast<unsigned long long>(st_after.idle_exits - st_before.idle_exits));
    }

    /* ---------------- 阶段 7：收尾，采集三态必录字段 ---------------- */
    const auto st_final = pool.stats();
    const long final_routes = static_cast<long>(pool.route_count());
    const size_t final_threads = thread_count();
    const long idle_exits = static_cast<long>(st_final.idle_exits);
    const long thread_restarts = static_cast<long>(st_final.thread_restarts);

    /* ---------------- 判定 ---------------- */
    int verdict = gates_fail.empty() ? 0 : 1;
    if (state == 3 && !no_confirm && confirmed != n) verdict = 1;
    if (state == 3 && !skip_recover && recover_lost != 0) verdict = 1;
    if (state == 3 && with_load) { /* 负载档不要求全收（开环），但必须都发出去 */ }

    /* ---------------- 落盘 ---------------- */
    if (!out.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec))
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s\n", out.c_str());
            return 2;
        }
        std::filesystem::create_directories(out, ec);
        const auto sh = [&](const char* f) { return out + "/" + f; };

        {
            std::ofstream f(sh("phases.csv"));
            f << "phase,ms,note\n";
            for (auto& p : phases) f << p << "\n";
        }
        {
            std::ofstream f(sh("windows.csv"));
            f << "window,state,phase,routes,control_entries,wall_s,threads,peak_threads,cpu_cores,ctx_per_s,"
                 "ctx_unreadable_tids,"
                 "d_scan_rounds,d_scanned_routes_total,d_ready_rounds,d_wait_timeouts,"
                 "d_idle_exits,d_thread_restarts,"
                 "d_g_scan_rounds,d_g_scanned_routes_total,d_g_scan_time_ns,d_g_ready_observed,d_g_wait_timeout_count,"
                 "avg_scanned_per_round,avg_ns_per_round,avg_ns_per_entry,ready_round_ratio,wait_timeouts_per_s,"
                 "resident_wait_timeouts_per_s,d_tick,tick_per_s,sched_tick_last_us,sched_tick_max_us,sched_overruns,"
                 "resident_gated_consistent,gates_passed\n";
            for (auto& w : rows)
            {
                size_t gpass = 0;
                for (auto& g : w.gates) if (g.second) ++gpass;
                f << w.idx << "," << w.state << "," << w.phase << "," << w.routes << "," << w.control_entries << ","
                  << w.wall_s << "," << w.threads << "," << w.peak_threads << "," << w.cpu_cores << "," << w.ctx_per_s
                  << "," << w.ctx_unreadable << ","
                  << (w.r1.scan_rounds - w.r0.scan_rounds) << "," << (w.r1.scanned_routes_total - w.r0.scanned_routes_total)
                  << "," << (w.r1.scan_ready_rounds - w.r0.scan_ready_rounds) << ","
                  << (w.r1.wait_timeouts - w.r0.wait_timeouts) << "," << w.d_idle_exits << "," << w.d_thread_restarts
                  << "," << (w.g1.scan_rounds - w.g0.scan_rounds) << ","
                  << (w.g1.scanned_routes_total - w.g0.scanned_routes_total) << ","
                  << (w.g1.scan_time_ns_total - w.g0.scan_time_ns_total) << ","
                  << (w.g1.ready_observed - w.g0.ready_observed) << ","
                  << (w.g1.wait_timeout_count - w.g0.wait_timeout_count) << "," << fmt(w.scan_per_round) << ","
                  << fmt(w.ns_per_round) << "," << fmt(w.ns_per_entry) << "," << fmt(w.ready_ratio) << ","
                  << fmt(w.timeouts_per_s) << "," << fmt(w.resident_timeouts_per_s) << "," << w.d_tick << ","
                  << w.tick_per_s << "," << w.tick_last_us << "," << w.tick_max_us << "," << w.tick_overruns << ","
                  << (w.resident_gated_consistent ? 1 : 0) << "," << gpass << "/"
                  << w.gates.size() << "\n";
            }
        }
        {
            std::ofstream f(sh("windows.jsonl"));
            for (auto& w : rows)
            {
                f << "{\"run_id\":\"" << run_id << "\",\"window\":" << w.idx << ",\"state\":" << w.state
                  << ",\"phase\":\"" << w.phase << "\",\"routes\":" << w.routes
                  << ",\"control_entries\":" << w.control_entries << ",\"wall_s\":" << w.wall_s
                  << ",\"diagnostics_enabled\":" << (diag ? "true" : "false")
                  << ",\"derived\":{\"avg_scanned_per_round\":" << js(w.scan_per_round)
                  << ",\"avg_ns_per_round\":" << js(w.ns_per_round) << ",\"avg_ns_per_entry\":" << js(w.ns_per_entry)
                  << ",\"ready_round_ratio\":" << js(w.ready_ratio) << ",\"wait_timeouts_per_s\":" << js(w.timeouts_per_s)
                  << ",\"resident_wait_timeouts_per_s\":" << js(w.resident_timeouts_per_s)
                  << "},\"null_reasons\":{\"avg_ns_per_round\":\"" << (w.ns_per_round.valid ? "" : w.ns_per_round.reason)
                  << "\",\"avg_ns_per_entry\":\"" << (w.ns_per_entry.valid ? "" : w.ns_per_entry.reason)
                  << "\"},\"resident_gated_consistent\":" << (w.resident_gated_consistent ? "true" : "false") << "}\n";
            }
        }
        {
            std::ofstream f(sh("counters.json"));
            const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
            f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"diagnostics_enabled\": " << (diag ? "true" : "false")
              << ",\n  \"kind\": \"counter_snapshot\",\n  \"note\": \""
              << (diag ? "门控面已采集" : "门控面**未采集**（诊断关）⇒ 原始 0 不代表零成本")
              << "\",\n  \"counters\": {\n";
            for (std::size_t i = 0; i < dzIPC::measure::kCounterCount; ++i)
            {
                const auto& m = dzIPC::measure::counter_table()[i];
                f << "    \"" << m.name << "\": " << c.values[i]
                  << (i + 1 < dzIPC::measure::kCounterCount ? "," : "") << "\n";
            }
            f << "  }\n}\n";
        }
        {
            std::ofstream f(sh("threestate.json"));
            f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"state\": " << state << ",\n";
            f << "  \"应用对象\": {\"created_pubs\": " << created_pubs << ", \"created_subs\": " << created_subs
              << ", \"live_pubs\": " << pubs.size() << ", \"live_subs\": " << subs.size()
              << "},\n  \"应用对象说明\": \"created_* = 本阶段创建过的对象数；live_* = 采样时仍存活的对象数"
                 "（state 1 为创建后全部析构）\",\n";
            f << "  \"控制项\": " << dzIPC::shm_control::ShmControlScheduler::instance().entry_count() << ",\n";
            f << "  \"worker_route_counts\": {\"total\": " << final_routes << ", \"workers\": " << eff_workers << "},\n";
            f << "  \"routes_final\": " << final_routes << ",\n";
            f << "  \"window_routes\": " << (rows.empty() ? -1 : rows.front().routes) << ",\n";
            for (auto& w : rows)
                f << "  \"window_phase\": \"" << w.phase << "\",\n  \"window_routes\": " << w.routes
                  << ",\n  \"window_control_entries\": " << w.control_entries << ",\n";
            f << "  \"threads_final\": " << final_threads << ",\n";
            f << "  \"idle_exits_total\": " << idle_exits << ",\n";
            f << "  \"thread_restarts_total\": " << thread_restarts << ",\n";
            for (auto& w : rows)
                f << "  \"window_idle_exits\": " << w.d_idle_exits << ",\n  \"window_thread_restarts\": "
                  << w.d_thread_restarts << ",\n";
            f << "  \"confirm\": {\"confirmed\": " << confirmed << ", \"missing_count\": " << confirm_missing.size()
              << "},\n";
            f << "  \"confirm_first_packet_us\": [";
            for (std::size_t i = 0; i < confirm_first_us.size(); ++i)
                f << (i ? "," : "") << confirm_first_us[i];
            f << "],\n  \"recover_first_packet_us\": [";
            for (std::size_t i = 0; i < recover_first_us.size(); ++i) f << (i ? "," : "") << recover_first_us[i];
            f << "],\n  \"recover_ok_routes\": " << recover_ok_routes << ",\n  \"recover_lost\": " << recover_lost
              << ",\n  \"recover_missing_routes\": [";
            for (std::size_t i = 0; i < recover_missing.size(); ++i) f << (i ? "," : "") << recover_missing[i];
            f << "],\n  \"confirm_missing_routes\": [";
            for (std::size_t i = 0; i < confirm_missing.size(); ++i) f << (i ? "," : "") << confirm_missing[i];
            f << "]\n}\n";
        }
        {
            std::ofstream f(sh("manifest.json"));
            f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"work_package\": \"W10-R5\",\n";
            f << "  \"round\": " << round << ",\n  \"state\": " << state << ",\n  \"phase\": \""
              << (rows.empty() ? "n/a" : rows.front().phase) << "\",\n";
            f << "  \"topic_count\": " << n << ",\n  \"requested_workers\": " << workers
              << ",\n  \"effective_workers\": " << eff_workers << ",\n";
            f << "  \"diagnostics_enabled\": " << (diag ? "true" : "false") << ",\n";
            f << "  \"window_ms\": " << window_ms << ",\n  \"window_s_min_required\": 60,\n";
            f << "  \"port_policy\": \"SHM（无端口）\",\n";
            f << "  \"criteria_version\": \"" << criteria_version << "\",\n";
            f << "  \"lib_path\": \"" << lib_path_before << "\",\n  \"lib_sha256\": \"" << lib_sha_before
              << "\",\n  \"tool_sha256\": \"" << tool_sha << "\",\n";
            f << "  \"deadlines\": {\"wait_timeout_ms\": " << kWaitTimeoutMs << ", \"idle_keep_alive_ms\": "
              << kIdleKeepAliveMs << ", \"handshake_s\": 120, \"recover_s\": 30},\n";
            f << "  \"capacity_model\": {\"per_worker_wait_set\": " << kWaitSetCap << ", \"total\": "
              << kWaitSetCap * static_cast<long>(eff_workers) << ", \"capacity_limited\": "
              << ((n > kWaitSetCap * static_cast<long>(eff_workers)) ? "true" : "false") << "},\n";
            f << "  \"input_manifest_hash\": \"topics=r5_<state>_" << dom << "_0.." << (n - 1) << " sha256=pending\",\n";
            f << "  \"result_files\": [\"manifest.json\",\"phases.csv\",\"windows.csv\",\"windows.jsonl\","
                 "\"counters.json\",\"threestate.json\",\"verdict.md\"]\n}\n";
        }
        {
            std::ofstream f(sh("verdict.md"));
            f << "# W10-R5 §10.2 / 三态复验 — 独立结论\n\n";
            f << "- run_id: `" << run_id << "`｜state " << state << "｜n=" << n << "｜workers=" << eff_workers
              << "｜diag=" << (diag ? "on" : "off") << "｜round=" << round << "\n";
            f << "- 判据版本: `" << criteria_version << "`\n";
            f << "- 判定: **" << (verdict ? "不通过" : "通过") << "**\n\n";
            f << "## 运行时门禁（方案 §6.2）\n\n| 门禁 | 结果 |\n|---|---|\n";
            for (auto& w : rows)
                for (auto& g : w.gates) f << "| " << g.first << " | " << (g.second ? "✅" : "❌") << " |\n";
            f << "\n## 五项派生值（同一窗口差值；零分母 = null + 原因）\n\n";
            f << "| 量 | 值 | null 原因 |\n|---|---|---|\n";
            for (auto& w : rows)
            {
                f << "| 平均每轮扫描条目 | " << fmt(w.scan_per_round) << " | " << (w.scan_per_round.valid ? "-" : w.scan_per_round.reason) << " |\n";
                f << "| 平均每轮扫描耗时(ns) | " << fmt(w.ns_per_round) << " | " << (w.ns_per_round.valid ? "-" : w.ns_per_round.reason) << " |\n";
                f << "| 平均每条目扫描耗时(ns) | " << fmt(w.ns_per_entry) << " | " << (w.ns_per_entry.valid ? "-" : w.ns_per_entry.reason) << " |\n";
                f << "| 发现就绪的轮次占比 | " << fmt(w.ready_ratio) << " | " << (w.ready_ratio.valid ? "-" : w.ready_ratio.reason) << " |\n";
                f << "| 每秒等待超时（门控 `wait_timeout_count`） | " << fmt(w.timeouts_per_s) << " | "
                  << (w.timeouts_per_s.valid ? "-" : w.timeouts_per_s.reason) << " |\n";
                f << "| 每秒等待超时（常驻孪生 `wait_timeouts`，标注 resident_） | " << fmt(w.resident_timeouts_per_s)
                  << " | " << (w.resident_timeouts_per_s.valid ? "-" : w.resident_timeouts_per_s.reason) << " |\n";
            }
            f << "\n> ⛔ `scan_time_ns_total` 是**墙钟区间累计**，不得换算为 CPU core。CPU 由独立采样给出。\n";
            f << "\n## 失败清单（" << gates_fail.size() << " 条）\n\n";
            if (gates_fail.empty()) f << "（无）\n";
            for (auto& x : gates_fail) f << "- " << x << "\n";
        }
        std::printf("artifacts_dir=%s\n", out.c_str());
    }

    const std::string lib_sha_after = lib_path_before.empty() ? std::string() : sha256_of_file(lib_path_before);
    if (!lib_sha_after.empty() && lib_sha_after != lib_sha_before)
    {
        verdict = 1;
        gates_fail.push_back("库在运行期间被替换（lib_sha before=" + lib_sha_before + " after=" + lib_sha_after +
                             "）⇒ 本 run 读数不作数（R0-A1）");
    }
    std::printf("W10_R5_DONE verdict=%s failures=%zu routes=%ld threads=%zu diag=%d\n", verdict ? "FAIL" : "PASS",
                gates_fail.size(), final_routes, final_threads, diag ? 1 : 0);
    for (auto& x : gates_fail) std::printf("GATE_FAIL: %s\n", x.c_str());
    std::fflush(stdout);
    /* ⛔ 池是进程级单例且 stop 后不可重启：显式析构顺序（先 pub/sub 后进程退出）以避免
     * "对象还活着就退库" 的既有崩溃形态（r25 教训）。 */
    subs.clear();
    pubs.clear();
    return verdict;
}
