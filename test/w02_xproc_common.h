#pragma once
/* W02 统一跨进程基准 · 公共定义（控制块 / 配置 / 载荷 / 逐样本记录）
 * ============================================================================
 * 交付依据：方案 §4 W02 全部七条 + §10.6（构建与路径假通过防线）+ §12（证据目录
 * 与逐样本字段）。父进程编排与两个角色进程（pub / sub）都由 test/xproc_benchmark.cpp
 * 一个二进制承担，用 `--role=` 分派；本文件只放两边都要用的定义。
 *
 * 三条硬边界（§10.7，禁止混用结束点）：
 *   transport_ns = transport_done_ns - publish_enter_ns   （发布侧本地，不随消息走）
 *   delivery_ns  = app_obtained_ns   - transport_done_ns  （跨进程，靠单调时钟同源）
 *   app_read_ns  = fully_consumed_ns - app_obtained_ns    （消费侧工作负载）
 *   e2e_ns       = fully_consumed_ns - produced_ns        （生产到消费）
 * transport_done_ns **不可能**随同一条消息送达（发送返回时消息已离开发布进程），
 * 因此本文件不把它放进消息头；head 只带 produced/enter，由父进程按 seq 合并。
 *
 * 跨进程身份可核验（§10.6 + W03 口径）：
 *   · 两个角色进程都是 exec 出来的**新映像**（不是 fork 后共享状态的线程）；
 *   · 各自把 `role_ready_ns`（本进程 CLOCK_MONOTONIC）写进共享控制块，
 *     父进程用"spawn 前/ready 后"两次读数把该值夹住 —— 时基不同源必然失败；
 *   · pid + /proc/<pid>/stat 的 starttime 一起登记，防 pid 复用；
 *   · 角色进程实际加载的 libipc.so 路径与 sha256 进 manifest。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dzIPC/measure/monotonic_clock.h"

namespace w02 {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

inline u64 now_ns() noexcept { return dzIPC::measure::monotonic_now_ns(); }

/* ------------------------------------------------------------------ 配置 */
enum class Backend
{
    dzipc,   /* SHM */ 
    dds,     /* CycloneDDS */
};

enum class PathSel
{
    tlv,        /* dzIPC SHM + TLV   */
    dzflat_a,   /* dzIPC SHM + DZFlat A（对象 → 共享段一次复制） */
    dzflat_b,   /* dzIPC SHM + DZFlat B（loan → 应用原地构造 → publish_loaned） */
    cyc_udp,    /* CycloneDDS，SharedMemory 关闭 */
    cyc_iox,    /* CycloneDDS，SharedMemory 开启（iceoryx/RouDi） */
};

enum class Workload
{
    timestamp,  /* §10.7 组1：只读时间戳/头部，不解全量载荷 */
    crc,        /* §10.7 组2：全量遍历并核对逐样本校验值（单遍） */
    full,       /* §10.7 组2 强化：逐元素与序号相关模式比对 */
    inplace,    /* §10.7 组3：生产端原地构造（仅 DZFlat B 有效） */
};

enum class WaitMode
{
    blocking,  /* 主对照：队列阻塞/定时阻塞等待（事件驱动） */
    busy,      /* 单列组：忙等（无睡眠），必须单独报告 CPU */
};

enum class VerifyMode
{
    crc,
    full,
};

inline const char* path_name(PathSel p) noexcept
{
    switch (p)
    {
    case PathSel::tlv:      return "tlv";
    case PathSel::dzflat_a: return "dzflat-a";
    case PathSel::dzflat_b: return "dzflat-b";
    case PathSel::cyc_udp:  return "cyclonedds-udp";
    case PathSel::cyc_iox:  return "cyclonedds-iox";
    }
    return "unknown";
}

inline const char* workload_name(Workload w) noexcept
{
    switch (w)
    {
    case Workload::timestamp: return "timestamp";
    case Workload::crc:       return "crc";
    case Workload::full:      return "full";
    case Workload::inplace:   return "inplace";
    }
    return "unknown";
}

