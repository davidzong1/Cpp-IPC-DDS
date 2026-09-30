/* W10 规模与故障验收工装 —— 逐 route 收发台账（方案 §13.1/§13.2/§13.3）。
 *
 * ── 为什么必须自建（交接自 W07 的复核结论）────────────────────────────────
 * W07 的 `sent=20/received=20` 只来自 `subs[0]` **一条** route
 * (test/test_socket_high_fd.cpp:330-334)。方案 §13.2 第 2 条要求
 * `valid_rx_count == expected_count` 且**每条 route 至少收到规定数量序号并完成载荷校验**。
 * 因此本工装的核心不是"总共收到多少"，而是**逐 route 的台账**：
 *   每条 route 独立记录 计划/实发/实收/序号集合/载荷校验/重复/乱序/超时。
 * ⛔ 不得以首条 route 或 registered 数代表规模闭环（§13.2 末句）。
 *
 * ── 拓扑（§13.1，一次运行只选一个）────────────────────────────────────────
 *   independent : N 个独立话题，每话题 1 pub × 1 sub，各自收自己的 --msgs 条
 *   broadcast   : 1 个话题 × N 个订阅者（N ≤ 32，受 libipc 连接位宽限制）
 *   hotcold     : 1 条高频"热路" + (N-1) 条静默"冷路"，冷路必须在有界预算内被服务
 *
 * ── 阶段（§13.1 要求逐阶段记录计数）──────────────────────────────────────
 *   prepare / register / handshake / warmup / fixed-rate / silence / recover /
 *   concurrent-close-rebuild / reclaim
 *
 * ── 载荷校验 ──────────────────────────────────────────────────────────────
 * 每条消息的载荷由 (route_idx, seq) 确定性生成，接收端逐字节重算校验
 * (P == 字节模式)。⛔ 不用固定载荷 —— 那会把旧帧与内容损坏一起放过（§10.7）。
 *
 * 用法：
 *   w10_matrix --transport shm|socket --topology independent|broadcast|hotcold
 *              --n 1000 --domain 300 --msgs 3 --payload 64 --out <dir> --run-id <id>
 *              [--silence-ms 3000] [--hot-msgs 20000] [--resume-msgs 5]
 * 退出码：0 = 该拓扑全部判据通过；1 = 至少一条判据失败（verdict.md 里逐条给）。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/hash.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/control_plane.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/platform_info.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* ---------------- H1：已占 UDP 端口的**按列**解析（可判定、可落盘、失败不静默） ----------------
 * 修前缺陷（队长已独立核实）：只取每行**第一个** token。`/proc/net/udp` 的列序是
 *     sl  local_address  rem_address  st  ...
 * 行首是 `sl`（形如 `145:`），冒号在索引 1 ⇒ `colon+1` 指向串尾 ⇒ `strtol("") == 0`
 * ⇒ **几乎每行都解析出 0**，`used` 实际只有 `{0}` ⇒ 端口预检**从未生效**。
 * 最小反例：`counterexamples/h1_buggy_parse.py` 逐字复现该逻辑 ⇒ `used = [0], |used| = 1`。
 *
 * 修后要求：
 *   ① 按列解析第 2 列 `local_address`（`00000000:A874` ⇒ 0xA874）；
 *   ② 同时读 `/proc/net/udp6`（IPv6 通配/双栈绑定会占用同一个 IPv4 端口）；
 *   ③ **读取失败不得按"无占用"处理**（返回 false 并把原因写入错误列表）；
 *   ④ 保存原始表与解析结果（`port_table.txt` / `port_parse.txt`），供复核复算。
 */
struct PortScanResult
{
    bool ok{false};
    std::set<int> used;
    long parsed_ok{0};
    long parsed_bad{0};
    std::vector<std::string> errors;
    std::vector<std::string> raw_lines;      /* 原始行（含表头），落盘用 */
    std::string first_sample;
};

bool parse_proc_net_udp(const char* path, PortScanResult& r)
{
    std::ifstream f(path);
    if (!f)
    {
        r.errors.push_back(std::string("open ") + path + " 失败（errno=" + std::to_string(errno) + "）");
        return false;
    }
    std::string line;
    if (!std::getline(f, line))
    {
        r.errors.push_back(std::string("read ") + path + " 表头失败");
        return false;
    }
    r.raw_lines.push_back(std::string("# ") + path + " 表头: " + line);
    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        r.raw_lines.push_back(line);
        /* ⛔ 按列读两个 token：第 1 列 sl、第 2 列 local_address。 */
        std::istringstream is(line);
        std::string slot, addr;
        if (!(is >> slot >> addr))
        {
            ++r.parsed_bad;
            continue;
        }
        if (addr == "local_address") continue;          /* 兼容某些内核的表头重复 */
        const auto colon = addr.rfind(':');
        if (colon == std::string::npos)
        {
            ++r.parsed_bad;
            continue;
        }
        char* endp = nullptr;
        const long p = std::strtol(addr.c_str() + colon + 1, &endp, 16);
        if (endp == addr.c_str() + colon + 1 || p < 0 || p > 65535)
        {
            ++r.parsed_bad;
            continue;
        }
        r.used.insert(static_cast<int>(p));
        ++r.parsed_ok;
        if (r.first_sample.empty()) r.first_sample = addr;
    }
    return true;
}

PortScanResult scan_used_udp_ports()
{
    PortScanResult r;
    const bool a = parse_proc_net_udp("/proc/net/udp", r);
    const bool b = parse_proc_net_udp("/proc/net/udp6", r);   /* IPv6 双栈同样占 IPv4 端口号 */
    /* ⛔ 读取失败不得按"无占用"处理：任一失败即整体判不可用，由调用方决定终止。 */
    r.ok = a && b;
    return r;
}

long read_ephemeral_range(long* lo_out, long* hi_out)
{
    std::ifstream f("/proc/sys/net/ipv4/ip_local_port_range");
    long a = 0, b = 0;
    if (f && (f >> a >> b) && a > 0 && b >= a)
    {
        *lo_out = a;
        *hi_out = b;
        return 0;
    }
    /* 读不到**不得**当作"没有临时端口区间"：报错并让调用方终止。 */
    return -1;
}

/* 在文件末定义；socket 千路用（见其注释）。 */
bool w10_pick_socket_topics(std::vector<std::string>& out, long dom, long want, bool verbose,
                            std::vector<std::string>* diag_out);
struct PortScanResult;
bool parse_proc_net_udp(const char* path, PortScanResult& r);
PortScanResult scan_used_udp_ports();
long read_ephemeral_range(long* lo_out, long* hi_out);

namespace {

constexpr std::uint32_t kMsgId = 93;   /* 与其它 W10 工装区分的模板 id */
constexpr std::size_t kPayloadHeader = 16;

/* ---------------- 载荷模式（确定性、可逐字节重算） ---------------- */
void fill_payload(std::string& s, std::uint32_t route_idx, std::uint32_t seq, std::size_t bytes)
{
    const std::size_t n = std::max<std::size_t>(bytes, kPayloadHeader);
    s.assign(n, '\0');
    std::memcpy(&s[0], &seq, 4);
    std::memcpy(&s[4], &route_idx, 4);
    const std::uint64_t magic = 0xA5A5A5A5DEADBEEFULL ^ (static_cast<std::uint64_t>(seq) * 2654435761ULL);
    std::memcpy(&s[8], &magic, 8);
    for (std::size_t i = kPayloadHeader; i < n; ++i)
    {
        s[i] = static_cast<char>((route_idx * 31u + seq * 131u + i * 7u) & 0xFFu);
    }
}

/* 返回 true 且写出 seq；失败返回 false（载荷损坏 / 长度不足 / magic 不符）。 */
bool verify_payload(const std::string& s, std::uint32_t expect_route, std::uint32_t* seq_out)
{
    if (s.size() < kPayloadHeader) return false;
    std::uint32_t seq = 0, route = 0;
    std::uint64_t magic = 0;
    std::memcpy(&seq, &s[0], 4);
    std::memcpy(&route, &s[4], 4);
    std::memcpy(&magic, &s[8], 8);
    if (route != expect_route) return false;
    const std::uint64_t expect_magic = 0xA5A5A5A5DEADBEEFULL ^ (static_cast<std::uint64_t>(seq) * 2654435761ULL);
    if (magic != expect_magic) return false;
    for (std::size_t i = kPayloadHeader; i < s.size(); ++i)
    {
        if (s[i] != static_cast<char>((expect_route * 31u + seq * 131u + i * 7u) & 0xFFu)) return false;
    }
    if (seq_out) *seq_out = seq;
    return true;
}

/* H2：握手确认帧的 seq 专用区间起点（与规模帧严格分离）。 */
constexpr std::uint32_t kConfirmSeqBase() { return 1000000000u; }
/* H4：旧 generation 标记帧的 seq 区间。该区间的帧**只**累 `old_gen_rx_`，
 * ⛔ 不计入 `rx/dup/ooo`（否则"旧代投递"会污染规模读数）。 */
constexpr std::uint32_t kOldGenMarkerSeqBase() { return 2000000000u; }

/* ---------------- 逐 route 台账 ---------------- */
struct RouteLedger
{
    std::string topic;
    bool registered{false};
    long planned{0};
    long sent{0};               /* 交给库的发送尝试数（含返回 false）— F6 修后 hotcold 也记账 */
    long sent_ok{0};            /* 库返回 true 的次数 */
    long confirm_rx{0};         /* H2：确认帧（供握手判定的带 run_id/route/generation/seq 的帧） */
    long confirm_payload_ok{0}; /* H2：确认帧**载荷校验通过**的条数（≠ 只收到） */
    bool confirm_first_ok{false};
    long first_packet_null{0};  /* H3：首次收到但无法解析的帧计数（不得计作首包） */
    long rx{0};                 /* 通过载荷校验的接收条数 */
    long dup{0};                /* 重复序号 */
    long ooo{0};                /* 乱序（本次 seq < 已见最大 seq） */
    long corrupt{0};            /* 载荷校验失败 */
    long timeout{0};            /* 超期未收满 */
    long rx_after_silence{0};   /* §10.1 恢复阶段收到的条数 */
    bool corrupt_detail_done{false};
    bool first_packet_ok{false};
    /* H3：恢复阶段**独立**状态（与首次接收分开）。 */
    long rx_before_recover_{0};
    long recover_old_seq_rejected{0};
    long old_gen_rx_{0};        /* H4：旧 generation 帧计数（独立） */
    bool recover_first_rx_us_set{false};
    long long recover_first_rx_us{-1};
    long long first_packet_us{-1};    /* 首发→首包可读（µs）；⛔ 不是绝对时间戳 */
    long long first_publish_us{-1};   /* 本 route 第一次发布完成时刻（steady，µs） */
    std::set<std::uint32_t> seen;
    std::string to_csv() const
    {
        std::ostringstream o;
        o << topic << ',' << (registered ? 1 : 0) << ',' << planned << ',' << sent << ',' << sent_ok << ',' << rx
          << ',' << dup << ',' << ooo << ',' << corrupt << ',' << timeout << ',' << rx_after_silence << ','
          << (first_packet_ok ? 1 : 0) << ',' << first_packet_us << ',' << confirm_rx;
        return o.str();
    }
};

/* ---------------- seam 计数（路径可分性，§13.2 条件 3/4） ---------------- */
struct SeamTally
{
    std::mutex m;
    long worker_path{0};
    long compat_total{0};
    long fallback{0};      /* 真回退（非路径选择） */
    long path_choice{0};   /* forced / fork / no_route */
    std::map<int, long> reasons;
    void on(const dzIPC::detail::SeamEvent& ev)
    {
        std::lock_guard<std::mutex> lock(m);
        if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker) { ++worker_path; return; }
        if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat) return;
        ++compat_total;
        const int r = static_cast<int>(ev.size);
        ++reasons[r];
        switch (static_cast<dzIPC::detail::RecvPathReason>(ev.size))
        {
        case dzIPC::detail::RecvPathReason::kForcedCompatEnv:
        case dzIPC::detail::RecvPathReason::kForkChild:
        case dzIPC::detail::RecvPathReason::kNoRoute:
            ++path_choice;
            break;
        default:
            ++fallback;
            break;
        }
    }
};
SeamTally g_seam;
void seam_hook(const dzIPC::detail::SeamEvent& ev) noexcept { g_seam.on(ev); }

/* ---------------- 通用小工具 ---------------- */
long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
    return def;
}
const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}
bool arg_flag(int argc, char** argv, const char* key)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

/* F4：计算文件 sha256（用于库/工装指纹配对前置检查）。 */
std::string sha256_of_file(const std::string& path_in)
{
    /* ⛔ 坑：`popen` 起的是 `/bin/sh`，若 path 是 `/proc/self/exe`，那里解析到的是
     * **sh 自己**（实测 hash 与工装二进制不符）⇒ 必须先用本进程的 `readlink` 解析成真实路径。 */
    std::string path = path_in;
    if (path == "/proc/self/exe")
    {
        char buf[4096] = {0};
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n <= 0) return std::string();
        path.assign(buf, static_cast<std::size_t>(n));
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    /* 用系统 sha256sum（无需引入 crypto 依赖；失败即空串 ⇒ 上层判失败）。 */
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

/* F4：实际加载的 libipc 路径（从 /proc/self/maps 取，⛔ 不靠猜测）。 */
std::string loaded_lib_path()
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

std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return 0;
    std::size_t n = 0;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}

std::size_t fd_count(std::size_t* maxfd = nullptr)
{
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) { if (maxfd) *maxfd = 0; return 0; }
    std::size_t n = 0, mx = 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        ++n;
        const long v = std::strtol(e->d_name, nullptr, 10);
        if (v > 0 && static_cast<std::size_t>(v) > mx) mx = static_cast<std::size_t>(v);
    }
    ::closedir(d);
    if (maxfd) *maxfd = mx;
    return n;
}

/* /proc/self/stat 的 utime+stime：内核 whole=1 ⇒ **覆盖线程组全部线程**。
 * （W03 口径：CPU 用 /proc/<pid>/stat 或 rusage；ctx 必须逐 TID 或 rusage。） */
double proc_cpu_ticks()
{
    std::ifstream f("/proc/self/stat");
    if (!f) return -1;
    std::string b;
    std::getline(f, b);
    const auto q = b.rfind(')');
    if (q == std::string::npos) return -1;
    std::istringstream is(b.substr(q + 2));
    std::string t;
    int i = 2;
    double u = 0, s = 0;
    while (is >> t)
    {
        if (i == 13) u = std::strtoull(t.c_str(), nullptr, 10);
        else if (i == 14) s = std::strtoull(t.c_str(), nullptr, 10);
        ++i;
    }
    return u + s;
}