inline const char* wait_name(WaitMode w) noexcept
{
    return w == WaitMode::blocking ? "blocking" : "busy";
}

inline bool parse_path(const std::string& s, PathSel& out) noexcept
{
    if (s == "tlv") { out = PathSel::tlv; return true; }
    if (s == "dzflat-a" || s == "dzflat_a") { out = PathSel::dzflat_a; return true; }
    if (s == "dzflat-b" || s == "dzflat_b") { out = PathSel::dzflat_b; return true; }
    if (s == "cyc-udp" || s == "cyc_udp" || s == "cyclonedds-udp") { out = PathSel::cyc_udp; return true; }
    if (s == "cyc-iox" || s == "cyc_iox" || s == "cyclonedds-iox") { out = PathSel::cyc_iox; return true; }
    return false;
}

/* 从 DZFlat 借用得到的载荷预算（变长区上界，留足对齐与段头余量）。 */
inline u32 loan_budget(u64 payload_bytes) noexcept
{
    return static_cast<u32>(payload_bytes + 8192u);
}

/* -------------------------------------------------------------- 控制块
 * POSIX 共享内存里的一块 POD，父进程创建，两个角色进程按名字打开。
 * 只有 plain atomic 成员 + placement new 初始化（与 test/dzipc_perf_benchmark.cpp
 * 的 SharedCtl 同构，那是本仓既有做法）。
 */
struct ControlBlock
{
    std::atomic<u32> magic;
    std::atomic<i32> phase;      /* 0=init 1=handshake 2=warmup 3=measure 4=drain 5=stop */
    std::atomic<i32> sub_ready;
    std::atomic<i32> pub_ready;
    std::atomic<i32> sub_failed;
    std::atomic<i32> pub_failed;
    std::atomic<i32> probe_received;   /* 数据面握手：订阅侧真的收到了探测帧 */
    std::atomic<i32> stop_received;    /* 订阅侧收到停止哨兵帧 */
    std::atomic<i32> sub_stop_request; /* 父进程要求订阅侧收尾（超时/异常） */
    std::atomic<i32> run_failed;       /* 任一角色自报失败 */
    std::atomic<i32> sub_done;         /* 订阅侧已正常收尾（发布侧等它再释放通道） */

    std::atomic<u64> sub_ready_ns;
    std::atomic<u64> pub_ready_ns;
    std::atomic<u64> sub_ready_bracket_lo;   /* 父进程 spawn 前读数 */
    std::atomic<u64> sub_ready_bracket_hi;   /* 父进程观察到 ready 后读数 */
    std::atomic<u64> pub_ready_bracket_lo;
    std::atomic<u64> pub_ready_bracket_hi;
    std::atomic<u64> measure_start_ns;
    std::atomic<u64> measure_end_ns;
    std::atomic<u64> pub_finish_ns;

    /* 发送计划（§13.1：计划独立于接收完成） */
    std::atomic<u64> plan_total;
    std::atomic<u64> attempt_total;
    std::atomic<u64> sent_ok;
    std::atomic<u64> send_failed;
    std::atomic<u64> send_blocked;      /* publish 因队列/等待而阻塞（有界等待超时） */
    std::atomic<u64> send_retried;
    std::atomic<u64> late_sends;        /* 迟发次数（超过 late_threshold_ns） */
    std::atomic<u64> late_threshold_ns; /* 迟发判定阈值: max(50us, 周期/10)，输出到 summary */
    std::atomic<u64> backlog_sum_ns;    /* 迟发量累计（积压度量） */
    std::atomic<u64> backlog_max_ns;
    std::atomic<u64> stop_frames_sent;
    std::atomic<u64> stop_frames_recv;

    std::atomic<u64> sub_received_total;
    std::atomic<u64> sub_checksum_bad;
    std::atomic<u64> sub_bad_header;    /* 段头/对象结构自相矛盾 */
    std::atomic<u64> sub_wrong_seq;     /* 序号与模式不符（旧帧/重复） */
    std::atomic<u64> sub_probe_count;
    std::atomic<u64> sub_first_rx_ns;
    std::atomic<u64> sub_unavailable;   /* 该后端/路径不可用的显式标记 */

    /* 各角色的 DZFlat 路径计数（进程级计数器导出） */
    std::atomic<u64> pub_dzflat;
    std::atomic<u64> pub_fallback;
    std::atomic<u64> sub_rx_dzflat_accepted;
    std::atomic<u64> sub_rx_tlv_accepted;
    std::atomic<u64> sub_rx_defects;
    std::atomic<u64> sub_rx_id_skipped;

    /* 角色进程自报身份（跨进程可核验） */
    std::atomic<i32> pub_pid;
    std::atomic<i32> sub_pid;
    std::atomic<u64> pub_starttime_ticks;
    std::atomic<u64> sub_starttime_ticks;

    char pub_loaded_lib[256];
    char sub_loaded_lib[256];
};

constexpr u32 kCtlMagic = 0x5730'3242u;   /* 'W','0','2','B' */

inline ControlBlock* ctl_map(const std::string& name, bool create)
{
    const int flags = create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    const int fd = ::shm_open(name.c_str(), flags, 0600);
    if (fd < 0)
    {
        std::fprintf(stderr, "[fatal] shm_open(%s) failed: %s\n", name.c_str(), std::strerror(errno));
        return nullptr;
    }
    if (create && ::ftruncate(fd, static_cast<off_t>(sizeof(ControlBlock))) != 0)
    {
        std::fprintf(stderr, "[fatal] ftruncate failed: %s\n", std::strerror(errno));
        ::close(fd);
        return nullptr;
    }
    void* p = ::mmap(nullptr, sizeof(ControlBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED)
    {
        std::fprintf(stderr, "[fatal] mmap ctl failed: %s\n", std::strerror(errno));
        return nullptr;
    }
    if (create)
    {
        ControlBlock* c = new (p) ControlBlock;
        c->magic.store(kCtlMagic);
        c->phase.store(0);
        c->sub_ready.store(0); c->pub_ready.store(0);
        c->sub_failed.store(0); c->pub_failed.store(0);
        c->probe_received.store(0); c->stop_received.store(0);
        c->sub_stop_request.store(0); c->run_failed.store(0);
        c->sub_done.store(0);
        c->sub_ready_ns.store(0); c->pub_ready_ns.store(0);
        c->sub_ready_bracket_lo.store(0); c->sub_ready_bracket_hi.store(0);
        c->pub_ready_bracket_lo.store(0); c->pub_ready_bracket_hi.store(0);
        c->measure_start_ns.store(0); c->measure_end_ns.store(0); c->pub_finish_ns.store(0);
        c->plan_total.store(0); c->attempt_total.store(0); c->sent_ok.store(0);
        c->send_failed.store(0); c->send_blocked.store(0); c->send_retried.store(0);
        c->late_sends.store(0); c->late_threshold_ns.store(0);
        c->backlog_sum_ns.store(0); c->backlog_max_ns.store(0);
        c->stop_frames_sent.store(0); c->stop_frames_recv.store(0);
        c->sub_received_total.store(0); c->sub_checksum_bad.store(0);
        c->sub_bad_header.store(0); c->sub_wrong_seq.store(0); c->sub_probe_count.store(0);
        c->sub_first_rx_ns.store(0); c->sub_unavailable.store(0);
        c->pub_dzflat.store(0); c->pub_fallback.store(0);
        c->sub_rx_dzflat_accepted.store(0); c->sub_rx_tlv_accepted.store(0);
        c->sub_rx_defects.store(0); c->sub_rx_id_skipped.store(0);
        c->pub_pid.store(0); c->sub_pid.store(0);
        c->pub_starttime_ticks.store(0); c->sub_starttime_ticks.store(0);
        std::memset(c->pub_loaded_lib, 0, sizeof(c->pub_loaded_lib));
        std::memset(c->sub_loaded_lib, 0, sizeof(c->sub_loaded_lib));
    }
    return static_cast<ControlBlock*>(p);
}

inline void ctl_unmap(ControlBlock* c)
{
    if (c) ::munmap(c, sizeof(ControlBlock));
}

/* ---------------------------------------------------- 逐样本原始记录
 * 两个角色各写自己的原始 CSV（父进程合并成样本表）。
 * 发布/订阅**分开**落盘的理由：发布侧的 end 点与订阅侧的 begin 点属于两个进程，
 * 只有父进程能按 seq 合并；也让"发送了没收到"在原始文件里一眼可见。
 */
struct PubRow
{
    u64 seq{0};
    u32 idx{0};
    u32 flags{0};
    u64 produced_ns{0};
    u64 enter_ns{0};
    u64 done_ns{0};
    u32 app_bytes{0};
    u64 wire_bytes{0};
    /* wire 字节是否**实测**。0 是一个合法测量值（本仓没有 0 字节的 wire，但语义上不排除），
     * 所以"未采集"必须用独立标志表达，不能靠值 0 区分 —— 否则 CSV 里 0 会同时表示
     * "实测 0" 与 "未测"，正是 W03 §2.1 禁止的"用 0 冒充 null"。 */
    std::uint8_t wire_known{1};
    i32 ok{0};
    i32 publish_rc{0};    /* 0=成功 1=失败(超时/无接收方) */
    u64 backlog_ns{0};
    u64 retry{0};
};
inline const char* pub_csv_header()
{
    return "seq,idx,flags,produced_ns,enter_ns,done_ns,app_bytes,wire_bytes,ok,publish_rc,backlog_ns,retry";
}

struct SubRow
{
    u64 seq{0};
    u32 idx{0};
    u32 flags{0};
    u64 app_obtained_ns{0};
    u64 fully_consumed_ns{0};
    i32 checksum_ok{-1};   /* -1 未校验（timestamp 工作负载） */
    u32 elements_checked{0};
    u64 rx_wire_bytes{0};
    std::uint8_t rx_wire_known{1};   ///< 同 PubRow::wire_known：未采集时 CSV 写空字段
    i32 abnormal{0};       /* 结构/校验异常 */
    i32 view_kind{0};      /* 0=object 1=flat-view 2=dds */
};
inline const char* sub_csv_header()
{
    return "seq,idx,flags,app_obtained_ns,fully_consumed_ns,checksum_ok,elements_checked,"
           "rx_wire_bytes,abnormal,view_kind";
}

/* ------------------------------------------------------------------ 小工具 */
inline std::string jstr(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) { o += ' '; }
        else o += c;
    }
    return o;
}