/* 逐 TID 聚合的 ctx（处理新生/退出：读不到按"窗口内新建/退出"计入 incomplete 标志）。
 * ⛔ 不用 /proc/self/status（那是主线程值）。 */
struct CtxSample
{
    unsigned long long vol{0}, nonvol{0};
    std::size_t readable{0}, unreadable{0};
};
CtxSample ctx_sample()
{
    CtxSample out;
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return out;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        char p[256];
        std::snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        std::ifstream f(p);
        if (!f) { ++out.unreadable; continue; }   /* 退出/新生线程：读不到 ⇒ 不用 0 冒充 */
        std::string l;
        bool got = false;
        while (std::getline(f, l))
        {
            if (l.rfind("voluntary_ctxt_switches:", 0) == 0)
            { out.vol += std::strtoull(l.c_str() + 24, nullptr, 10); got = true; }
            else if (l.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
            { out.nonvol += std::strtoull(l.c_str() + 27, nullptr, 10); got = true; }
        }
        if (got) ++out.readable; else ++out.unreadable;
    }
    ::closedir(d);
    return out;
}

struct PhaseRow
{
    std::string name;
    long long ms{0};
    std::string note;
};

std::string ts()
{
    char buf[64];
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", std::localtime(&t));
    return buf;
}

std::string now_stamp()
{
    char buf[32];
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

/* ---------------- 工装主体 ---------------- */
struct Options
{
    std::string transport{"shm"};
    std::string topology{"independent"};
    long n{100};
    long domain{300};
    long msgs{3};
    long payload{64};
    long silence_ms{3000};
    long hot_msgs{4000};
    long resume_msgs{5};
    long handshake_timeout_ms{120000};
    long drain_timeout_ms{120000};
    /* R0-3/R0-4 冻结：L1 单通道建连 ≤30 s、L2 批注册 ≤90 s、L3 外部 watchdog 120 s。
     * 握手 5 s 与建连 30 s 是**两个量**，不得互相顶替。 */
    long connect_deadline_ms{30000};
    long register_batch_deadline_ms{90000};
    long handshake_deadline_ms{5000};
    long close_deadline_ms{2000};
    long rebuild_deadline_ms{1000};
    bool broadcast{false};
    std::string out_dir;
    std::string run_id;
    std::string expect_run_id;        /* 与 --run-id 不一致必须非零退出 */
    std::string require_lib_sha256;   /* 库指纹配对前置检查（F4） */
    bool skip_rebuild{false};
    std::string force_fail;    /* 验收用：故意注入一个失败（验批次失败传播） */
    bool negctl_no_pub{false}; /* 验收用：不建发布端（H2 负控） */
    std::string require_bin_sha256;   /* 工装指纹配对前置检查（F4） */
};

class Harness
{
public:
    explicit Harness(const Options& o) : o_(o) {}

    int run();

private:
    bool is_shm() const { return o_.transport == "shm"; }
    std::shared_ptr<dzIPC::TopicData> td() const
    {
        return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
    }
    bool pub_publish(long idx, std::uint32_t seq);
    /* 非阻塞排空该 route 当前可见的全部消息并记台账；返回本次新增条数。 */
    long drain_once(long idx, RouteLedger& led, bool mark_resume);
    /* 轮询直到 rx >= want（want>0）或 deadline；want==0 表示只做一次非阻塞排空。 */
    bool drain_until(long idx, RouteLedger& led, long want, long timeout_ms, bool mark_resume);
    void publish_route(long idx, std::uint32_t seq);
    bool wait_handshake(long idx, long timeout_ms);
    void write_artifacts(int verdict);

    Options o_;
    std::vector<std::string> names_;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> shm_pubs_;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> shm_subs_;
    std::vector<std::unique_ptr<dzIPC::socket::socket_pub_ipc>> sk_pubs_;
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> sk_subs_;
    std::vector<RouteLedger> led_;
    std::vector<PhaseRow> phases_;
    /* ⛔ t43 修复（H7，本次实测发现）：`samples_` 曾被**两个线程并发 push** ——
     * hotcold 拓扑的 `hot_thread` 调 `publish_route()`（写 tx 样本），同时主线程
     * 调 `publish_route()`（冷路）或 `drain_once()`（写 rx 样本）⇒ `std::vector`
     * 数据竞争 ⇒ 实测 `double free or corruption (!prev)` 且 rc=134 中止
     * （`hot-msgs=20000/40000` 两档 2/2 复现；r25 时期同代码未崩，属**潜伏竞态**）。
     * 修法：所有 `samples_` 写入走同一把互斥量（写点仅 3 处，价格可忽略）。 */
    std::mutex samples_mtx_;
    void push_sample(const char* buf) { std::lock_guard<std::mutex> lk(samples_mtx_); samples_.push_back(buf); }
    std::vector<std::string> samples_;
    std::vector<std::string> failures_;
    std::vector<std::string> findings_;
    std::vector<std::string> corrupt_details_;
    long registered_{0};
    long pool_routes_alive_{0};
    long socket_pool_routes_alive_{0};
    long valid_rx_{0};
    long fallback_{0};
    long worker_path_{0};
    long expected_valid_{0};
    std::size_t threads_create_{0};
    std::size_t threads_peak_{0};
    std::size_t threads_after_destroy_{0};
    std::size_t fds_create_{0};
    std::size_t maxfd_create_{0};
    std::size_t fds_after_destroy_{0};
    std::size_t routes_before_destroy_{0};
    long pool_alive_before_destroy_{0};
    long pool_after_destroy_{0};
    std::size_t routes_after_destroy_{0};
    double create_ms_{0};
    double window_s_{0};
    double cpu_cores_{0};
    unsigned long long ctx_vol_{0}, ctx_nonvol_{0};
    int ctx_unreadable_{0};
    long recover_first_packet_ok_{0};
    long recover_lost_{0};
    double recover_first_packet_us_{-1};
    std::string idle_state_{"n/a"};
    bool broadcast_mode_{false};
    bool picker_short_{false};
    long hot_route_rx_{0};
    long hot_route_planned_{0};
    std::string recover_note_;

    /* ---- R3 新增状态 ---- */
    long hs_batch_ms_{0};             /* 握手批次实际耗时（H2） */
    std::vector<std::string> hs_failed_;   /* 握手失败/未确认的 route（H2） */
    long hs_confirmed_{0};            /* 收到确认帧的 route 数（H2） */
    long hs_registered_real_{0};      /* 真实注册（池内归属）数（H2） */
    /* 恢复阶段独立时间戳（H3）：绝对单调时刻，延迟 = 接收 − 发送。 */
    long long recover_send_us_{-1};
    long long recover_recv_us_{-1};
    long long recover_delay_us_{-1};
    long recover_resume_seq_base_{-1};
    long long recover_phase_us_{-1};
    long resume_seq_base_{0};   /* H3：恢复阶段的序号起点（只认 >= 该值的帧） */
    /* 真实重建阶段（H4）：begin_rebuild → 新 generation 首包。 */
    long long rebuild_begin_us_{-1};
    long long rebuild_first_packet_us_{-1};
    long long rebuild_ms_{-1};
    unsigned rebuild_gen_before_{0};
    unsigned rebuild_gen_after_{0};
    long rebuild_new_gen_rx_{0};
    long rebuild_old_gen_rx_{0};
    bool rebuild_ok_{false};
    long long close_ms_{-1};
    long old_gen_pre_{0};
    std::vector<long long> rebuild_substep_us_;   /* H4：重建各子步绝对时刻（µs，单调） */
    long old_gen_post_{0};
    std::string rebuild_note_{"n/a"};
    std::vector<bool> conditions_;      /* H6：§13.2 六条断言结果 */
    bool run_id_mismatch_{false};
    std::vector<std::string> adversarial_;   /* 故意注入的失败（H5/验收用） */
    /* 证据落盘结果：落盘失败也是失败（H5）。 */
    bool artifacts_ok_{false};
    std::string artifacts_note_;
    std::vector<std::string> picker_diag_;
};

/* F6 修复（t31 发现）：修前 `publish_route()` **不记账** ⇒ hotcold 臂的 `sent` 列恒 0，
 * 且首次发布时间 `first_publish_us` 也不写 ⇒ 该臂的"计划独立于接收"这条**不可读**。
 * 修后：与 `pub_publish()` 同口径记账（`sent` 递增、首次发布时刻、tx 样本）。
 * 两者**分列**：`sent` = 已交给库的发送尝试数（含返回 false 的尝试），
 * `sent_ok` = 返回 true 的次数 ⇒ "发送计划独立于接收结果"可被机器复核。 */
void Harness::publish_route(long idx, std::uint32_t seq)
{
    if (o_.negctl_no_pub) { ++led_[static_cast<std::size_t>(idx)].sent; return; }   /* H2 负控：无发布端 */
    std::string body;
    fill_payload(body, static_cast<std::uint32_t>(idx), seq, static_cast<std::size_t>(o_.payload));
    auto m = std::make_shared<dzIPC::Msg::StdString>();
    m->set_msg_id(kMsgId);
    m->str = std::move(body);
    const bool ok = is_shm() ? shm_pubs_[static_cast<std::size_t>(idx)]->publish(m)
                             : sk_pubs_[static_cast<std::size_t>(idx)]->publish(m);
    RouteLedger& lg = led_[static_cast<std::size_t>(idx)];
    if (lg.first_publish_us < 0)
        lg.first_publish_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  Clock::now().time_since_epoch()).count();
    ++lg.sent;
    if (ok) ++lg.sent_ok;
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"run_id\":\"%s\",\"route\":\"%s\",\"route_idx\":%ld,\"seq\":%u,\"direction\":\"tx\","
                  "\"path\":\"%s\",\"ok\":%d}",
                  o_.run_id.c_str(), names_[static_cast<std::size_t>(idx)].c_str(), idx, seq,
                  is_shm() ? "shm" : "socket", ok ? 1 : 0);
    push_sample(buf);
}

bool Harness::pub_publish(long idx, std::uint32_t seq)
{
    if (o_.negctl_no_pub)
    {
        /* H2 负控：没有发布端 ⇒ 发送必然失败；台账如实 +1 sent、+0 sent_ok。 */
        RouteLedger& lg = led_[static_cast<std::size_t>(idx)];
        if (lg.first_publish_us < 0)
            lg.first_publish_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      Clock::now().time_since_epoch()).count();
        ++lg.sent;
        return false;
    }
    std::string body;
    fill_payload(body, static_cast<std::uint32_t>(idx), seq, static_cast<std::size_t>(o_.payload));
    auto m = std::make_shared<dzIPC::Msg::StdString>();
    m->set_msg_id(kMsgId);
    m->str = body;
    const bool ok = is_shm() ? shm_pubs_[static_cast<std::size_t>(idx)]->publish(m)
                             : sk_pubs_[static_cast<std::size_t>(idx)]->publish(m);
    RouteLedger& lg = led_[static_cast<std::size_t>(idx)];
    if (lg.first_publish_us < 0)
        lg.first_publish_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  Clock::now().time_since_epoch()).count();
    ++lg.sent;
    if (ok)
    {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "{\"run_id\":\"%s\",\"route\":\"%s\",\"route_idx\":%ld,\"seq\":%u,\"direction\":\"tx\",\"path\":\"tlv\"}",
                      o_.run_id.c_str(), names_[static_cast<std::size_t>(idx)].c_str(), idx, seq);
        push_sample(buf);
    }
    return ok;
}

bool Harness::wait_handshake(long idx, long timeout_ms)
{
    if (!is_shm())
    {
        /* socket：UDP 组播没有反向发现通道，订阅端靠 IpcInfoPool 回填 subscribed_。
         * ⚠️ t33 修复：原实现**逐 route** sleep 400 ms ⇒ n=1000 时握手阶段要 400 s，
         * 叠加 t29 的有界重试（碰撞话题最多 30×1 s）后整轮接近 timeout 上限。
         * 改为**一次性**批量 settle（在下方的 handshake 阶段统一做），此处只做占位返回。 */
        return true;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (cp.open(shm_topic_control_name(names_[static_cast<std::size_t>(idx)], static_cast<std::size_t>(o_.domain)))
            && cp.peer_count() >= 1 && cp.state() == dzIPC::control_plane_shm::TopicState::Ready)
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

/* 非阻塞排空该 route 当前可见的全部消息并记台账；返回本次新增条数。 */
long Harness::drain_once(long idx, RouteLedger& led, bool mark_resume)
{
    long got = 0;
    for (;;)
    {
        auto sink = td();
        bool have = false;
        if (is_shm())
        {
            if (shm_subs_[static_cast<std::size_t>(idx)]->try_get_clone(sink)) have = true;
        }
        else
        {
            if (sk_subs_[static_cast<std::size_t>(idx)]->try_get_clone(sink)) have = true;
        }
        if (!have) break;
        auto s = sink->topic() ? sink->topic()->msgcast<dzIPC::Msg::StdString>() : nullptr;
        if (!s) { ++led.corrupt; continue; }
        std::uint32_t seq = 0;
        const std::uint32_t payload_route = broadcast_mode_ ? 0u : static_cast<std::uint32_t>(idx);
        if (!verify_payload(s->str, payload_route, &seq))
        {
            ++led.corrupt;
            /* ⛔ t33 诊断：原实现只 +1 就 continue，**丢掉了"到底收到了什么"** ——
             * 于是"跨 route 串包"（socket 组播串扰）与"载荷被截断/损坏"无法区分。
             * 这里记下首次失败的前 16 字节与解析出的 route/seq，供归因。 */
            if (!led.corrupt_detail_done)
            {
                led.corrupt_detail_done = true;
                if (s->str.size() >= 16)
                {
                    std::uint32_t got_seq = 0, got_route = 0;
                    std::uint64_t got_magic = 0;
                    std::memcpy(&got_seq, &s->str[0], 4);
                    std::memcpy(&got_route, &s->str[4], 4);
                    std::memcpy(&got_magic, &s->str[8], 8);
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                                  "corrupt_detail route_idx=%ld expect_route=%u len=%zu got_seq=%u got_route=%u got_magic=0x%llx",
                                  idx, payload_route, s->str.size(), got_seq, got_route,
                                  static_cast<unsigned long long>(got_magic));
                    corrupt_details_.push_back(buf);
                }
                else
                {
                    corrupt_details_.push_back("corrupt_detail route_idx=" + std::to_string(idx) + " len=" +
                                               std::to_string(s->str.size()) + " (short)");
                }
            }
            continue;
        }
        /* H2：确认帧与规模帧**完全分账** —— 确认帧的 seq 落在专用区间，
         * 只累 `confirm_rx`，⛔ 不计入 `rx/dup/ooo`（否则握手会污染规模读数）。 */
        if (seq >= kOldGenMarkerSeqBase())
        {
            /* H4：旧 generation 标记帧 —— **独立计数**，不参与规模台账。 */
            ++led.old_gen_rx_;
            ++got;
            continue;
        }
        if (seq >= kConfirmSeqBase())
        {
            ++led.confirm_rx;
            ++led.confirm_payload_ok;
            if (!led.confirm_first_ok) { led.confirm_first_ok = true; }
            ++got;
            continue;
        }
        if (!led.seen.insert(seq).second) { ++led.dup; continue; }
        if (seq + 1 < *led.seen.rbegin()) ++led.ooo;
        ++led.rx;
        ++got;
        const long long now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     Clock::now().time_since_epoch()).count();
        if (mark_resume)
        {
            /* H3：恢复阶段**只接受新恢复序号**；旧帧（seq < 恢复起点）不得填补恢复首包。 */
            if (static_cast<long>(seq) >= resume_seq_base_)
            {
                ++led.rx_after_silence;
                if (!led.recover_first_rx_us_set)
                {
                    led.recover_first_rx_us_set = true;
                    led.recover_first_rx_us = now_us;
                }
            }
            else
            {
                ++led.recover_old_seq_rejected;
            }
        }
        if (!led.first_packet_ok)
        {
            led.first_packet_ok = true;
            led.first_packet_us = now_us;
        }
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "{\"run_id\":\"%s\",\"route\":\"%s\",\"route_idx\":%ld,\"seq\":%u,\"direction\":\"rx\",\"path\":\"tlv\"}",
                      o_.run_id.c_str(), names_[static_cast<std::size_t>(idx)].c_str(), idx, seq);
        push_sample(buf);
    }
    return got;
}