inline std::string json_u64_or_null(const std::vector<u64>& v, double q)
{
    if (v.empty()) return "null";
    std::vector<u64> s = v;
    std::sort(s.begin(), s.end());
    const std::size_t i = std::min(s.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.size())));
    return std::to_string(s[i]);
}

inline std::string json_num_or_null(double v, bool has)
{
    if (!has) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

/* 运行外部命令并取一行输出（sha256sum / pgrep 等）。失败返回空串。 */
inline std::string run_capture(const std::string& cmd)
{
    std::string out;
    std::FILE* f = ::popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f))
    {
        out += buf;
        if (out.size() > 4096) break;
    }
    ::pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

inline std::string sha256_of_file(const std::string& path)
{
    if (path.empty()) return std::string();
    const std::string out = run_capture("sha256sum '" + path + "' 2>/dev/null");
    const std::size_t sp = out.find(' ');
    return sp == std::string::npos ? std::string() : out.substr(0, sp);
}

/* 本进程实际加载的某个库路径（从 /proc/self/maps 里取第一条匹配）。 */
inline std::string loaded_lib_path(const std::string& needle)
{
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return std::string();
    char line[1024];
    std::string found;
    while (std::fgets(line, sizeof(line), f))
    {
        if (std::strstr(line, needle.c_str()))
        {
            char* p = std::strchr(line, '/');
            if (p)
            {
                found = p;
                while (!found.empty() && (found.back() == '\n' || found.back() == '\r')) found.pop_back();
                break;
            }
        }
    }
    std::fclose(f);
    return found;
}

}   // namespace w02