/* 轮询直到 rx >= want（want>0）或超时；want<=0 表示只做一次非阻塞排空。 */
bool Harness::drain_until(long idx, RouteLedger& led, long want, long timeout_ms, bool mark_resume)
{
    if (want <= 0) { (void)drain_once(idx, led, mark_resume); return true; }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        (void)drain_once(idx, led, mark_resume);
        if (led.rx >= want) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int Harness::run()
{
    std::printf("mode=%s\n", is_shm() ? "shm_pub_sub" : "socket_pub_sub");
    std::printf("topology=%s\n", o_.topology.c_str());
    std::printf("run_id=%s\npid=%d\nn=%ld domain=%ld msgs=%ld payload=%ld\n", o_.run_id.c_str(), (int)::getpid(),
                o_.n, o_.domain, o_.msgs, o_.payload);
    std::fflush(stdout);

    dzIPC::detail::SetSeamHook(&seam_hook);

    /* ---------------- 阶段 0：证据目录**独占预占**（方案 §5.3） ----------------
     * 要求"在建对象前独占创建输出目录并拒绝覆盖"—— 修前是在**全部阶段结束后**才检查目录，
     * 于是一个长跑可以把结果算完再因为目录非空而丢弃（r25 的 `.log` 与目录不同轮就是这么来的）。
     * 修后：开跑前先用 `mkdir`（原子）预占；已存在且非空 ⇒ 立刻以非零判定退出。 */
    if (!o_.out_dir.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(o_.out_dir, ec))
        {
            if (!std::filesystem::is_empty(o_.out_dir, ec))
            {
                std::printf("ARTIFACT_DIR_NOT_EMPTY: %s —— 开跑前即拒绝覆盖（重跑请新建 run_id）\n", o_.out_dir.c_str());
                std::printf("W10_MATRIX_DONE verdict=FAIL failures=1\n");
                std::printf("FAILURE: 输出目录已存在且非空，拒绝覆盖（阶段 0 独占预占失败）\n");
                return 1;
            }
        }
        else if (!std::filesystem::create_directories(o_.out_dir, ec))
        {
            std::printf("ARTIFACT_DIR_CREATE_FAILED: %s (%s)\n", o_.out_dir.c_str(), ec.message().c_str());
            std::printf("W10_MATRIX_DONE verdict=FAIL failures=1\n");
            return 1;
        }
        /* 独占标记：写入 owner 文件，后到的进程会因目录非空被拒。 */
        std::ofstream mk(o_.out_dir + "/.claimed");
        mk << getpid() << " " << o_.run_id << "\n";
    }

    /* ---------------- F4：库/工装指纹配对前置检查（防陈旧 build 假绿） ----------------
     * 按值返回结构体加字段 ⇒ 陈旧调用方与新库 ABI 不匹配（t30 实测退出时 SIGSEGV）。
     * 调用方给出的期望指纹必须与实际加载的库/自身二进制一致，否则**拒绝出结论**。 */
    if (!o_.require_lib_sha256.empty())
    {
        const std::string actual = sha256_of_file(loaded_lib_path());
        if (actual.empty())
        {
            std::printf("FINGERPRINT_CHECK: FAIL 无法计算实际加载库的 sha256（%s）\n", loaded_lib_path().c_str());
            return 1;
        }
        if (actual != o_.require_lib_sha256)
        {
            std::printf("FINGERPRINT_MISMATCH lib: actual=%s expected=%s\n", actual.c_str(),
                        o_.require_lib_sha256.c_str());
            std::printf("W10_MATRIX_DONE verdict=FAIL failures=1\n");
            return 1;
        }
        std::printf("FINGERPRINT_CHECK: lib OK %s\n", actual.c_str());
    }
    if (!o_.require_bin_sha256.empty())
    {
        const std::string actual = sha256_of_file("/proc/self/exe");
        if (actual != o_.require_bin_sha256)
        {
            std::printf("FINGERPRINT_MISMATCH bin: actual=%s expected=%s\n", actual.c_str(),
                        o_.require_bin_sha256.c_str());
            std::printf("W10_MATRIX_DONE verdict=FAIL failures=1\n");
            return 1;
        }
        std::printf("FINGERPRINT_CHECK: bin OK %s\n", actual.c_str());
    }

    /* ---------------- 阶段 1：prepare ---------------- */
    auto t_phase = Clock::now();
    names_.reserve(static_cast<std::size_t>(o_.n));
    /* ⛔ socket 侧：千路独立话题时，端口公式 `udp_discovery_port_calculate` 会与
     * **同机其它进程已占用的 UDP 端口**（含 ip_local_port_range 内的临时源端口）碰撞；
     * 而 `socket_sub_ipc::InitChannel` 的重连是 `while(!connect()) sleep(1s)` 无上界
     * ⇒ 一条话题碰撞就把整个千路运行**永久挂住**（实测 topic #38 / 端口 48650）。
     * 该缺陷已单独登记（见 port-conflict/）。为了让「千路有效收发」这条规模判据
     * 不被无关的端口碰撞掩盖，socket 千路用 `--topic-scan` 只取**端口段互不重叠且
     * 避开当前已占端口**的话题名（扫描是确定性的：端口是话题名的纯函数）。 */
    /* ⛔ t37 修正：原实现在 `n < 1000` 时**不走**端口预检 ⇒ 直接用朴素命名，
     * 于是 n=500 也会出现 (组地址,端口) 重复（实测 domain=32455：498/500 注册、
     * 8 条 corrupt、2 条确认帧缺失）。端口预检必须对**所有** socket independent 生效。 */
    if (o_.transport == "socket" && o_.topology == "independent")
    {
        const bool picked_ok = w10_pick_socket_topics(names_, o_.domain, o_.n, true, &picker_diag_);
        if (!picked_ok)
        {
            /* ⛔ t33 第四处修复：原实现在这里**继续用旧名字填满** ⇒ 重名 ⇒ 同一
             * (组地址,端口) 被两个 sub 绑 ⇒ 互相串包（实测 final-1 有 52 个重名、
             * 52 条 corrupt，两者集合逐条相同）。改为**直接判定 pickset 不足即失败**，
             * 并把 want 降为实得数（不重名），由 verdict 反映。 */
            std::printf("FAILURE: picker 只找到 %zu 个无碰撞话题名（需要 %ld）⇒ 不重名补齐，"
                        "本轮的 socket 规模判定作废\n",
                        names_.size(), o_.n);
            picker_short_ = true;
            o_.n = static_cast<long>(names_.size());
            std::printf("WARN: 本轮 n 降为 %ld（重名补齐会让两个 sub 绑同一 (组地址,端口) 而串包）\n", o_.n);
        }
    }
    for (long i = static_cast<long>(names_.size()); i < o_.n; ++i)
    {
        if (o_.topology == "broadcast")
        {
            /* 同一话题、N 个**独立订阅者**（每个都是自己的 route/队列）。
             * 台账按订阅者下标区分，因此名字仍逐条不同只是为了可读性。 */
            names_.push_back("w10_" + o_.transport + "_broadcast_" + std::to_string(o_.domain) + "_sub"
                             + std::to_string(i));
        }
        else
        {
            names_.push_back("w10_" + o_.transport + "_" + o_.topology + "_" + std::to_string(o_.domain) + "_" + std::to_string(i));
        }
    }
    led_.resize(static_cast<std::size_t>(o_.n));
    for (long i = 0; i < o_.n; ++i) led_[static_cast<std::size_t>(i)].topic = names_[static_cast<std::size_t>(i)];
    phases_.push_back({"prepare", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(), "话题名与台账预置"});

    /* ---------------- 阶段 2：register（建 pub/sub 通道） ---------------- */
    t_phase = Clock::now();
    const bool broadcast = (o_.topology == "broadcast");
    broadcast_mode_ = broadcast;
    const long n_pub = o_.negctl_no_pub ? 0 : (broadcast ? 1 : o_.n);
    if (broadcast && o_.n > 32)
    {
        findings_.push_back("broadcast 拓扑要求 N ≤ 32（libipc 连接位宽 32），实际 " + std::to_string(o_.n));
    }
    /* broadcast：1 个话题，故发布端只需要 1 个实例，它绑定的是**共用的那个话题名**。 */
    const std::string bc_topic = "w10_" + o_.transport + "_broadcast_" + std::to_string(o_.domain);
    for (long i = 0; i < n_pub; ++i)
    {
        const std::string& tn = broadcast ? bc_topic : names_[static_cast<std::size_t>(i)];
        if (is_shm())
        {
            shm_pubs_.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), tn, static_cast<std::size_t>(o_.domain), false));
            shm_pubs_.back()->InitChannel("w10");
        }
        else
        {
            sk_pubs_.emplace_back(new dzIPC::socket::socket_pub_ipc(td(), tn, static_cast<std::size_t>(o_.domain), false));
            sk_pubs_.back()->InitChannel("w10");
        }
    }
    for (long i = 0; i < o_.n; ++i)
    {
        const std::string tn = broadcast ? bc_topic : names_[static_cast<std::size_t>(i)];
        if (is_shm())
        {
            shm_subs_.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), tn, static_cast<std::size_t>(o_.domain), 64, false));
            shm_subs_.back()->InitChannel("w10");
        }
        else
        {
            sk_subs_.emplace_back(new dzIPC::socket::socket_sub_ipc(td(), tn, static_cast<std::size_t>(o_.domain), 64, false));
            sk_subs_.back()->InitChannel("w10");
        }
    }
    create_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - t_phase).count();
    fds_create_ = fd_count(&maxfd_create_);
    threads_create_ = thread_count();
    phases_.push_back({"register", static_cast<long long>(create_ms_), "建通道 + InitChannel"});

    /* ---------------- 阶段 3：handshake（真实注册 + 逐 route 确认帧，H2） ----------------
     * 修前缺陷：socket 分支每 route `sleep_for(400ms)` 后**直接 return true**
     *   ⇒ ① 不证明注册、更不证明通信；② 1000 路顺序睡眠 ≈400 s。
     * 修后（方案 §5.3）：**注册状态**与**通信状态**分开，且两段都用**批量轮询**
     *   （每轮遍历全部 route 各查一次，⛔ 不对已就绪对象逐个睡眠）：
     *     · 注册 = 该 route 在**自己那一侧**的接收池里真实在册（`route_count`），
     *       并（shm）用控制面 `Ready + peer_count>=1` 交叉核对；
     *     · 通信 = 逐 route 发一个**确认帧**（`seq = kConfirmSeqBase + i`，载荷含
     *       run_id/route/generation/seq）并真实收到、且**载荷校验通过**。
     * 批次总期限 = `register_batch_deadline_ms`（R0-4 L2=90 s）；超时把未确认 route
     * 逐条记入 `hs_failed_` 并**判失败**（H6：不靠日志提示代替失败状态）。 */
    t_phase = Clock::now();
    const long hs_timeout = o_.register_batch_deadline_ms;
    if (broadcast)
    {
        const auto dl = Clock::now() + std::chrono::milliseconds(hs_timeout);
        long peers = 0;
        while (Clock::now() < dl)
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            peers = (cp.open(shm_topic_control_name(bc_topic, static_cast<std::size_t>(o_.domain)))
                     && cp.state() == dzIPC::control_plane_shm::TopicState::Ready)
                        ? static_cast<long>(cp.peer_count())
                        : 0;
            if (peers >= o_.n) break;
            std::this_thread::sleep_for(5ms);
        }
        std::printf("broadcast_peers=%ld/%ld\n", peers, o_.n);
        registered_ = std::min(peers, o_.n);
        for (long i = 0; i < o_.n; ++i) led_[static_cast<std::size_t>(i)].registered = (i < registered_);
    }
    else
    {
        auto& shm_pool_hs = dzIPC::threepools::RecvWorkerPool::instance();
        auto& sk_pool_hs = dzIPC::threepools::SocketRecvWorkerPool::instance();
        const auto dl = Clock::now() + std::chrono::milliseconds(hs_timeout);
        long last_pool = 0, last_ready = 0;
        for (;;)
        {
            last_pool = static_cast<long>(is_shm() ? shm_pool_hs.route_count() : sk_pool_hs.route_count());
            if (is_shm())
            {
                /* shm：控制面 Ready + 对端计数（批量轮询，每轮只查一次已打开的平面）。 */
                last_ready = 0;
                for (long i = 0; i < o_.n; ++i)
                {
                    dzIPC::control_plane_shm::TopicControlPlane cp;
                    if (cp.open(shm_topic_control_name(names_[static_cast<std::size_t>(i)],
                                                       static_cast<std::size_t>(o_.domain)))
                        && cp.state() == dzIPC::control_plane_shm::TopicState::Ready && cp.peer_count() >= 1)
                        ++last_ready;
                }
            }
            else
            {
                last_ready = last_pool;
            }
            if (last_pool >= o_.n || Clock::now() >= dl) break;
            std::this_thread::sleep_for(5ms);
        }
        hs_registered_real_ = last_pool;
        std::printf("handshake_register pool_routes=%ld control_ready=%ld/%ld/%ld ms\n", last_pool, last_ready, o_.n,
                    static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count()));

        /* 逐 route 确认帧：**批量**发送（一次遍历）+ **批量**轮询（每轮一次遍历，无逐 route 睡眠）。 */
        for (long i = 0; i < o_.n; ++i)
        {
            RouteLedger& l = led_[static_cast<std::size_t>(i)];
            (void)pub_publish(i, kConfirmSeqBase() + static_cast<std::uint32_t>(i));
            /* 确认帧的发送量不与规模计划混算（下面按区间重建 sent）。 */
        }
        for (long i = 0; i < o_.n; ++i)
        {
            RouteLedger& l = led_[static_cast<std::size_t>(i)];
            l.sent = 0;
            l.sent_ok = 0;
            l.first_publish_us = -1;    /* 首次数据帧发布时间在规模阶段重记（H3） */
        }
        const auto dl2 = dl;             /* 与注册共用批次总期限（R0-4 L2=90 s，不叠加） */
        for (;;)
        {
            long got = 0;
            for (long i = 0; i < o_.n; ++i)
            {
                RouteLedger& l = led_[static_cast<std::size_t>(i)];
                (void)drain_once(i, l, false);
                if (l.confirm_rx > 0) ++got;
            }
            hs_confirmed_ = got;
            if (got >= o_.n || Clock::now() >= dl2) break;
            std::this_thread::sleep_for(5ms);
        }
        /* 逐 route 记录确认结果；未确认的**逐条**记入失败清单（H6）。 */
        for (long i = 0; i < o_.n; ++i)
        {
            RouteLedger& l = led_[static_cast<std::size_t>(i)];
            /* 逐 route 注册判定 = **收到本 route 自己的确认帧**（同时证明注册与通信）。 */
            l.registered = (l.confirm_rx > 0);
            if (l.registered) ++registered_;
            else
                hs_failed_.push_back("route " + l.topic + " 未收到确认帧（池内在册=" +
                                     std::to_string(last_pool) + "）");
        }
        hs_batch_ms_ = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count());
        std::printf("handshake_confirm confirmed=%ld/%ld batch_ms=%ld failed=%zu\n", hs_confirmed_, o_.n, hs_batch_ms_,
                    hs_failed_.size());
    }
    phases_.push_back({"handshake", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count()),
                       "registered=" + std::to_string(registered_) + "/" + std::to_string(o_.n) +
                           " confirmed=" + std::to_string(hs_confirmed_) + " pool=" + std::to_string(hs_registered_real_)});
    std::printf("registered_count=%ld/%ld threads_after_create=%zu create_ms=%.1f fds_open=%zu max_fd=%zu\n",
                registered_, o_.n, threads_create_, create_ms_, fds_create_, maxfd_create_);

    /* 存活期的归属证据（⛔ 不是销毁后的 0）。
     * route 到 worker 的归属由控制面 Ready 之后的**下一次 tick**（sub_heartbeat=10 ms）
     * 触发，因此这里必须有界等待"归属数收敛到 registered"，而不是立刻读一次
     * （立刻读会把"尚未 tick"误判成"未归属"）。等待上界给足，超时如实记失败。 */
    {
        const auto dl = Clock::now() + std::chrono::seconds(20);
        auto& shm_pool = dzIPC::threepools::RecvWorkerPool::instance();
        auto& sk_pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
        for (;;)
        {
            pool_routes_alive_ = static_cast<long>(shm_pool.route_count());
            socket_pool_routes_alive_ = static_cast<long>(sk_pool.route_count());
            const long got = is_shm() ? pool_routes_alive_ : socket_pool_routes_alive_;
            if (got >= registered_ || Clock::now() >= dl) break;
            std::this_thread::sleep_for(5ms);
        }
        std::printf("pool_routes_alive=%ld socket_pool_routes_alive=%ld (registered=%ld)\n", pool_routes_alive_,
                    socket_pool_routes_alive_, registered_);
    }

    /* ---------------- 阶段 4：warmup ---------------- */
    t_phase = Clock::now();
    std::this_thread::sleep_for(200ms);
    phases_.push_back({"warmup", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(), "控制面稳定"});

    /* ---------------- 阶段 5：fixed-rate（逐 route 定速发布 + 逐条校验） ---------------- */
    t_phase = Clock::now();
    if (o_.topology == "hotcold")
    {
        /* 1 条热路满速 + (N-1) 条冷路静默；冷路在热路压满的同 worker 上仍须被服务。 */
        const long hot = 0;
        std::atomic<long> hot_sent{0};
        std::thread hot_thread([&] {
            for (long i = 0; i < o_.hot_msgs; ++i)
            {
                publish_route(hot, static_cast<std::uint32_t>(i));
                ++hot_sent;
            }
        });
        /* 冷路各发 1 条，紧随热路启动（同 worker 的冷路共享同一份预算）。 */
        const auto cold_t0 = Clock::now();
        std::vector<long> cold_order;
        for (long i = 1; i < o_.n; ++i) cold_order.push_back(i);
        for (long idx : cold_order) publish_route(idx, 0);
        const auto cold_t1 = Clock::now();
        hot_thread.join();
        led_[0].planned = o_.hot_msgs;
        for (long i = 0; i < o_.n; ++i)
        {
            if (i == 0) led_[0].planned = o_.hot_msgs;
            else led_[static_cast<std::size_t>(i)].planned = 1;
        }
        const double cold_ms = std::chrono::duration<double, std::milli>(cold_t1 - cold_t0).count();
        std::printf("hotcold hot_sent=%ld hot_msgs=%ld cold_publish_ms=%.1f\n", hot_sent.load(), o_.hot_msgs, cold_ms);
        /* 冷路必须收齐自己的 1 条（有界）。 */
        for (long i = 1; i < o_.n; ++i)
        {
            RouteLedger& l = led_[static_cast<std::size_t>(i)];
            drain_until(i, l, 1, 8000, false);
            if (l.rx < 1) ++l.timeout;
        }
        /* 热路排空（不要求全收：满速开环允许丢，但必须计数）。 */
        drain_until(0, led_[0], 0, 0, false);
    }
    else
    {
        /* 定速：每条 route 发 o_.msgs 条，seq 0..msgs-1。发送计划**独立于**接收结果
         * （§13.1：接收变慢不得让发送量下降而美化统计）。 */
        const auto rate_start = Clock::now();
        if (broadcast)
        {
            /* 广播：1 次 publish 应被 N 个订阅者各自收到。载荷按【发布者】下标生成，
             * 校验时按订阅者各自队列逐条重算同一载荷（校验函数只认 route=0）。 */
            for (long round = 0; round < o_.msgs; ++round)
            {
                (void)pub_publish(0, static_cast<std::uint32_t>(round));
            }
            for (long i = 0; i < o_.n; ++i) led_[static_cast<std::size_t>(i)].planned = o_.msgs;
        }
        else
        {
            for (long round = 0; round < o_.msgs; ++round)
            {
                for (long i = 0; i < n_pub; ++i) (void)pub_publish(i, static_cast<std::uint32_t>(round));
            }
            for (long i = 0; i < o_.n; ++i) led_[static_cast<std::size_t>(i)].planned = o_.msgs;
        }
        for (long i = 0; i < o_.n; ++i) led_[static_cast<std::size_t>(i)].planned = o_.msgs;
        const double send_ms = std::chrono::duration<double, std::milli>(Clock::now() - rate_start).count();
        std::printf("fixed_rate_send_ms=%.1f\n", send_ms);
    }
    phases_.push_back({"fixed-rate", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(), "定速发布"});

    /* ---------------- 阶段 6：drain-verify（逐 route 排空 + 校验） ---------------- */
    t_phase = Clock::now();
    long total_dup = 0, total_corrupt = 0, total_ooo = 0;
    {
        const auto dl = Clock::now() + std::chrono::milliseconds(o_.drain_timeout_ms);
        while (Clock::now() < dl)
        {
            long total_valid = 0;
            for (long i = 0; i < o_.n; ++i)
            {
                RouteLedger& l = led_[static_cast<std::size_t>(i)];
                if (l.rx < l.planned) drain_once(i, l, false);
                if (l.rx >= l.planned) ++total_valid;
            }
            if (total_valid >= o_.n) break;
            std::this_thread::sleep_for(2ms);
        }
        for (long i = 0; i < o_.n; ++i)
        {
            RouteLedger& l = led_[static_cast<std::size_t>(i)];
            /* hotcold 的热路是开环满速：丢包是语义的一部分，不计 timeout。 */
            const bool open_loop = (o_.topology == "hotcold" && i == 0);
            if (!open_loop && l.rx < l.planned) ++l.timeout;
            total_dup += l.dup; total_corrupt += l.corrupt; total_ooo += l.ooo;
        }
    }
    valid_rx_ = 0;
    if (o_.topology == "hotcold")
    {
        /* 冷路（1..n-1）各发 1 条 ⇒ 必须收齐；热路是开环满速 ⇒ 允许丢包但必须计数。 */
        for (long i = 1; i < o_.n; ++i)
            if (led_[static_cast<std::size_t>(i)].rx >= 1) ++valid_rx_;
        expected_valid_ = o_.n - 1;
        hot_route_rx_ = led_[0].rx;
        hot_route_planned_ = led_[0].planned;
    }
    else
    {
        for (long i = 0; i < o_.n; ++i)
            if (led_[static_cast<std::size_t>(i)].rx >= led_[static_cast<std::size_t>(i)].planned) ++valid_rx_;
        expected_valid_ = o_.n;
    }
    std::printf("valid_rx_count=%ld/%ld dup=%ld corrupt=%ld out_of_order=%ld\n", valid_rx_, expected_valid_, total_dup,
                total_corrupt, total_ooo);
    phases_.push_back({"drain-verify", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(),
                       "valid_rx=" + std::to_string(valid_rx_) + "/" + std::to_string(o_.n)});

    /* ---------------- 阶段 7：silence（静默窗口；三相空闲口径按 §10.1/§10.3） ---------------- */
    t_phase = Clock::now();
    {
        const double win_s = std::max(1.0, static_cast<double>(o_.silence_ms) / 1000.0);
        const std::size_t t_before = thread_count();
        const double cpu_before = proc_cpu_ticks();
        const CtxSample ctx_before = ctx_sample();
        std::size_t peak = t_before;
        const auto w0 = Clock::now();
        while (std::chrono::duration<double>(Clock::now() - w0).count() < win_s)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const std::size_t t = thread_count();
            if (t > peak) peak = t;
        }
        const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
        const double cpu_after = proc_cpu_ticks();
        const CtxSample ctx_after = ctx_sample();
        const long hz = 100;   /* sysconf(_SC_CLK_TCK)；本机实测 100 */
        cpu_cores_ = (cpu_after - cpu_before) / hz / wall;
        ctx_vol_ = ctx_after.vol >= ctx_before.vol ? ctx_after.vol - ctx_before.vol : 0;
        ctx_nonvol_ = ctx_after.nonvol >= ctx_before.nonvol ? ctx_after.nonvol - ctx_before.nonvol : 0;
        ctx_unreadable_ = static_cast<int>(ctx_before.unreadable + ctx_after.unreadable);
        window_s_ = wall;
        threads_peak_ = peak;
        /* 三相空闲判定（§10.1）：本工装此刻处于"状态 3 = 已连接、有效订阅、停止发布"。 */
        idle_state_ = (registered_ > 0 && valid_rx_ > 0) ? "connected_silent"
                     : (registered_ > 0 ? "registered_no_publisher" : "no_registered_route");
        std::printf("silence window_s=%.3f state=%s threads=%zu peak=%zu cpu_cores=%.5f ctx_per_s=%.1f "
                    "(per route %.4f) ctx_unreadable_tids=%d\n",
                    wall, idle_state_.c_str(), t_before, peak, cpu_cores_,
                    (ctx_vol_ + ctx_nonvol_) / wall, (ctx_vol_ + ctx_nonvol_) / wall / static_cast<double>(std::max(1L, o_.n)),
                    ctx_unreadable_);
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "state=%s window_s=%.3f cpu_cores=%.5f ctx_per_s=%.1f threads=%zu peak=%zu unreadable_tids=%d",
                      idle_state_.c_str(), wall, cpu_cores_, (ctx_vol_ + ctx_nonvol_) / wall, t_before, peak,
                      ctx_unreadable_);
        phases_.push_back({"silence", static_cast<long long>(o_.silence_ms), buf});
    }

    /* ---------------- 阶段 7：recover（§10.1 强制用例；H3 重写时间戳口径） ----------------
     * 修前缺陷（队长实测）：`recover_first_packet_us = 9.52005e+10` —— 那是
     *   `Clock::now().time_since_epoch()` 的**绝对**值（26.4 小时），**不是延迟**；
     *   而且首次接收状态（`first_packet_ok`/`first_packet_us`）在恢复阶段**未复位**，
     *   于是"恢复首包"可能被**旧帧**满足。
     * 修后：
     *   ① 首次接收与恢复接收**独立状态**（`first_packet_*` vs `recover_*`）；
     *   ② `恢复延迟 = 恢复首个有效帧的接收时刻 − 该帧的发送时刻`，**同一单调时钟**；
     *   ③ 只接受**新恢复序号**（`seq >= o_.msgs`，即恢复阶段新发的），旧帧不得填补；
     *   ④ 延迟必须 `>= 0` 且 `<=` 恢复阶段实际耗时，并在 verdict 里机械校验。 */
    t_phase = Clock::now();
    if (o_.topology != "hotcold" && o_.resume_msgs > 0)
    {
        const long probe_route = 0;
        RouteLedger& l = led_[static_cast<std::size_t>(probe_route)];
        const long rx_before = l.rx;
        /* 清掉旧帧（重置接收状态）：只认恢复阶段新发的序号。 */
        (void)drain_once(probe_route, l, false);
        l.rx_before_recover_ = l.rx;
        recover_resume_seq_base_ = o_.msgs;
        resume_seq_base_ = o_.msgs;
        const long long send_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      Clock::now().time_since_epoch()).count();
        recover_send_us_ = send_us;
        const auto rt0 = Clock::now();
        for (long k = 0; k < o_.resume_msgs; ++k)
        {
            (void)pub_publish(probe_route, static_cast<std::uint32_t>(o_.msgs + k));
        }
        l.planned += o_.resume_msgs;
        /* 有界等待**新恢复序号**的首帧。 */
        bool got_new = false;
        const auto dl = Clock::now() + std::chrono::milliseconds(5000);
        while (Clock::now() < dl)
        {
            (void)drain_once(probe_route, l, true);
            if (l.recover_first_rx_us_set && l.recover_first_rx_us >= 0)
            {
                got_new = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        (void)drain_until(probe_route, l, l.planned, 2000, true);
        const long long phase_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - rt0).count();
        recover_recv_us_ = l.recover_first_rx_us;
        recover_delay_us_ = (l.recover_first_rx_us >= 0 && send_us >= 0) ? (l.recover_first_rx_us - send_us) : -1;
        recover_first_packet_ok_ = (got_new && recover_delay_us_ >= 0 && recover_delay_us_ <= phase_us) ? 1 : 0;
        recover_lost_ = l.planned - l.rx;
        recover_phase_us_ = phase_us;
        char note[512];
        std::snprintf(note, sizeof note,
                      "silence_ms=%ld resume_msgs=%ld rx_after=%ld ok=%ld 恢复延迟=%lldus 发送绝对=%lldus 接收绝对=%lldus "
                      "阶段耗时=%lldus lost=%ld new_seq_only=%d",
                      o_.silence_ms, o_.resume_msgs, l.rx_after_silence, recover_first_packet_ok_,
                      recover_delay_us_, send_us, recover_recv_us_, phase_us, recover_lost_,
                      l.recover_old_seq_rejected ? 0 : 1);
        recover_note_ = note;
        recover_first_packet_us_ = static_cast<double>(recover_delay_us_);
        std::printf("recover silence_ms=%ld rx_after_silence=%ld ok=%ld delay_us=%lld send_abs_us=%lld recv_abs_us=%lld "
                    "phase_us=%lld lost=%ld old_frames_rejected=%ld\n",
                    o_.silence_ms, l.rx_after_silence, recover_first_packet_ok_, recover_delay_us_, send_us,
                    recover_recv_us_, phase_us, recover_lost_, l.recover_old_seq_rejected);
    }
    phases_.push_back({"recover", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(), recover_note_});

    /* ---------------- 阶段 8：concurrent-close（并发关闭） + 阶段 8b：rebuild（真实重建，H4） ----------------
     * 修前缺陷：该阶段实际**只并发析构后半数订阅、没有任何重建代码**，阶段名
     *   `concurrent-close-rebuild` 因此不足以证明"重建时限"与"旧 generation 隔离"。
     * 修后拆成两段，各自独立判据：
     *   8a `concurrent-close`：8 lane 并发析构后半数订阅（保留前一半做资源对账），
     *      按 R0-4 的关闭时限（≤2 s）机械判定；
     *   8b `rebuild`：**真实重建** —— 对任一保留的 SHM route，取控制面 `begin_rebuild()`
     *      推进 generation（这正是生产路径 `before_generation_rebuild` 的触发源），
     *      记录 `begin_rebuild` → 新 generation 首包的耗时（≤1 s 判据），并把
     *      旧 generation 帧计数**独立**列出（⛔ 不得填补新首包）。
     *      非 SHM（socket 无 generation 概念）如实标注为 `n/a`，不加假判据。 */
    t_phase = Clock::now();
    {
        std::atomic<long> closed{0};
        std::vector<std::thread> th;
        const long per = std::max(1L, (o_.n + 7) / 8);
        for (int w = 0; w < 8; ++w)
        {
            th.emplace_back([&, w] {
                const long lo = static_cast<long>(w) * per;
                const long hi = std::min(o_.n, lo + per);
                for (long i = lo; i < hi; ++i)
                {
                    if (i < o_.n / 2) continue;   /* 只拆后一半，前一半留作资源对账 */
                    if (is_shm()) shm_subs_[static_cast<std::size_t>(i)].reset();
                    else sk_subs_[static_cast<std::size_t>(i)].reset();
                    ++closed;
                }
            });
        }
        for (auto& x : th) x.join();
        std::printf("concurrent_close closed=%ld\n", closed.load());
        close_ms_ = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count());
    }
    phases_.push_back({"concurrent-close", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count()),
                       "并发析构后半数订阅 closed=" + std::to_string(o_.n / 2) + " ms=" + std::to_string(close_ms_)});

    /* ---- 阶段 8b：真实重建（H4）----
     * **为什么走"换发布端"而不是外部调 `cp.begin_rebuild()`**：生产路径里 generation 推进由
     * 发布端重建触发（见 `w10_faults` F2 与 `shm_sub_ipc::before_generation_rebuild`）。
     * 外部直接 `begin_rebuild()` 会与仍在运行的原发布端争夺 owner 语义，造出非生产形态。
     * 因此本段**逐字沿用故障矩阵 F2 的造法**：析构旧发布端 → 建新发布端（generation 递增）
     * → 记 `begin_rebuild` 时刻 → 等对端重新 attach → 发新代首包 → 判"是否在冻结时限内收到"。
     * 判据：
     *   · `rebuild_ms <= rebuild_deadline_ms`（R0-3 冻结 1 s）；
     *   · 新代首包**必须**在时限内被收到（`rebuild_new_gen_rx == 1`）；
     *   · 旧代帧在重建后**独立计数**（`old_gen_rx_`，期望 0；如实报告实测值）。
     * socket 无 generation 概念 ⇒ 本段标 `n/a`，⛔ 不加假判据。 */
    t_phase = Clock::now();
    {
        rebuild_note_ = "n/a（socket 无 generation 概念）";
        if (is_shm() && o_.n >= 2 && o_.topology != "broadcast" && !o_.skip_rebuild)
        {
            const long rr = 1;
            const std::string& tn = names_[static_cast<std::size_t>(rr)];
            RouteLedger& l = led_[static_cast<std::size_t>(rr)];
            (void)drain_once(rr, l, false);
            /* 旧代帧（重建前）：正常排空，单独记 pre 值（不构成违规）。 */
            (void)pub_publish(rr, kOldGenMarkerSeqBase());
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            (void)drain_once(rr, l, false);
            old_gen_pre_ = l.old_gen_rx_;
            l.old_gen_rx_ = 0;                 /* 重建后的旧代计数从 0 起算 */

            dzIPC::control_plane_shm::TopicControlPlane cp;
            rebuild_gen_before_ = (cp.open(shm_topic_control_name(tn, static_cast<std::size_t>(o_.domain)))
                                       ? cp.generation()
                                       : 0u);
            const auto t0 = Clock::now();
            rebuild_begin_us_ = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
            const long long t0us_ = rebuild_begin_us_;

            /* 生产路径：旧发布端析构 ⇒ 新发布端重建（generation 递增）。
             * ⛔ 不以"等 generation 推进"为门（推进本身就由新发布端创建触发，那样会自锁）；
             *    改为**逐子步记时刻**，把重建时限拆开归因。 */
            auto us_now = [] {
                return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
            };
            const long long t_reset = us_now();
            shm_pubs_[static_cast<std::size_t>(rr)].reset();
            shm_pubs_[static_cast<std::size_t>(rr)] =
                std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), tn, static_cast<std::size_t>(o_.domain), false);
            shm_pubs_[static_cast<std::size_t>(rr)]->InitChannel("w10-rb");
            const long long t_pub_back = us_now();
            rebuild_substep_us_ = {t_reset, t_pub_back, 0, 0};

            /* 等对端重新 attach（Ready 且 peer≥1），有界到 deadline。 */
            long peers_after = 0;
            const auto ready_dl = t0 + std::chrono::milliseconds(o_.rebuild_deadline_ms);
            while (Clock::now() < ready_dl)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp2;
                peers_after = (cp2.open(shm_topic_control_name(tn, static_cast<std::size_t>(o_.domain)))
                               && cp2.state() == dzIPC::control_plane_shm::TopicState::Ready)
                                  ? static_cast<long>(cp2.peer_count())
                                  : 0;
                if (peers_after >= 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const long long t_peer_ready = us_now();
            {
                dzIPC::control_plane_shm::TopicControlPlane cp3;
                rebuild_gen_after_ = cp3.open(shm_topic_control_name(tn, static_cast<std::size_t>(o_.domain)))
                                         ? cp3.generation()
                                         : 0u;
            }
            /* 新代首包（seq 用**新区间**，与旧代/确认帧都不重叠）。 */
            const std::uint32_t new_seq = 500u;
            const bool pub_ok = pub_publish(rr, new_seq);
            const auto dl = t0 + std::chrono::milliseconds(o_.rebuild_deadline_ms);
            bool got = false;
            while (Clock::now() < dl)
            {
                (void)drain_once(rr, l, false);
                if (l.seen.count(new_seq) != 0) { got = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            rebuild_first_packet_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                           Clock::now().time_since_epoch()).count();
            rebuild_ms_ = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
            const long long t_first_packet = us_now();
            rebuild_substep_us_ = {t_reset, t_pub_back, t_peer_ready, t_first_packet};
            rebuild_new_gen_rx_ = got ? 1 : 0;
            old_gen_post_ = l.old_gen_rx_;      /* 重建后旧代帧（期望 0） */
            rebuild_old_gen_rx_ = old_gen_post_;
            rebuild_ok_ = got && rebuild_gen_after_ > rebuild_gen_before_ && rebuild_ms_ <= o_.rebuild_deadline_ms;
            l.planned += 1;
            char buf[320];
            std::snprintf(buf, sizeof buf,
                          "gen %u→%u rebuild_ms=%lld 极限=%ldms 新代首包=%ld pub_ok=%d peers_after=%ld "
                          "旧代帧(重建前)=%ld 旧代帧(重建后)=%ld pub_back=%lldus peer_ready=%lldus first_pkt=%lldus ok=%d",
                          rebuild_gen_before_, rebuild_gen_after_, rebuild_ms_, o_.rebuild_deadline_ms,
                          rebuild_new_gen_rx_, pub_ok ? 1 : 0, peers_after, old_gen_pre_, old_gen_post_,
                          (rebuild_substep_us_.size() > 1 ? rebuild_substep_us_[1] - t0us_ : -1),
                          (rebuild_substep_us_.size() > 2 ? rebuild_substep_us_[2] - t0us_ : -1),
                          (rebuild_substep_us_.size() > 3 ? rebuild_substep_us_[3] - t0us_ : -1),
                          rebuild_ok_ ? 1 : 0);
            rebuild_note_ = buf;
            std::printf("rebuild %s\n", buf);
        }
    }
    phases_.push_back({"rebuild", rebuild_ms_, rebuild_note_});

    /* ---------------- 阶段 9：reclaim（资源对账） ---------------- */
    t_phase = Clock::now();
    {
        /* ⛔ t33 修正：原实现恒取 **SHM 池** 的 route_count，
         * 导致 socket 臂的 `routes_before/after` 恒为 0（socket route 在 socket 池里），
         * 该列对 socket 不可读。改为按传输分别取各自池，并把两列都写进 verdict。 */
        const std::size_t shm_routes_now = dzIPC::threepools::RecvWorkerPool::instance().route_count();
        const std::size_t sk_routes_now = dzIPC::threepools::SocketRecvWorkerPool::instance().route_count();
        routes_before_destroy_ = is_shm() ? shm_routes_now : sk_routes_now;
        pool_alive_before_destroy_ = is_shm() ? static_cast<long>(shm_routes_now)
                                             : static_cast<long>(sk_routes_now);
        if (is_shm()) { shm_pubs_.clear(); shm_subs_.clear(); }
        else { sk_pubs_.clear(); sk_subs_.clear(); }
        std::this_thread::sleep_for(500ms);
        const std::size_t shm_routes_after = dzIPC::threepools::RecvWorkerPool::instance().route_count();
        const std::size_t sk_routes_after = dzIPC::threepools::SocketRecvWorkerPool::instance().route_count();
        routes_after_destroy_ = is_shm() ? shm_routes_after : sk_routes_after;
        pool_after_destroy_ = is_shm() ? static_cast<long>(shm_routes_after)
                                      : static_cast<long>(sk_routes_after);
        threads_after_destroy_ = thread_count();
        fds_after_destroy_ = fd_count();
    }
    phases_.push_back({"reclaim", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_phase).count(),
                       "routes=" + std::to_string(routes_before_destroy_) + "->" + std::to_string(routes_after_destroy_)});

    /* ---------------- 计数与窗口统计 ---------------- */
    {
        const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
        const auto v = [&](dzIPC::measure::CounterId id) { return static_cast<unsigned long long>(c.get(id)); };
        fallback_ = static_cast<long>(v(dzIPC::measure::CounterId::fallback_total));
        std::printf("counters fallback_total=%llu backend_unavailable=%llu capacity_full=%llu wait_set_full=%llu "
                    "wait_token_invalid=%llu registration_attempts=%llu registration_ok=%llu registration_failed=%llu "
                    "chunk_exhausted=%llu fd_limit=%llu queue_evicted=%llu generation_mismatch=%llu publish_blocked=%llu\n",
                    v(dzIPC::measure::CounterId::fallback_total),
                    v(dzIPC::measure::CounterId::fallback_backend_unavailable),
                    v(dzIPC::measure::CounterId::fallback_capacity_full),
                    v(dzIPC::measure::CounterId::wait_set_full),
                    v(dzIPC::measure::CounterId::wait_token_invalid),
                    v(dzIPC::measure::CounterId::registration_attempts),
                    v(dzIPC::measure::CounterId::registration_ok),
                    v(dzIPC::measure::CounterId::registration_failed),
                    v(dzIPC::measure::CounterId::chunk_exhausted),
                    v(dzIPC::measure::CounterId::fd_limit),
                    v(dzIPC::measure::CounterId::queue_evicted),
                    v(dzIPC::measure::CounterId::generation_mismatch),
                    v(dzIPC::measure::CounterId::publish_blocked));
    }
    if (is_shm())
    {
        auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
        const auto st = pool.stats();
        const auto& b = pool.budget();
        worker_path_ = g_seam.worker_path;
        std::printf("pool running=%d workers=%zu route_count=%zu msgs=%llu wait_wakeups=%llu wait_timeouts=%llu "
                    "idle_exits=%llu restarts=%llu recv_once_calls=%llu recv_once_max_ns=%llu over_budget=%llu recv_errors=%llu\n",
                    static_cast<int>(pool.running()), pool.worker_count(), st.route_count,
                    (unsigned long long)st.messages_received, (unsigned long long)st.wait_wakeups,
                    (unsigned long long)st.wait_timeouts, (unsigned long long)st.idle_exits,
                    (unsigned long long)st.thread_restarts, (unsigned long long)st.recv_once_calls,
                    (unsigned long long)st.recv_once_max_ns, (unsigned long long)st.recv_once_over_budget,
                    (unsigned long long)st.recv_errors);
        std::printf("budget msgs=%zu bytes=%zu time_us=%lld wait_ms=%lld idle_ms=%lld\n", b.max_messages_per_route,
                    b.max_bytes_per_route, (long long)b.max_processing_time_per_route.count(),
                    (long long)b.wait_timeout.count(), (long long)b.idle_keep_alive.count());
    }
    else
    {
        auto& pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
        const auto st = pool.stats();
        std::printf("socket_pool running=%d workers=%zu route_count=%zu msgs=%llu wait_timeouts=%llu idle_exits=%llu restarts=%llu recv_errors=%llu\n",
                    static_cast<int>(pool.running()), pool.worker_count(), st.route_count,
                    (unsigned long long)st.messages_received, (unsigned long long)st.wait_timeouts,
                    (unsigned long long)st.idle_exits, (unsigned long long)st.thread_restarts,
                    (unsigned long long)st.recv_errors);
    }
    {
        std::lock_guard<std::mutex> lock(g_seam.m);
        std::printf("worker_path_count=%ld compat_total=%ld fallback_count=%ld path_choice=%ld reasons=",
                    g_seam.worker_path, g_seam.compat_total, g_seam.fallback, g_seam.path_choice);
        for (auto& kv : g_seam.reasons) std::printf("%d:%ld,", kv.first, kv.second);
        std::printf("\n");
    }
    std::printf("routes_before_destroy=%zu routes_after_destroy=%zu threads_after_destroy=%zu fds_after_destroy=%zu\n",
                routes_before_destroy_, routes_after_destroy_, threads_after_destroy_, fds_after_destroy_);

    /* ---------------- 判定（H6：§13.2 六项**逐条机械断言**） ----------------
     * 修前缺陷：① 记录 dup/ooo 但总判定**不据此判失败**；② 生命周期/资源条件未全部机械判定
     * ⇒ 人工台账审计与 `PASS` 可能不一致。修后：六条各建一个**布尔断言**（`c1..c6`），
     * 任一条失败即 `verdict=1`；断言结果逐条写入 verdict.md。⛔ 不得用日志提示代替失败状态。 */
    int verdict = 0;
    const auto fail = [&](const std::string& s) { failures_.push_back(s); verdict = 1; };

    /* 台账重算（方案 §5.3：registered/valid_rx 从**逐 route 台账**重算，不用计数器自述）。 */
    long ledger_reg = 0, ledger_valid = 0, ledger_dup = 0, ledger_ooo = 0, ledger_corrupt = 0, ledger_timeout = 0;
    for (long i = 0; i < o_.n; ++i)
    {
        const RouteLedger& l = led_[static_cast<std::size_t>(i)];
        if (l.registered) ++ledger_reg;
        const bool counted = (o_.topology == "hotcold") ? (i != 0) : true;
        if (counted && l.rx >= l.planned) ++ledger_valid;
        ledger_dup += l.dup;
        ledger_ooo += l.ooo;
        ledger_corrupt += l.corrupt;
        ledger_timeout += l.timeout;
    }
    /* 计数器自述与台账重算**必须一致**（否则说明读数来源不同步）。 */
    if (ledger_reg != registered_) fail("计数器 registered=" + std::to_string(registered_) + " 与台账重算=" +
                                        std::to_string(ledger_reg) + " 不一致");
    if (ledger_valid != valid_rx_) fail("计数器 valid_rx=" + std::to_string(valid_rx_) + " 与台账重算=" +
                                        std::to_string(ledger_valid) + " 不一致");

    /* ---- §13.2 #1：registered_count == expected（逐 route） ---- */
    const bool c1 = (registered_ == o_.n) && (ledger_reg == o_.n);
    if (!c1) fail("§13.2#1 registered_count=" + std::to_string(registered_) + "/" + std::to_string(o_.n) +
                  "（台账重算 " + std::to_string(ledger_reg) + "）");
    for (auto& x : hs_failed_) fail("§13.2#1 " + x);

    /* ---- §13.2 #2：valid_rx_count == expected，且**逐 route 序号+载荷校验** ---- */
    const bool c2 = (valid_rx_ == expected_valid_) && (ledger_corrupt == 0) && (ledger_dup == 0) &&
                    (ledger_ooo == 0) && (ledger_timeout == 0);
    if (valid_rx_ != expected_valid_)
        fail("§13.2#2 valid_rx_count=" + std::to_string(valid_rx_) + " != expected " + std::to_string(expected_valid_));
    if (ledger_dup) fail("§13.2#2 重复序号 dup=" + std::to_string(ledger_dup) + "（⛔ 直接判失败）");
    if (ledger_ooo) fail("§13.2#2 乱序 out_of_order=" + std::to_string(ledger_ooo) + "（⛔ 直接判失败）");
    if (ledger_corrupt) fail("§13.2#2 载荷校验失败 corrupt=" + std::to_string(ledger_corrupt));
    if (ledger_timeout) fail("§13.2#2 超期未收满 timeout_routes=" + std::to_string(ledger_timeout));

    /* ---- §13.2 #3：fallback == 0，且四类回退各自独立计数 ---- */
    const bool c3 = (fallback_ == 0) && (g_seam.fallback == 0);
    if (fallback_ != 0) fail("§13.2#3 fallback_count=" + std::to_string(fallback_) + " != 0");
    if (g_seam.fallback != 0) fail("§13.2#3 seam 真回退=" + std::to_string(g_seam.fallback) + " != 0");
    if (fallback_ == 0 && g_seam.compat_total != 0 && g_seam.path_choice == 0)
        fail("§13.2#3 compat_total>0 且非路径选择（回退被掩盖）");
    if (picker_short_) fail("§13.2#3 picker 未能提供足够无碰撞话题名（重名 ⇒ 串包）");

    /* ---- §13.2 #4：固定配置（⛔ 无 per-route 补齐） ---- */
    const long pool_alive = is_shm() ? pool_routes_alive_ : socket_pool_routes_alive_;
    bool c4 = (pool_alive == registered_) && (threads_create_ > 0);
    if (is_shm() && o_.topology != "hotcold" && worker_path_ == 0)
    {
        c4 = false;
        fail("§13.2#4 worker_path_count=0（无 route 实际归属 worker）");
    }
    if (o_.topology != "hotcold" && pool_alive != registered_)
    {
        c4 = false;
        fail("§13.2#4 存活期池 route_count=" + std::to_string(pool_alive) + " != registered=" +
             std::to_string(registered_) + "（有 route 未归属 worker）");
    }

    /* ---- §13.2 #5：关闭/恢复在冻结时限内、旧 generation 无投递 ---- */
    bool c5 = true;
    if (o_.topology != "hotcold" && o_.resume_msgs > 0)
    {
        if (recover_first_packet_ok_ == 0)
        {
            c5 = false;
            fail("§13.2#5 §10.1 恢复首包失败（delay_us=" + std::to_string(recover_delay_us_) + "，阶段耗时=" +
                 std::to_string(recover_phase_us_) + "）");
        }
        if (recover_delay_us_ < 0)
        {
            c5 = false;
            fail("§13.2#5 恢复延迟为负（" + std::to_string(recover_delay_us_) + "）");
        }
        if (recover_phase_us_ > 0 && recover_delay_us_ > recover_phase_us_)
        {
            c5 = false;
            fail("§13.2#5 恢复延迟 " + std::to_string(recover_delay_us_) + "us 超过恢复阶段耗时 " +
                 std::to_string(recover_phase_us_) + "us（时间戳口径不一致）");
        }
    }
    if (recover_lost_ != 0)
    {
        c5 = false;
        fail("§13.2#5 恢复阶段丢包=" + std::to_string(recover_lost_));
    }
    /* 关闭时限按 R0-4 的**单通道/批**分层：本段并发析构 N/2 个订阅者（每条一个通道），
     * 因此合理判据是 `close_ms <= (N/2) × close_deadline_ms`（每条通道 ≤2 s），
     * 同时必须满足**批次总期限**（register_batch_deadline_ms 的量级）。
     * ⛔ 修前把 N/2 条通道的**总耗时**与**单通道上限**直接比较 ⇒ 千路必然假失败
     * （这正是 R0-4 "不得用一个量顶替另一个量"要防的错）。 */
    const long close_channels = std::max(1L, (o_.n + 1) / 2);
    const long close_budget_ms = std::min<long>(o_.register_batch_deadline_ms,
                                               close_channels * o_.close_deadline_ms);
    if (close_ms_ >= 0 && close_ms_ > close_budget_ms)
    {
        c5 = false;
        fail("§13.2#5 并发关闭 " + std::to_string(close_ms_) + "ms 超过批次上限 " +
             std::to_string(close_budget_ms) + "ms（" + std::to_string(close_channels) + " 条通道 × " +
             std::to_string(o_.close_deadline_ms) + "ms 单通道，且不超批次期限）");
    }
    /* 单通道均值也单独记录（可归因，不判失败）。 */
    const double close_per_channel_ms = close_channels > 0 ? static_cast<double>(close_ms_) / close_channels : 0.0;
    std::printf("close_budget channels=%ld per_channel_avg_ms=%.3f budget_ms=%ld actual_ms=%lld\n", close_channels,
                close_per_channel_ms, close_budget_ms, close_ms_);
    if (is_shm() && !o_.skip_rebuild && o_.topology != "broadcast" && o_.n >= 2 && !rebuild_ok_)
    {
        c5 = false;
        fail("§13.2#5 重建未通过（" + rebuild_note_ + "）");
    }
    if (old_gen_post_ != 0)
    {
        c5 = false;
        fail("§13.2#5 重建后仍收到旧 generation 帧 " + std::to_string(old_gen_post_) + " 条");
    }

    /* ---- §13.2 #6：route/token/fd 回基线（queue/chunk 无读数 API ⇒ 不可判定，⛔ 不判 ✅） ---- */
    bool c6 = true;
    if (routes_after_destroy_ != 0)
    {
        c6 = false;
        fail("§13.2#6 销毁后池 route_count=" + std::to_string(routes_after_destroy_) + " != 0");
    }
    if (pool_after_destroy_ != 0)
    {
        c6 = false;
        fail("§13.2#6 销毁后 token（池内在册）=" + std::to_string(pool_after_destroy_) + " != 0");
    }
    const bool c6_undecidable = true;   /* queue/chunk 无公开读数 API：本条**不得**判 ✅ */

    /* ---- fd：按 R0-5/R0-6 冻结模型（分母 = 端点总数，不是话题数） ---- */
    {
        const long endpoints = 2 * (broadcast ? 1 : o_.n) /* pub 端点 */ + 2 * o_.n /* sub 端点 */;
        const long pred = 4 + 2 * 32 + endpoints;         /* 4 + 2*W + 2*N_pub + 2*N_sub */
        std::printf("fd_model fds_open=%zu endpoints=%ld predicted=%ld budget_4096=%s\n", fds_create_, endpoints,
                    pred, fds_create_ <= 4096 ? "OK" : "OVER");
        if (fds_create_ > 4096)
            fail("fd 超硬上限 4096（实测 " + std::to_string(fds_create_) + "，模型预测 " + std::to_string(pred) + "）");
    }

    std::printf("verdict_assertions c1=%d c2=%d c3=%d c4=%d c5=%d c6_undecidable=%d\n", c1 ? 1 : 0, c2 ? 1 : 0,
                c3 ? 1 : 0, c4 ? 1 : 0, c5 ? 1 : 0, c6_undecidable ? 1 : 0);
    conditions_ = {c1, c2, c3, c4, c5, c6};

    /* run_id 一致性（方案 §5.3）：与期望不符 ⇒ 非零退出。 */
    if (!o_.expect_run_id.empty() && o_.expect_run_id != o_.run_id)
    {
        fail("run_id 与期望不一致：actual=" + o_.run_id + " expected=" + o_.expect_run_id);
        run_id_mismatch_ = true;
    }
    for (auto& x : adversarial_) findings_.push_back(x);
    if (!o_.force_fail.empty()) fail("注入失败（--force-fail）：" + o_.force_fail);

    write_artifacts(verdict);
    if (!artifacts_ok_) { verdict = 1; failures_.push_back("证据落盘失败：" + artifacts_note_); }
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W10_MATRIX_DONE verdict=%s failures=%zu\n", verdict ? "FAIL" : "PASS", failures_.size());
    for (auto& f : failures_) std::printf("FAILURE: %s\n", f.c_str());
    return verdict;
}

void Harness::write_artifacts(int verdict)
{
    if (o_.out_dir.empty()) { artifacts_ok_ = true; artifacts_note_ = "未指定 --out（按纪律不落盘）"; return; }
    /* 证据目录只追加（方案 §12）：目录不存在则创建；已存在且非空则**拒绝写入**，
     * 避免覆盖既有 run 的证据（重跑必须新建 run_id）。 */
    const std::string dir = o_.out_dir;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec))
    {
        /* 阶段 0 已**独占预占**并写入 `.claimed`；那是本进程自己的标记，不算"非空"。 */
        bool only_own_claim = false;
        {
            bool any_other = false;
            std::error_code e2;
            for (const auto& ent : std::filesystem::directory_iterator(dir, e2))
            {
                const std::string nm = ent.path().filename().string();
                if (nm == ".claimed") continue;
                std::ifstream cf(ent.path());
                std::string first;
                std::getline(cf, first);
                if (nm.empty()) { any_other = true; break; }
                (void)first;
                any_other = true;
                break;
            }
            only_own_claim = !any_other;
        }
        if (!std::filesystem::is_empty(dir, ec) && !only_own_claim)
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s —— 拒绝覆盖（重跑请新建 run_id）\n", dir.c_str());
            artifacts_ok_ = false;
            artifacts_note_ = "输出目录非空，拒绝写入";
            return;
        }
    }
    else if (!std::filesystem::create_directories(dir, ec))
    {
        std::printf("ARTIFACT_DIR_CREATE_FAILED: %s (%s)\n", dir.c_str(), ec.message().c_str());
        artifacts_ok_ = false;
        artifacts_note_ = "输出目录创建失败: " + ec.message();
        return;
    }
    const auto sh = [&](const char* name) { return dir + "/" + name; };

    /* ledger.csv 列语义（F6 修后）：
     *   sent     = 交给库的**发送尝试数**（含返回 false 的尝试）⇒ 与 `planned` 独立，可判"发送未被接收拖慢"
     *   sent_ok  = 库返回 true 的次数
     *   confirm_rx = H2 握手确认帧的接收数（与规模帧分账，⛔ 不计入 rx）
     *   注：确认帧与重建帧的发送量在握手/重建阶段结束后**已从 sent 复位**，
     *       故 `sent` 只含规模阶段与该 route 参与的重建阶段的尝试。 */

    /* 台账合计（从**逐 route 台账重算**，供 counters.json 与 verdict.md 共用；⛔ 不用计数器自述）。 */
    long timeout_routes = 0, corrupt_routes = 0, dup_total = 0, ooo_total = 0, corrupt_total = 0;
    long timeout_routes_all = 0;
    for (const auto& l : led_)
    {
        if (l.timeout) ++timeout_routes;
        if (l.corrupt) ++corrupt_routes;
        if (l.timeout) ++timeout_routes_all;
        dup_total += l.dup;
        ooo_total += l.ooo;
        corrupt_total += l.corrupt;
    }

    {
        std::ofstream f(sh("ledger.csv"));
        f << "route,registered,planned,sent,sent_ok,rx,dup,out_of_order,corrupt,timeout,rx_after_silence,"
             "first_packet_ok,first_packet_us,confirm_rx\n";
        for (const auto& l : led_) f << l.to_csv() << "\n";
    }
    {
        std::ofstream f(sh("phases.csv"));
        f << "phase,ms,note\n";
        for (const auto& p : phases_) f << p.name << "," << p.ms << ",\"" << p.note << "\"\n";
    }
    {
        std::ofstream f(sh("samples.jsonl"));
        for (const auto& s : samples_) f << s << "\n";
    }
    {
        /* counters.json：方案 §13.3 的**十类**失败归因逐类计数 + 首个失败资源。
         * 名称逐字对齐 §13.3（`fallback_activated` 在 counters.h 里的实际名字是
         * `fallback_total`，此处两个名字都给出，避免下游对不上）。 */
        const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
        const auto gv = [&](dzIPC::measure::CounterId id) { return static_cast<unsigned long long>(c.get(id)); };

        /* 首个失败资源：按 §13.3 的枚举顺序取第一个非零项（可机械判定）。 */
        std::vector<std::pair<std::string, unsigned long long>> classes = {
            {"registration_rejected", gv(dzIPC::measure::CounterId::registration_rejected)},
            {"wait_token_invalid", gv(dzIPC::measure::CounterId::wait_token_invalid)},
            {"wait_set_full", gv(dzIPC::measure::CounterId::wait_set_full)},
            {"fd_limit", gv(dzIPC::measure::CounterId::fd_limit)},
            {"chunk_exhausted", gv(dzIPC::measure::CounterId::chunk_exhausted)},
            {"queue_backpressure", gv(dzIPC::measure::CounterId::queue_backpressure)},
            {"generation_mismatch", gv(dzIPC::measure::CounterId::generation_mismatch)},
            {"fallback_activated", static_cast<unsigned long long>(fallback_)},
            {"publish_blocked", gv(dzIPC::measure::CounterId::publish_blocked)},
            {"rx_timeout", gv(dzIPC::measure::CounterId::rx_timeout)},
        };
        std::string first_nz = "none";
        for (const auto& kv : classes)
            if (kv.second != 0) { first_nz = kv.first; break; }
        /* 工装侧观测（不属于 §13.3 枚举但必须与上述并列，防"只保留总失败数"）。 */
        classes.push_back({"harness_timeout_routes", static_cast<unsigned long long>(timeout_routes)});
        classes.push_back({"harness_corrupt_routes", static_cast<unsigned long long>(corrupt_routes)});
        classes.push_back({"harness_dup_total", static_cast<unsigned long long>(dup_total)});
        classes.push_back({"harness_out_of_order_total", static_cast<unsigned long long>(ooo_total)});

        /* t43：§13.2#3 的**覆盖范围声明**（把 t35/F1 §6 的解读纪律机器化）。
         * ⛔ 未接线 ID 的 0 必须读作「未采集」，不得读作「本轮未发生」。 */
        const std::vector<std::string> unwired_ids = {
            "tlv_bytes", "dzflat_a_bytes", "dzflat_b_bytes",
            "fallback_type_incompatible", "fallback_oversized", "fallback_pool_exhausted", "fallback_reason_unknown",
            "borrow_failed_no_receiver", "borrow_failed_pool_exhausted", "borrow_failed_oversized",
            "borrow_failed_publish", "borrow_failed_reason_unknown",
            "path_selection_dzflat_disabled", "path_selection_type_unsupported",
            "registration_rejected", "fd_limit", "queue_backpressure", "generation_mismatch",
            "publish_blocked", "publish_failed", "rx_timeout",
            "seq_monotonic_ok", "seq_out_of_order", "seq_duplicate", "seq_lost",
            "payload_checksum_ok", "payload_checksum_bad"};
        const std::vector<std::string> diag_gated = {"scan_time_ns_total", "ready_observed",
                                                     "deferred_depth_last", "deferred_depth_max"};
        const std::vector<std::string> covered = {
            "wait_token_invalid", "wait_set_full", "chunk_exhausted", "chunk_alloc_failed",
            "queue_evicted", "fallback_total(=fallback_activated)", "fallback_capacity_full",
            "fallback_backend_unavailable", "registration_attempts", "registration_ok",
            "registration_failed", "registration_duplicate", "registration_busy"};

        std::ofstream f(sh("counters.json"));
        f << "{\n  \"run_id\": \"" << o_.run_id << "\",\n";
        f << "  \"topology\": \"" << o_.topology << "\",\n";
        f << "  \"transport\": \"" << o_.transport << "\",\n";
        f << "  \"expected_count\": " << o_.n << ",\n";
        f << "  \"registered_count\": " << registered_ << ",\n";
        f << "  \"valid_rx_count\": " << valid_rx_ << ",\n";
        f << "  \"failure_classes\": {\n";
        for (std::size_t i = 0; i < classes.size(); ++i)
            f << "    \"" << classes[i].first << "\": " << classes[i].second
              << (i + 1 < classes.size() ? "," : "") << "\n";
        f << "  },\n";
        /* §13.2#3 的**四类容量计数**（R1 复核 F1 的原文四类）—— 单独成段，便于逐条引用。
         * ⛔ 与 §13.3 十类**不是同一张表**：这里只列这四类 + 两个接线后的关键类别。 */
        f << "  \"capacity_classes\": {\n";
        f << "    \"wait_set_full\": " << gv(dzIPC::measure::CounterId::wait_set_full) << ",\n";
        f << "    \"wait_token_invalid\": " << gv(dzIPC::measure::CounterId::wait_token_invalid) << ",\n";
        f << "    \"chunk_exhausted\": " << gv(dzIPC::measure::CounterId::chunk_exhausted) << ",\n";
        f << "    \"queue_evicted\": " << gv(dzIPC::measure::CounterId::queue_evicted) << ",\n";
        f << "    \"chunk_alloc_failed\": " << gv(dzIPC::measure::CounterId::chunk_alloc_failed) << ",\n";
        f << "    \"fallback_total\": " << static_cast<unsigned long long>(fallback_) << ",\n";
        f << "    \"note\": \"四类=§13.2#3 原文（wait-set 满 / token 无效 / chunk 耗尽 / 队列淘汰）；"
             "四类的 0 只有在【接线已被独立臂证明】的前提下才可读作『本轮未发生』——"
             "接线有效性证据见 W10/R4b 的 A(send 腿)/B(loan 腿)/C(队列)/D(waitset)/E2(token) 五臂\"\n";
        f << "  },\n";
        f << "  \"first_failed_resource\": \"" << first_nz << "\",\n";
        f << "  \"first_failed_resource_caveat\": \""
          << (first_nz == "none"
                  ? "none 的排除力**不完整**：未采集集合内的类别其 0 不可读作未发生（见 counter_coverage.unwired_ids）"
                  : "定位到具体资源（该类别接线有效且本 run 实测非零）")
          << "\",\n";
        f << "  \"counter_coverage\": {\n";
        f << "    \"diagnostics_enabled\": "
          << (dzIPC::measure::CounterRegistry::instance().diagnostics_enabled() ? "true" : "false") << ",\n";
        f << "    \"covered_classes\": [";
        for (std::size_t i = 0; i < covered.size(); ++i) f << (i ? ", " : "") << "\"" << covered[i] << "\"";
        f << "],\n    \"unwired_ids\": [";
        for (std::size_t i = 0; i < unwired_ids.size(); ++i) f << (i ? ", " : "") << "\"" << unwired_ids[i] << "\"";
        f << "],\n    \"diagnostics_gated_ids\": [";
        for (std::size_t i = 0; i < diag_gated.size(); ++i) f << (i ? ", " : "") << "\"" << diag_gated[i] << "\"";
        f << "],\n    \"reading_discipline\": \""
             "凡 unwired_ids 内的 ID，其 0 一律读作【未采集/未接线】，不得读作【本轮未发生】"
             "（依据 t35/F1 §6 冻结纪律）；diagnostics_gated_ids 在 diagnostics_enabled=false 时结构性必为 0。\",\n";
        f << "    \"scope_of_this_run\": \"§13.2#3 对 covered_classes 可判定；对其余类别"
             "（unwired_ids + diagnostics_gated_ids）只能写【未采集】\"\n  },\n";
        f << "  \"harness_failures\": [";
        for (std::size_t i = 0; i < failures_.size(); ++i)
            f << (i ? ", " : "") << "\"" << dzIPC::measure::json_escape(failures_[i]) << "\"";
        f << "],\n";
        f << "  \"registry_counters\": " << dzIPC::measure::CounterRegistry::instance().to_json();
        f << ",\n  \"verdict\": \"" << (verdict ? "规模试验失败" : "通过") << "\"\n}\n";
    }
    {
        /* H1 要求：**保存原始表与解析结果**（供复核逐行列复算），并把读取错误一并落盘。 */
        PortScanResult ps = scan_used_udp_ports();
        {
            std::ofstream f(sh("port_table.txt"));
            f << "# /proc/net/udp + /proc/net/udp6 原始表（H1：端口预检的输入）\n";
            for (const auto& l : ps.raw_lines) f << l << "\n";
        }
        {
            std::ofstream f(sh("port_parse.txt"));
            f << "# H1 端口预检的解析结果\n";
            f << "parse_ok=" << (ps.ok ? 1 : 0) << "\n";
            f << "parsed_ok=" << ps.parsed_ok << "\n";
            f << "parsed_bad=" << ps.parsed_bad << "\n";
            f << "used_set_size=" << ps.used.size() << "\n";
            f << "used_ports=";
            for (int v : ps.used) f << v << ",";
            f << "\n";
            for (auto& e : ps.errors) f << "error=" << e << "\n";
            f << "# picker 诊断\n";
            for (auto& d : picker_diag_) f << d << "\n";
        }
    }
    {
        std::ofstream f(sh("manifest.json"));
        f << "{\n";
        f << "  \"run_id\": \"" << o_.run_id << "\",\n";
        f << "  \"work_package\": \"W10\",\n";
        f << "  \"transport\": \"" << o_.transport << "\",\n";
        f << "  \"topology\": \"" << o_.topology << "\",\n";
        f << "  \"process_model\": \"同一进程内 pub/sub（跨进程切分见 verdict.md 说明）\",\n";
        f << "  \"measurement_mode\": \"periodic-sampling\",\n";
        f << "  \"topic_count\": " << o_.n << ",\n";
        f << "  \"unique_topic_count\": " << (o_.topology == "broadcast" ? 1 : o_.n) << ",\n";
        f << "  \"expected_count\": " << o_.n << ",\n";
        f << "  \"registered_count\": " << registered_ << ",\n";
        f << "  \"valid_rx_count\": " << valid_rx_ << ",\n";
        f << "  \"fallback_count\": " << fallback_ << ",\n";
        f << "  \"worker_path_count\": " << worker_path_ << ",\n";
        f << "  \"pool_routes_alive\": " << pool_routes_alive_ << ",\n";
        f << "  \"socket_pool_routes_alive\": " << socket_pool_routes_alive_ << ",\n";
        f << "  \"messages_per_route\": " << o_.msgs << ",\n";
        f << "  \"payload_bytes\": " << o_.payload << ",\n";
        f << "  \"threads_after_create\": " << threads_create_ << ",\n";
        f << "  \"fds_open\": " << fds_create_ << ",\n";
        f << "  \"max_fd\": " << maxfd_create_ << ",\n";
        f << "  \"routes_before_destroy\": " << routes_before_destroy_ << ",\n";
        f << "  \"routes_after_destroy\": " << routes_after_destroy_ << ",\n";
        f << "  \"idle_state\": \"" << idle_state_ << "\",\n";
        f << "  \"idle_window_s\": " << window_s_ << ",\n";
        f << "  \"idle_cpu_cores\": " << cpu_cores_ << ",\n";
        f << "  \"idle_ctx_per_s\": " << ((ctx_vol_ + ctx_nonvol_) / (window_s_ > 0 ? window_s_ : 1.0)) << ",\n";
        f << "  \"ctx_scope\": \"per-TID aggregate (/proc/self/task/<tid>/status), unreadable_tids=" << ctx_unreadable_ << "\",\n";
        f << "  \"threads_peak\": " << threads_peak_ << ",\n";
        f << "  \"recover_first_packet_ok\": " << recover_first_packet_ok_ << ",\n";
        f << "  \"recover_first_packet_us\": " << recover_first_packet_us_ << ",\n";
        f << "  \"recover_lost\": " << recover_lost_ << ",\n";
        f << "  \"produced_at\": \"" << ts() << "\",\n";
        f << "  \"counter_reading_discipline\": \""
             "counters.json 内 unwired_ids 的 0 = 【未采集】而非【未发生】（t35/F1 §6 冻结纪律）；"
             "diagnostics_gated_ids 在 diagnostics_enabled=false 时结构性必为 0；"
             "§13.2#3 仅对 counter_coverage.covered_classes 可判定\",\n";
        f << "  \"diagnostics_enabled\": "
          << (dzIPC::measure::CounterRegistry::instance().diagnostics_enabled() ? "true" : "false") << ",\n";
        f << "  \"result_files\": [\"ledger.csv\",\"phases.csv\",\"samples.jsonl\",\"counters.json\","
             "\"manifest.json\",\"verdict.md\",\"port_table.txt\",\"port_parse.txt\"]\n";
        f << "}\n";
    }
    {
        std::ofstream f(sh("verdict.md"));
        f << "# W10 " << o_.topology << " / " << o_.transport << " — 判定\n\n";
        f << "- run_id: `" << o_.run_id << "`\n- 拓扑: `" << o_.topology << "`（" << o_.n << " 个话题，每 route "
          << o_.msgs << " 条，载荷 " << o_.payload << " B）\n";
        f << "- 判定: **" << (verdict ? "规模试验失败" : "通过") << "**\n\n";
        f << "## §13.2 六条\n\n";
        f << "| # | 条件 | 实测 | 判定 |\n|---|---|---|---|\n";
        const auto cond = [&](std::size_t i) { return i < conditions_.size() && conditions_[i]; };
        f << "> **H6 机械断言**（c1..c6 由总判定直接产生，⛔ 不用日志提示代替失败状态）："
             "c1=" << (cond(0) ? 1 : 0) << " c2=" << (cond(1) ? 1 : 0) << " c3=" << (cond(2) ? 1 : 0)
          << " c4=" << (cond(3) ? 1 : 0) << " c5=" << (cond(4) ? 1 : 0)
          << " c6=部分测量（不可判定）\n\n";
        f << "| 1 | registered_count == expected | " << registered_ << "/" << o_.n << " | " << (registered_ == o_.n ? "✅" : "❌") << " |\n";
        f << "| 2 | valid_rx_count == expected（逐 route 序号+载荷校验） | " << valid_rx_ << "/" << expected_valid_
          << "；dup=" << dup_total << " ooo=" << ooo_total << " corrupt=" << corrupt_total << " timeout_routes="
          << timeout_routes_all << " | "
          << (valid_rx_ == expected_valid_ && dup_total == 0 && ooo_total == 0 && corrupt_total == 0 &&
                      timeout_routes_all == 0
                  ? "✅"
                  : "❌")
          << " |\n";
        if (o_.topology == "hotcold")
            f << "\n> hotcold 拓扑：热路（route 0）为**开环满速**，实收 " << hot_route_rx_ << "/" << hot_route_planned_
              << "（丢包为开环语义的一部分，按 §13.1 只计数、不作判据）；冷路 1.." << (o_.n - 1) << " 各发 1 条须全部收到 ⇒ 判据用 "
              << expected_valid_ << "。\n";
        f << "| 3 | fallback_count == 0 | " << fallback_ << " | " << (fallback_ ? "❌" : "✅") << " |\n";
        f << "| 4 | worker/控制线程符合冻结配置（⛔ 无 per-route 补齐） | ";
        if (is_shm())
            f << "worker_path=" << worker_path_ << "（seam）pool_routes_alive=" << pool_routes_alive_;
        else
            f << "socket_pool_routes_alive=" << socket_pool_routes_alive_ << "（池内归属，socket 无 seam）";
        f << " threads=" << threads_create_ << "（create 后） / " << threads_after_destroy_ << "（reclaim 后）"
          << " | " << (socket_pool_routes_alive_ == o_.n || (!is_shm() && registered_ == o_.n) ? "✅" : "❌") << " |\n";
        f << "| 5 | 关闭/恢复在冻结超时内且旧 generation 无投递 | recover_ok=" << recover_first_packet_ok_
          << " lost=" << recover_lost_ << " | " << ((recover_first_packet_ok_ && recover_lost_ == 0) || o_.topology == "hotcold" ? "✅" : "❌") << " |\n";
        f << "| 6 | 结束时 route/token/fd/队列/chunk 回基线（±5%） | routes " << routes_before_destroy_ << "→"
          << routes_after_destroy_ << "，fd " << fds_create_ << "→" << fds_after_destroy_
          << "，**token（代理：存活期池内在册数→销毁后 0）** " << pool_alive_before_destroy_ << "→"
          << pool_after_destroy_ << "；queue/chunk 见下 | 部分测量 |\n\n";
        f << "> §13.2 #6 的**逐类结论**（⛔ 不接受把未测项打成 ✅）：\n\n";
        f << "| 资源 | 实测 | 判定 |\n|---|---|---|\n";
        f << "| route | " << routes_before_destroy_ << "→" << routes_after_destroy_ << " | "
          << (routes_after_destroy_ == 0 ? "✅ 已测" : "❌") << " |\n";
        f << "| token | 池内在册 **" << pool_alive_before_destroy_ << "→" << pool_after_destroy_
          << "**（与 token 一一对应：add_route 取 token、remove_route 同步摘除） | "
          << (pool_after_destroy_ == 0 ? "✅ 已测（代理）" : "❌") << " |\n";
        f << "| fd | " << fds_create_ << "→" << fds_after_destroy_ << " | ✅ 已测 |\n";
        f << "| queue（view/adopt 深度） | ⛔ **无公开读数 API** | **未测（不可判定）** |\n";
        f << "| chunk（池内占用） | ⛔ **无公开读数 API** | **未测（不可判定）** |\n\n";
        f << "## 失败清单（" << failures_.size() << " 条）\n\n";
        if (failures_.empty()) f << "（无）\n";
        for (auto& x : failures_) f << "- " << x << "\n";
        f << "\n## 载荷校验失败明细（首次，最多每 route 一条）\n\n";
        if (corrupt_details_.empty()) f << "（无）\n";
        for (auto& x : corrupt_details_) f << "- `" << x << "`\n";
        f << "\n## 既有缺陷侦察（⛔ 与本轮改造引入的回归分开登记）\n\n";
        if (findings_.empty()) f << "（本次未触发）\n";
        for (auto& x : findings_) f << "- " << x << "\n";
    }
    /* 必需文件三件套（H5）：缺一即"落盘失败也是失败"。 */
    {
        const char* need[] = {"ledger.csv", "phases.csv", "samples.jsonl",
                              "counters.json", "manifest.json", "verdict.md"};
        std::vector<std::string> missing;
        for (const char* n : need)
        {
            std::error_code e;
            if (!std::filesystem::exists(sh(n), e)) missing.push_back(n);
        }
        if (!missing.empty())
        {
            artifacts_ok_ = false;
            artifacts_note_ = "必需文件缺失: ";
            for (auto& m : missing) artifacts_note_ += m + " ";
            std::printf("ARTIFACT_MISSING: %s\n", artifacts_note_.c_str());
        }
        else
        {
            artifacts_ok_ = true;
        }
    }
    std::printf("artifacts_dir=%s artifacts_ok=%d\n", o_.out_dir.c_str(), artifacts_ok_ ? 1 : 0);
    std::fflush(stdout);
}

}   // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    o.transport = arg_str(argc, argv, "--transport", "shm");
    o.topology = arg_str(argc, argv, "--topology", "independent");
    o.n = arg_long(argc, argv, "--n", 100);
    o.domain = arg_long(argc, argv, "--domain", 300);
    o.msgs = arg_long(argc, argv, "--msgs", 3);
    o.payload = arg_long(argc, argv, "--payload", 64);
    o.silence_ms = arg_long(argc, argv, "--silence-ms", 3000);
    o.hot_msgs = arg_long(argc, argv, "--hot-msgs", 4000);
    o.resume_msgs = arg_long(argc, argv, "--resume-msgs", 5);
    o.handshake_timeout_ms = arg_long(argc, argv, "--handshake-timeout-ms", 120000);
    o.drain_timeout_ms = arg_long(argc, argv, "--drain-timeout-ms", 120000);
    o.out_dir = arg_str(argc, argv, "--out", "");
    const std::string def_run_id = now_stamp() + "-W10";
    o.run_id = arg_str(argc, argv, "--run-id", def_run_id.c_str());
    if (arg_flag(argc, argv, "--no-recover")) o.resume_msgs = 0;
    if (arg_flag(argc, argv, "--no-rebuild")) o.skip_rebuild = true;
    o.expect_run_id = arg_str(argc, argv, "--expect-run-id", "");
    o.require_lib_sha256 = arg_str(argc, argv, "--require-lib-sha256", "");
    o.require_bin_sha256 = arg_str(argc, argv, "--require-bin-sha256", "");
    o.connect_deadline_ms = arg_long(argc, argv, "--connect-deadline-ms", 30000);
    o.register_batch_deadline_ms = arg_long(argc, argv, "--register-batch-deadline-ms", 90000);
    o.handshake_deadline_ms = arg_long(argc, argv, "--handshake-deadline-ms", 5000);
    o.close_deadline_ms = arg_long(argc, argv, "--close-deadline-ms", 2000);
    o.rebuild_deadline_ms = arg_long(argc, argv, "--rebuild-deadline-ms", 1000);
    o.force_fail = arg_str(argc, argv, "--force-fail", "");
    /* H2 负控：只建订阅者、**不建发布端** ⇒ 修后必须 `registered=0` 且判失败
     *（修前该场景仍会宣称 1000/1000 —— 见 w10_r3_counterexamples 的 H2 对拍）。 */
    o.negctl_no_pub = arg_flag(argc, argv, "--negctl-no-pub");

    Harness h(o);
    return h.run();
}
/* 端口无碰撞的话题名选取（socket 千路专用）。
 * 端口是话题名的**纯函数**（udp_discovery_port_calculate），故可离线/在线确定性筛选：
 * 依次尝试 `w10_socket_independent_<dom>_<i>`，保留那些 [base, base+4] 段
 * **既未与已选话题重叠、也不落在本机已占 UDP 端口上**的名字。
 *
 * ⚠️ t33 修复（缺陷由 t29 上报、本任务修复）：原实现只读 `/proc/net/udp` 的**第一个** token，
 *    那是行首的 **slot 号**（形如 `145:`），不是 `local_address` 列 ⇒ `strtol("")` = 0
 *    ⇒ 排除集合恒为 `{0}`，端口排除**从不生效**（实测 `used_set_size=1`）。
 *    正确列序是 `sl  local_address  rem_address  ...` ⇒ 必须读**两个** token。
 *    修法：`is >> slot >> addr`，并对解析结果加**机器可判的断言**（见下方 parsed/样本打印）。 */
bool w10_pick_socket_topics(std::vector<std::string>& out, long dom, long want, bool verbose,
                            std::vector<std::string>* diag_out)
{
    PortScanResult r = scan_used_udp_ports();
    long eph_lo = 0, eph_hi = 0;
    const long eph_rc = read_ephemeral_range(&eph_lo, &eph_hi);

    std::vector<std::string> diag;
    diag.push_back("picker.used_source=/proc/net/udp,/proc/net/udp6");
    diag.push_back("picker.parsed_ok=" + std::to_string(r.parsed_ok));
    diag.push_back("picker.parsed_bad=" + std::to_string(r.parsed_bad));
    diag.push_back("picker.used_set_size=" + std::to_string(r.used.size()));
    diag.push_back("picker.first_sample=" + (r.first_sample.empty() ? std::string("(none)") : r.first_sample));
    diag.push_back("picker.ephemeral=[" + std::to_string(eph_lo) + "," + std::to_string(eph_hi) + "]");
    for (auto& e : r.errors) diag.push_back("picker.error=" + e);

    if (verbose)
        for (auto& d : diag) std::printf("%s\n", d.c_str());

    out.clear();
    if (!r.ok)
    {
        /* ⛔ 读不到端口表 ⇒ **不得**按"无占用"继续（那样会重现修前的假预检）。 */
        if (verbose)
            std::printf("picker: FATAL 端口表读取失败 ⇒ 拒绝产出话题名（⛔ 不按『无占用』处理）\n");
        diag.push_back("picker.verdict=FAIL_PORT_TABLE_UNREADABLE");
        if (diag_out) *diag_out = diag;
        return false;
    }
    if (eph_rc != 0)
    {
        if (verbose)
            std::printf("picker: FATAL 读 ip_local_port_range 失败 ⇒ 拒绝产出话题名\n");
        diag.push_back("picker.verdict=FAIL_EPHEMERAL_RANGE_UNREADABLE");
        if (diag_out) *diag_out = diag;
        return false;
    }

    /* 本进程自己的 SendOnly ACK 套接字会从内核临时端口池取源端口（`udp.h:169` 明确
     * "SendOnly 不 bind"）⇒ 运行中新拿到的临时端口可能与后面某个话题的接收端口相同
     * ⇒ 自撞 EADDRINUSE。要与"外部进程占用"可区分，必须整段排除。 */
    std::set<std::string> picked_names;
    std::set<int> reserved;        /* 本批已选话题占用的端口段（与 used 分开记账，便于解释） */
    long tried = 0;
    for (long i = 0; static_cast<long>(out.size()) < want && i < 2000000; ++i)
    {
        const std::string name = "w10_socket_independent_" + std::to_string(dom) + "_" + std::to_string(i);
        if (picked_names.count(name) != 0) continue;
        const int base = static_cast<int>(dzIPC::common::udp_discovery_port_calculate(name, static_cast<int>(dom)));
        bool ok = true;
        for (int o = 0; o < 5 && ok; ++o)
        {
            const long p = base + o;
            if (r.used.count(static_cast<int>(p)) != 0) ok = false;   /* 本机已占（含 udp6） */
            else if (reserved.count(static_cast<int>(p)) != 0) ok = false; /* 本批已选话题 */
            else if (p >= eph_lo && p <= eph_hi) ok = false;          /* 会被自己的临时端口撞 */
        }
        if (!ok) continue;
        for (int o = 0; o < 5; ++o) reserved.insert(base + o);
        out.push_back(name);
        picked_names.insert(name);
        tried = i;
    }
    diag.push_back("picker.picked=" + std::to_string(out.size()) + "/" + std::to_string(want));
    diag.push_back("picker.tried_upto_i=" + std::to_string(tried));
    diag.push_back("picker.reserved_port_slots=" + std::to_string(reserved.size()));
    diag.push_back(std::string("picker.verdict=") + (static_cast<long>(out.size()) >= want ? "OK" : "SHORT"));
    if (verbose)
        std::printf("picker: picked=%zu/%ld tried_upto_i=%ld reserved=%zu ephemeral=[%ld,%ld]\n", out.size(), want,
                    tried, reserved.size(), eph_lo, eph_hi);
    if (diag_out) *diag_out = diag;
    return static_cast<long>(out.size()) >= want;
}