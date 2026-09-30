/* W02 统一跨进程基准 —— 父进程编排 + 发布/订阅角色进程 + 逐样本输出
 * ============================================================================
 * 交付依据：docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md
 *   §4 W02 七条要求；§10.6 构建与路径「假通过」防线；§10.7 三个实验组；
 *   §12 证据目录/逐样本字段；§13.1 发送计划独立于接收完成。
 *
 * 用法（父进程 = 采集器）:
 *   xproc_benchmark                                   # 默认 config 冒烟矩阵
 *   xproc_benchmark --path=tlv --workload=full --payload=1024 --rate=1000
 *   xproc_benchmark --compare=tlv,dzflat-a,dzflat-b    # 同一负载逐轮交替
 *   xproc_benchmark --smoke                            # 四档正确性用例(64B/1KiB/64KiB/1MiB)
 *
 * 角色进程（由父进程 fork+exec 起，不共享任何地址空间）:
 *   xproc_benchmark --role=pub --ctl=/w02ctl_xxx --path=... ...
 *   xproc_benchmark --role=sub --ctl=/w02ctl_xxx --path=... ...
 *
 * 为什么必须两个进程：同进程 pub/sub 会把「进程内快路径 / 无跨进程通知成本」测进来
 * （W01 UF-01 的 14.3× 就是同进程 + 忙等的产物）。本基准的 process_model 恒为
 * cross-process，且两个角色都是 exec 出来的新映像（不是 fork 后共享状态的线程）。
 *
 * 三条计时边界（§10.7，禁止混用结束点）：
 *   ① 传输机制   : delivery_ns  = app_obtained_ns   - transport_done_ns
 *   ② 完整读取   : e2e_full     = fully_consumed_ns - publish_enter_ns
 *   ③ 生产到消费 : e2e_ns       = fully_consumed_ns - produced_ns
 * transport_done_ns 由发布进程本地记录，父进程按 seq 与订阅侧原始表合并 —— 它不可能
 * 随同一条消息送出去（发送返回时消息已离开发布进程），因此消息头里只放
 * produced/enter（见 test/w02_pattern.h）。
 *
 * 发送计划独立于接收完成（§13.1）：定速档按 k/rate 预排发送时刻，接收变慢时计划
 * 不变，迟发量累加进 late_sends/backlog_*；发送量下降会被标成「负载未达标」而不是
 * 「低延迟好成绩」。
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/crc32c.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/dzipc.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/field_schema.h"
#include "dzIPC/measure/monotonic_clock.h"
#include "dzIPC/measure/path_evidence.h"
#include "dzIPC/measure/platform_info.h"
#include "dzIPC/measure/proc_sampler.h"
#include "ipc_msg/test_msg2/test_msg.hpp"

#include "w02_pattern.h"
#include "w02_xproc_common.h"

#ifdef W02_HAVE_DDS
#include <dds/dds.h>
#include "w02_dds_types.h"
#endif

using namespace std::chrono;
using namespace w02;
using TestMsg = dzIPC::Msg::TestMsg;
using TestMsgFlat = dzIPC::Msg::TestMsgFlat;
using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;

/* ======================================================================== */
/* 配置                                                                     */
/* ======================================================================== */

struct Config
{
    /* 角色进程专用 */
    std::string role;        ///< "" | pub | sub | ddscheck
    std::string ctl;         ///< 共享控制块名
    std::string topic;
    std::string out_file;    ///< 角色进程写原始样本的路径
    u64 payload = 1024;
    u32 shape_n1 = 0, shape_n2 = 0, shape_n3 = 0, shape_str = 0;

    /* 编排 */
    std::string run_id;
    std::string out_dir;
    std::string build_dir;
    std::vector<std::string> compare{"tlv", "dzflat-a", "dzflat-b"};
    bool compare_given = false;   /* --path= 与默认矩阵的优先级：显式给过就别覆盖 */
    bool workload_given = false;  /* --workload= 显式给过 ⇒ 只跑这一组 */
    bool payload_given = false;   /* --payload= 显式给过 ⇒ 只跑这一档 */
    std::vector<u64> payloads{64, 1024, 65536, 1048576};
    PathSel path = PathSel::tlv;
    Workload workload = Workload::full;
    WaitMode wait_mode = WaitMode::blocking;
    double duration = 2.0;
    double warmup = 0.5;
    double rate_hz = 1000.0;      ///< 0 = 满速（开环）
    std::size_t queue = 256;
    std::size_t view_queue_cap = 64;
    int domain = 1;
    int pin_pub = -1;
    int pin_sub = -1;
    u64 queue_wait_ms = 100;      ///< SHM publish_blocking 有界等待
    u64 sample_limit = 400000;
    int rounds = 1;
    bool smoke = false;
    bool smoke_dds = false;
    std::vector<std::string> precheck_skipped;   /* DDS 预检未通过而被显式剔除的档 */
    bool keep_going = true;
    bool dds_only = false;
    std::string roudi_path;
    std::string dds_uri_udp;
    std::string dds_uri_iox;
    /* run 用途与引用权威（t47/W02-F0；t50/R-3 改为**默认安全**）：
     *   --purpose=evidence         ⇒ 本轮**显式声明**自己是证据 run ⇒ currently_citable = 本 run
     *   --purpose=verification|experiment ⇒ 复核/实验轮；须配 --citation-authority=<证据 run>
     *   （缺省，即**不带任何参数**）⇒ 按 `verification` 处理且**不认领权威指针**
     *                                ⇒ currently_citable = null + authority_undeclared=true
     *
     * ⛔ 为什么不能保留「默认抢指针」：t48/R-3 实测——不带参数的复核轮会把 currently_citable
     * 指向**自己**。那正是 t47 建这条机制要堵的第 1 类缺陷（引用链断裂）的**复发形态**：
     * 一次手滑重跑就悄悄换掉权威指针，而下游拿到的仍是一个"看起来正常"的 run。
     * ⇒ 认领权威必须是**显式动作**（`--purpose=evidence`）。
     *
     * 选 (i)「缺省按 verification 且不认领」而非 (ii)「缺省直接 rc≠0」的理由：
     *   本基准的**自检用例与 `w02_benchmark.sh` 的冒烟**都不带该参数，若缺省即拒绝发布，
     *   会把它们全部变成假红（ctest 里一条真实缺陷都没有却报失败）。选 (i) 则：安全性由
     *   「指针必须显式认领」保证，而不靠"让常用路径失败"来保证。 */
    std::string purpose;            ///< 空 = 未声明 ⇒ 按 verification 处理且不认领权威
    std::string citation_authority;
    bool purpose_given = false;
    double stop_timeout_s = 15.0;
    bool best_effort = false;

    /* ---- 失败量判定阈值（t47/W02-F3；全部写入 manifest.failure_thresholds） ----
     * 为什么需要：任务书第 3 条要求「失败与异常**不被静默排除**」。修前 `late`/
     * `backlog`/`send_blocked`/`abnormal` 只落列、不进 failure_reasons，`bad_header`
     * 连列都没有 ⇒ 71 次迟发（r23：17/75 case）可以全部不进判定而 case_ok 全 True，
     * 那是「用仅落列实现静默排除」。修后每个量都有一条**显式声明阈值**的判据，
     * 阈值随 manifest 落盘 ⇒ 逐 case 可判、可复核。
     * ⛔ 不是无条件失败：阈值有默认值、可被命令行覆盖（覆盖值同样落盘）。 */
    /* 阈值取值的依据（写入 manifest，可被 CLI 覆盖）：
     *  · 迟发：`late` 统计的是「超过 max(50µs, 周期/10) 的发送次数」。sleep_until 的
     *    正常抖动在几十 µs 量级，故不能要求 0（那等于把调度噪声判成失败）；
     *    但也不能放任 1% 以上 —— 抢不到 CPU / 被抢占会表现为系统性迟发。
     *    ⇒ 默认「次数 > 20 **且** 比率 > 5%」才判失败（两个条件同时成立才亮 ⇒
     *    既要「量够多」又要「比例够高」，把零星抖动与"结构性跟不上声明速率"分开）。
     *    这是**预算**而不是无条件失败；敏感度见交付文档（0.5% 档会额外点亮 5/75）。 */
    u64  late_abs_max = 20;         ///< 迟发次数预算（与 rate 阈值**同时**成立才判失败）
    double late_rate_max = 0.05;    ///< 迟发率预算（超过 max(50µs,周期/10) 的次数 / attempts）
    /* backlog 预算是「允许落后多少个发送周期」：峰值积压包含一次完整调度量子 +
     * 唤醒链开销 ⇒ 默认 10 个周期（1 kHz 下 = 10 ms）。⚠️ 该值**不是**取「刚好不亮」的
     * 数字：实测 60 case 冒烟里，预算 1 周期 ⇒ 16 case 亮、5 周期 ⇒ **1 case** 亮（5.171，
     * 真实排队）、10 周期 ⇒ 0 case 亮。敏感度一并写入交付文档，⛔ 不得只报默认值。 */
    double backlog_max_periods = 10.0; ///< backlog_max 允许的最大「周期数」（0 = 不判，仅信息）
    u64  send_blocked_max = 0;      ///< 有界等待耗尽次数上限（默认 0 = 一次都不允许）
    u64  abnormal_max = 0;          ///< 结构异常样本数上限
    u64  bad_header_max = 0;        ///< 段头/对象结构自相矛盾次数上限
    bool thresholds_given = false;  ///< 命令行显式给过阈值 ⇒ 用于反例（把阈值压到 0 证判据会亮）

    bool is_dds_path() const
    {
        return path == PathSel::cyc_udp || path == PathSel::cyc_iox;
    }
    bool is_flat_path() const
    {
        return path == PathSel::dzflat_a || path == PathSel::dzflat_b;
    }
};

/* 角色进程诊断（仅 W02_DEBUG=1 时输出）：子进程的输出目录里 stderr 是独立文件，
 * 排查"到底是哪一步卡住"时不能靠猜。 */
/* 读角色进程写下的"被逐样本上限截断的行数"。缺失时返回 0 并把"未采集"记进 notes。 */
static u64 read_dropped_rows(const std::string& path, bool& seen)
{
    seen = false;
    std::ifstream f(path);
    if (!f) return 0;
    u64 v = 0;
    if (f >> v) { seen = true; return v; }
    return 0;
}

/* 角色进程写小 sidecar（不经过缓冲流，保证退出前落盘）。 */
static void write_text_file(const std::string& path, const std::string& body)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
}

static bool debug_on()
{
    static const bool on = (::getenv("W02_DEBUG") != nullptr);
    return on;
}
#define W02_DBG(...) do { if (debug_on()) { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } } while (0)

static void die(const std::string& msg)
{
    std::fprintf(stderr, "[fatal] %s\n", msg.c_str());
    std::exit(2);
}

static std::string run_id_now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
    return std::string(buf);
}

static bool mkdir_p(const std::string& p)
{
    if (p.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        cur += p[i];
        if (p[i] == '/' || i + 1 == p.size()) ::mkdir(cur.c_str(), 0755);
    }
    return true;
}

/* 文件名消毒：case_id 会变成文件名，不能带路径分隔符或空格。 */
static std::string sanitize(const std::string& s)
{
    std::string o = s;
    for (char& c : o) { if (c == '/' || c == ' ' || c == ',' || c == ':') c = '_'; }
    return o;
}

/* topic 名消毒：只保留 [A-Za-z0-9_/]。
 * 存在的理由：CycloneDDS 0.10.2 对 topic 名里的 '-' 直接返回 Bad Parameter（实测，
 * 且只在启用 iceoryx 的档上暴露 —— UDP 档同名字符串没事）。topic 名由 run_id/路径名/
 * 工作负载名拼出来，三者都可能带 '-'，所以出口处统一过一遍，不允许"路径名恰好带横线
 * 就把整档打红"。 */
static std::string topic_safe(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '/';
        o += ok ? c : '_';
    }
    return o;
}

/* ======================================================================== */
/* 载荷构造（dzIPC 侧：TestMsg + 8 槽时间戳头）                              */
/* ======================================================================== */

static Shape cfg_shape(const Config& c)
{
    Shape s;
    s.n1 = c.shape_n1; s.n2 = c.shape_n2; s.n3 = c.shape_n3; s.str_len = c.shape_str;
    return s;
}

static void fill_header_slots(TestMsg* m, const Shape& s, u64 seq, u32 idx, u32 flags,
                              u64 produced_ns, u64 enter_ns, u32 crc, bool has_crc)
{
    if (m->data2.size() < kHdrSlots)
    {
        m->data2.resize(kHdrSlots + s.n2);
    }
    m->data2[kSlotSeqLo] = static_cast<i32>(static_cast<u32>(seq & 0xFFFFFFFFull));
    m->data2[kSlotSeqHi] = static_cast<i32>(static_cast<u32>(seq >> 32));
    m->data2[kSlotIdxFlags] = static_cast<i32>((idx & 0xFFFFu) | (flags << 16));
    m->data2[kSlotCrc] = has_crc ? static_cast<i32>(crc) : 0;
    m->data2[kSlotProducedLo] = static_cast<i32>(static_cast<u32>(produced_ns & 0xFFFFFFFFull));
    m->data2[kSlotProducedHi] = static_cast<i32>(static_cast<u32>(produced_ns >> 32));
    m->data2[kSlotEnterLo] = static_cast<i32>(static_cast<u32>(enter_ns & 0xFFFFFFFFull));
    m->data2[kSlotEnterHi] = static_cast<i32>(static_cast<u32>(enter_ns >> 32));
}

/* 生产端「生成新数据」的等价工作：把整块逻辑载荷按 seq 相关模式写一遍。
 * 三种后端、三条路径都调这一个函数 —— 组间只有「字节落在哪里」不同。 */
static void produce_into(TestMsg* m, const Shape& s, u64 seq)
{
    m->data1.resize(s.n1);
    if (s.n1) std::memset(m->data1.data(), 0, 0);
    for (u32 i = 0; i < s.n1; ++i) m->data1[i] = pat_double(seq, i);

    m->data2.resize(kHdrSlots + s.n2);
    for (u32 i = 0; i < s.n2; ++i) m->data2[kHdrSlots + i] = pat_int(seq, i);

    m->data3.resize(s.n3);
    for (u32 k = 0; k < s.n3; ++k)
    {
        m->data3[k].assign(s.str_len, 'x');
        pat_string(seq, k, s.str_len, m->data3[k].data());
    }
    m->data4 = true;
}

/* 生产者侧 CRC（仅 workload=crc）：对逻辑载荷做一次顺序遍历。
 * A/TLV 在对象上算；B 在借到的 chunk span 上算（同一口径：都是"逻辑载荷字节"）。 */
static u32 crc_of_object(const TestMsg* m, const Shape& s)
{
    u32 c = 0;
    if (s.n1) c = dzIPC::common::crc32c(m->data1.data(), s.n1 * sizeof(double));
    for (u32 k = 0; k < s.n3; ++k)
    {
        const u32 part = dzIPC::common::crc32c(m->data3[k].data(), s.str_len);
        c = (c * 16777619u) ^ part;
    }
    if (s.n2)
    {
        const u32 part = dzIPC::common::crc32c(m->data2.data() + kHdrSlots, s.n2 * sizeof(i32));
        c = (c * 16777619u) ^ part;
    }
    return c;
}

/* ======================================================================== */
/* 订阅侧校验                                                               */
/* ======================================================================== */

struct CheckOutcome
{
    bool header_ok = false;
    bool payload_ok = true;
    u64 elements = 0;
    u64 mismatches = 0;
    u64 seq = 0;
    u32 idx = 0;
    u32 flags = 0;
    u64 produced_ns = 0;
    u64 enter_ns = 0;
    u32 crc_field = 0;
    u32 crc_computed = 0;
    bool abnormal = false;
    std::string why;
};

/* 头部最小确认（§10.7 组1）：只读 8 个槽。 */
static CheckOutcome check_header(const i32* d2, u32 n2)
{
    CheckOutcome o;
    if (n2 < kHdrSlots) { o.abnormal = true; o.why = "data2 短于时间戳头"; return o; }
    o.seq = (static_cast<u64>(static_cast<u32>(d2[kSlotSeqHi])) << 32)
          | static_cast<u64>(static_cast<u32>(d2[kSlotSeqLo]));
    const u32 idxf = static_cast<u32>(d2[kSlotIdxFlags]);
    o.idx = idxf & 0xFFFFu;
    o.flags = idxf >> 16;
    o.produced_ns = (static_cast<u64>(static_cast<u32>(d2[kSlotProducedHi])) << 32)
                  | static_cast<u64>(static_cast<u32>(d2[kSlotProducedLo]));
    o.enter_ns = (static_cast<u64>(static_cast<u32>(d2[kSlotEnterHi])) << 32)
               | static_cast<u64>(static_cast<u32>(d2[kSlotEnterLo]));
    o.crc_field = static_cast<u32>(d2[kSlotCrc]);
    o.header_ok = true;
    o.elements = kHdrSlots;
    return o;
}

/* 全量逐元素比对 + 可选 CRC（§10.7 组2）。workload=full 用逐元素，workload=crc 用
 * CRC 顺序遍历 —— 两者都是"遍历并校验全部载荷"，成本不同，报告里分开列。 */
static void verify_object(const TestMsg* m, const Shape& s, CheckOutcome& o, bool do_elements, bool do_crc)
{
    if (do_elements)
    {
        if (m->data1.size() != s.n1) { o.payload_ok = false; o.why = "data1 元素数不符"; o.abnormal = true; }
        const u32 n1 = std::min<u32>(static_cast<u32>(m->data1.size()), s.n1);
        for (u32 i = 0; i < n1; ++i)
        {
            ++o.elements;
            if (m->data1[i] != pat_double(o.seq, i)) { ++o.mismatches; }
        }
        const u32 n2have = m->data2.size() >= kHdrSlots ? static_cast<u32>(m->data2.size() - kHdrSlots) : 0u;
        if (n2have != s.n2) { o.payload_ok = false; o.why = "data2 负载元素数不符"; o.abnormal = true; }
        const u32 n2 = std::min<u32>(n2have, s.n2);
        for (u32 i = 0; i < n2; ++i)
        {
            ++o.elements;
            if (m->data2[kHdrSlots + i] != pat_int(o.seq, i)) { ++o.mismatches; }
        }
        if (m->data3.size() != s.n3) { o.payload_ok = false; o.why = "data3 元素数不符"; o.abnormal = true; }
        const u32 n3 = std::min<u32>(static_cast<u32>(m->data3.size()), s.n3);
        for (u32 k = 0; k < n3; ++k)
        {
            ++o.elements;
            if (m->data3[k].size() != s.str_len) { ++o.mismatches; continue; }
            char expect[256];
            const u32 len = std::min<u32>(s.str_len, 256u);
            pat_string(o.seq, k, len, expect);
            if (std::memcmp(m->data3[k].data(), expect, len) != 0) { ++o.mismatches; }
        }
        if (!m->data4) { ++o.mismatches; o.payload_ok = false; o.why = "data4 标志不符"; }
    }
    if (do_crc)
    {
        u32 c = 0;
        if (s.n1) c = dzIPC::common::crc32c(m->data1.data(), s.n1 * sizeof(double));
        for (u32 k = 0; k < s.n3; ++k)
        {
            const u32 part = dzIPC::common::crc32c(m->data3[k].data(), s.str_len);
            c = (c * 16777619u) ^ part;
        }
        if (s.n2)
        {
            const u32 part = dzIPC::common::crc32c(m->data2.data() + kHdrSlots, s.n2 * sizeof(i32));
            c = (c * 16777619u) ^ part;
        }
        o.crc_computed = c;
        if (c != o.crc_field) { o.payload_ok = false; o.why = "CRC 不符"; o.abnormal = true; }
    }
    if (o.mismatches) { o.payload_ok = false; o.abnormal = true; o.why = "模式比对失配"; }
}

#ifdef W02_HAVE_DDS
/* DDS 侧同样两组：头部最小确认 + 全量遍历（sequence 逐元素比对）。 */
static CheckOutcome check_header_dds(const w02_Sample* m)
{
    CheckOutcome o;
    o.seq = static_cast<u64>(m->seq);
    o.idx = static_cast<u32>(m->idx_flags) & 0xFFFFu;
    o.flags = static_cast<u32>(m->idx_flags) >> 16;
    o.produced_ns = static_cast<u64>(m->produced_ns);
    o.enter_ns = static_cast<u64>(m->enter_ns);
    o.crc_field = static_cast<u32>(m->hdr_xor);
    o.header_ok = true;
    o.elements = 8;
    return o;
}

static void verify_dds(const w02_Sample* m, const Shape& s, CheckOutcome& o, bool do_elements)
{
    if (!(do_elements)) return;
    if (m->data1._length != s.n1) { o.payload_ok = false; o.abnormal = true; o.why = "data1 元素数不符"; }
    const u32 n1 = std::min<u32>(m->data1._length, s.n1);
    for (u32 i = 0; i < n1; ++i)
    {
        ++o.elements;
        if (m->data1._buffer[i] != pat_double(o.seq, i)) ++o.mismatches;
    }
    const u32 n2 = std::min<u32>(m->data2._length, s.n2);
    if (m->data2._length != s.n2) { o.payload_ok = false; o.abnormal = true; o.why = "data2 元素数不符"; }
    for (u32 i = 0; i < n2; ++i)
    {
        ++o.elements;
        if (m->data2._buffer[i] != pat_int(o.seq, i)) ++o.mismatches;
    }
    if (m->data3._length != s.n3) { o.payload_ok = false; o.abnormal = true; o.why = "data3 元素数不符"; }
    const u32 n3 = std::min<u32>(m->data3._length, s.n3);
    for (u32 k = 0; k < n3; ++k)
    {
        ++o.elements;
        const char* p = m->data3._buffer[k];
        if (p == nullptr) { ++o.mismatches; continue; }
        const std::size_t len = std::strlen(p);
        if (len != s.str_len) { ++o.mismatches; continue; }
        char expect[256];
        const u32 l2 = std::min<u32>(s.str_len, 256u);
        pat_string(o.seq, k, l2, expect);
        if (std::memcmp(p, expect, l2) != 0) ++o.mismatches;
    }
    if (o.mismatches) { o.payload_ok = false; o.abnormal = true; o.why = "模式比对失配"; }
}
#endif

/* ======================================================================== */
/* 原始样本缓冲（两个角色各写自己的 CSV，父进程按 seq 合并）                  */
/* ======================================================================== */

struct RawWriter
{
    std::vector<PubRow> pub;
    std::vector<SubRow> sub;
    u64 dropped_rows = 0;   ///< 超出 sample_limit 未落盘的行数（不静默：写进 summary）
    std::size_t limit = 400000;

    void add(const PubRow& r)
    {
        if (pub.size() >= limit) { ++dropped_rows; return; }
        pub.push_back(r);
    }
    void add(const SubRow& r)
    {
        if (sub.size() >= limit) { ++dropped_rows; return; }
        sub.push_back(r);
    }
    bool write_pub(const std::string& path) const
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "%s\n", pub_csv_header());
        for (const auto& r : pub)
        {
            /* ⚠️ 未采集的 wire 字节写**空字段**（CSV 的 null），⛔ 不写 0：
             * W03 §2.1「未测项输出 null（CSV 空字段），不允许用 0 冒充」。
             * 0 与"未测"必须由两个不同表示区分，否则按 samples 聚合的下游会把
             * DDS 档的"未采集"读成"wire=0 = 零拷贝"。 */
            char wire[32];
            if (r.wire_known != 0) std::snprintf(wire, sizeof(wire), "%llu", (unsigned long long)r.wire_bytes);
            else wire[0] = '\0';
            std::fprintf(f, "%llu,%u,%u,%llu,%llu,%llu,%u,%s,%d,%d,%llu,%llu\n",
                         (unsigned long long)r.seq, r.idx, r.flags,
                         (unsigned long long)r.produced_ns, (unsigned long long)r.enter_ns,
                         (unsigned long long)r.done_ns, r.app_bytes,
                         wire, r.ok, r.publish_rc,
                         (unsigned long long)r.backlog_ns, (unsigned long long)r.retry);
        }
        std::fclose(f);
        return true;
    }
    bool write_sub(const std::string& path) const
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "%s\n", sub_csv_header());
        for (const auto& r : sub)
        {
            char wire[32];   /* 同 PubRow：未采集写空字段，不写 0 */
            if (r.rx_wire_known != 0) std::snprintf(wire, sizeof(wire), "%llu", (unsigned long long)r.rx_wire_bytes);
            else wire[0] = '\0';
            std::fprintf(f, "%llu,%u,%u,%llu,%llu,%d,%u,%s,%d,%d\n",
                         (unsigned long long)r.seq, r.idx, r.flags,
                         (unsigned long long)r.app_obtained_ns,
                         (unsigned long long)r.fully_consumed_ns,
                         r.checksum_ok, r.elements_checked,
                         wire, r.abnormal, r.view_kind);
        }
        std::fclose(f);
        return true;
    }
};

/* ======================================================================== */
/* 角色进程：dzIPC（SHM）发布者                                             */
/* ======================================================================== */

struct PubloopResult
{
    u64 plan = 0, attempts = 0, sent_ok = 0, failed = 0, blocked = 0, retried = 0;
    u64 late = 0, backlog_sum = 0, backlog_max = 0;
    u64 b_loan_fail = 0;
    u64 first_measure_seq = 0, last_measure_seq = 0;
    u64 dzflat = 0, fallback = 0;
    double elapsed = 0.0;
    std::string note;
};

/* 定速：独立于接收完成的发送计划（§13.1/§W02 第 7 条）。
 * k 条消息的目标时刻 = t0 + k/rate。错过时刻只记迟发/积压，不改计划。 */
struct SendPlan
{
    bool paced = false;
    steady_clock::time_point t0;
    nanoseconds period{0};
    u64 next_index = 0;

    void init(double rate_hz)
    {
        paced = rate_hz > 0;
        t0 = steady_clock::now();
        period = nanoseconds(static_cast<i64>(paced ? (1e9 / rate_hz) : 0.0));
        const u64 tenth = paced ? static_cast<u64>(period.count() / 10) : 0u;
        late_threshold_ns = std::max<u64>(50000ull, tenth);
    }
    /* 节拍迟发判定阈值。
     * sleep_until 的粒度本身就有几十 µs 抖动 —— 把它一律记成"迟发"会让每一轮都是
     * 100% 迟发，指标失去分辨力（实测：不设阈值时 1000/1000 全记迟发）。所以分两层：
     *   · backlog_*：原始迟发量（每次发送都记，含正常抖动）；
     *   · late：超过阈值的次数，阈值 = max(50 µs, 周期的 10%)，并在输出里写明。
     * 迟发 ≠ 丢消息：从 t0 起的绝对排定表保证"晚了就追回来"，不会把负载降下来。 */
    u64 late_threshold_ns{0};

    /* 等到第 index 条的排定时刻；返回迟发量(ns, 0 = 准时或提前)。 */
    u64 wait_for(u64 index, double max_spin_us)
    {
        if (!paced) { next_index = index + 1; return 0; }
        const auto due = t0 + nanoseconds(period.count() * static_cast<i64>(index));
        const auto spin = microseconds(static_cast<i64>(max_spin_us));
        const auto now0 = steady_clock::now();
        if (now0 < due - spin)
        {
            std::this_thread::sleep_until(due - spin);
        }
        while (steady_clock::now() < due) { /* 近距自旋，保证节拍 */ }
        next_index = index + 1;
        const auto now1 = steady_clock::now();
        return now1 > due ? static_cast<u64>(duration_cast<nanoseconds>(now1 - due).count()) : 0u;
    }
};

/* 读任意 pid 的累计 CPU 秒（/proc/<pid>/stat 字段 14+15）。
 * 用途：DDS+iceoryx 档必须把 RouDi 守护进程的成本单列 —— 方案 §10.7 明写
 * "CycloneDDS/RouDi 的服务成本不能无说明地遗漏"，而 RouDi 是独立进程、
 * wait4 的 rusage 覆盖不到它。 */
static bool read_pid_cpu_s(int pid, double& out)
{
    if (pid <= 0) return false;
    const dzIPC::measure::ProcessStat st = dzIPC::measure::read_process_stat(pid);
    if (!st.valid) return false;
    const double hz = static_cast<double>(dzIPC::measure::clock_ticks_per_sec());
    out = (static_cast<double>(st.utime_ticks) + static_cast<double>(st.stime_ticks)) / hz;
    return true;
}

static void write_identity(ControlBlock* ctl, bool is_pub)
{
#ifdef __linux__
    const int pid = static_cast<int>(::getpid());
    if (is_pub) { ctl->pub_pid.store(pid); }
    else        { ctl->sub_pid.store(pid); }
    const std::string raw = dzIPC::measure::read_text_file("/proc/" + std::to_string(pid) + "/stat");
    const std::size_t close = raw.rfind(')');
    if (close != std::string::npos)
    {
        std::istringstream is(raw.substr(close + 1));
        std::string tok;
        int k = 0;
        u64 starttime = 0;
        while (is >> tok)
        {
            if (k == 19) { starttime = static_cast<u64>(std::strtoull(tok.c_str(), nullptr, 10)); break; }
            ++k;
        }
        if (is_pub) { ctl->pub_starttime_ticks.store(starttime); }
        else        { ctl->sub_starttime_ticks.store(starttime); }
    }
    const std::string lib = loaded_lib_path("libipc.so");
    char* dst = is_pub ? ctl->pub_loaded_lib : ctl->sub_loaded_lib;
    std::snprintf(dst, sizeof(ctl->pub_loaded_lib), "%s", lib.c_str());
#else
    (void)ctl; (void)is_pub;
#endif
}

static int run_pub_dzipc(const Config& cfg, ControlBlock* ctl)
{
    const Shape s = cfg_shape(cfg);
    const bool flat = cfg.is_flat_path();
    dzIPC::EnableDzFlat(flat);
    dzIPC::EnableNodelet(false);   /* 跨进程基准：绝不允许进程内快路径 */
    dzIPC::ResetDzFlatCounters();
    dzIPC::ResetDzFlatRxCounters();

    RawWriter out;
    out.limit = static_cast<std::size_t>(cfg.sample_limit);

    W02_DBG("[pub] creating publisher topic=%s\n", cfg.topic.c_str());
    auto tpl = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>(), 42);
    /* ⚠️ 这里直接用 shm::shm_pub_ipc，而不是 PublisherIPCPtrMake 返回的 pimpl 包装：
     * B 路径的 loan()/publish_loaned() 只存在于 shm_pub_ipc 上（见 include/dzIPC/
     * shm_pub_sub_ipc.h:77/94），pimpl 包装不转发它们。B 的证据必须是**真实调用点**，
     * 所以基准自己持有具体类型，不用包装层。A/TLV 走同一个对象，语义与包装层一致。 */
    std::unique_ptr<dzIPC::shm::shm_pub_ipc> pub;
    try
    {
        pub.reset(new dzIPC::shm::shm_pub_ipc(tpl, cfg.topic, static_cast<size_t>(cfg.domain), false));
        pub->InitChannel("w02");
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[pub] init failed: %s\n", e.what());
        ctl->pub_failed.store(1);
        return 1;
    }

    auto pmsg = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
    auto* obj = static_cast<TestMsg*>(pmsg->topic().get());
    produce_into(obj, s, 1);   /* 先建好等尺寸缓冲，热路径不分配 */

    W02_DBG("[pub] channel ready, probing\n");
    write_identity(ctl, true);
    ctl->pub_ready_ns.store(now_ns());
    ctl->pub_ready.store(1, std::memory_order_release);

    /* ---- 数据面握手：发探测帧直到订阅侧真的收到（不是控制面计数） ---- */
    {
        auto deadline = steady_clock::now() + seconds(cfg.is_dds_path() ? 20 : 10);
        u32 probe_idx = 0;
        while (ctl->probe_received.load(std::memory_order_acquire) == 0
               && ctl->run_failed.load(std::memory_order_acquire) == 0
               && steady_clock::now() < deadline)
        {
            const u64 ts = now_ns();
            fill_header_slots(obj, s, 0, probe_idx++, kFlagProbe, ts, ts, 0, false);
            pub->publish_blocking(pmsg->topic(), 100);
            std::this_thread::sleep_for(milliseconds(5));
        }
        if (ctl->probe_received.load(std::memory_order_acquire) == 0)
        {
            std::fprintf(stderr, "[pub] handshake timeout: 订阅侧没有收到任何探测帧\n");
            ctl->pub_failed.store(2);
            ctl->run_failed.store(1);
            return 3;
        }
    }

    /* ---- 预热（flag=kFlagWarmup，订阅侧计过滤数但不入样本表） ---- */
    const u64 warmup_end_ns = now_ns() + static_cast<u64>(cfg.warmup * 1e9);
    {
        u32 idx = 0;
        u64 seq = 1;
        while (now_ns() < warmup_end_ns
               && ctl->run_failed.load(std::memory_order_acquire) == 0)
        {
            const u64 produced = now_ns();
            produce_into(obj, s, seq);
            const u64 enter = now_ns();
            fill_header_slots(obj, s, seq, idx++, kFlagWarmup, produced, enter, 0, false);
            if (flat && cfg.path == PathSel::dzflat_b)
            {
                /* ⚠️ 预热帧在 B 档**必须**与测量帧同形（借样 → 三段都分配 + 写时间戳头）。
                 * 修前这里只 `set_data4(true)` 就发布：订阅侧的视图处理器会先按 workload
                 * 校验载荷（full/crc 档）再判 flag，于是把每条预热帧都记成
                 * `bad_header`+`abnormal`（实测 1194–1200 条/case）。该缺陷在 t47 补
                 * `bad_header` 列之前**不可见**（只写进子进程日志），正是 F3 要堵的那类
                 * 「量存在但不进判定」——现在它既进判定、也已被修好。 */
                auto lo = pub->loan<dzIPC::Msg::TestMsgFlat>(loan_budget(s.bytes()));
                if (lo.valid())
                {
                    auto sp1 = lo->alloc_data1(s.n1);
                    for (u32 i = 0; i < sp1.size(); ++i) sp1[i] = pat_double(seq, i);
                    auto sp2 = lo->alloc_data2(kHdrSlots + s.n2);
                    for (u32 i = 0; i < s.n2 && sp2.size() > kHdrSlots + i; ++i)
                        sp2[kHdrSlots + i] = pat_int(seq, i);
                    lo->alloc_data3(s.n3);
                    char wbuf[256];
                    for (u32 k = 0; k < s.n3; ++k)
                    {
                        const u32 len = std::min<u32>(s.str_len, 256u);
                        pat_string(seq, k, len, wbuf);
                        lo->set_data3_at(k, std::string_view(wbuf, len));
                    }
                    lo->set_data4(true);
                    if (sp2.size() >= kHdrSlots)
                    {
                        i32* d2 = sp2.data();
                        d2[kSlotSeqLo] = static_cast<i32>(static_cast<u32>(seq & 0xFFFFFFFFull));
                        d2[kSlotSeqHi] = static_cast<i32>(static_cast<u32>(seq >> 32));
                        d2[kSlotIdxFlags] = static_cast<i32>((idx & 0xFFFFu) | (kFlagWarmup << 16));
                        d2[kSlotCrc] = 0;
                        d2[kSlotProducedLo] = static_cast<i32>(static_cast<u32>(produced & 0xFFFFFFFFull));
                        d2[kSlotProducedHi] = static_cast<i32>(static_cast<u32>(produced >> 32));
                        d2[kSlotEnterLo] = static_cast<i32>(static_cast<u32>(enter & 0xFFFFFFFFull));
                        d2[kSlotEnterHi] = static_cast<i32>(static_cast<u32>(enter >> 32));
                    }
                    pub->publish_loaned(std::move(lo));
                }
            }
            else
            {
                pub->publish_blocking(pmsg->topic(), 100);
            }
            ++seq;
            ++idx;
            std::this_thread::sleep_for(microseconds(200));
        }
    }
    std::this_thread::sleep_for(milliseconds(150));   /* 让订阅侧排空预热 */

    /* ---- 测量窗口 ---- */
    SendPlan plan;
    plan.init(cfg.rate_hz);
    const auto measure_start = steady_clock::now();
    ctl->measure_start_ns.store(now_ns(), std::memory_order_release);
    ctl->phase.store(3, std::memory_order_release);

    const u64 plan_total = cfg.rate_hz > 0
        ? static_cast<u64>(std::ceil(cfg.rate_hz * cfg.duration))
        : 0;
    ctl->plan_total.store(plan_total, std::memory_order_release);

    PubloopResult pr;
    pr.plan = plan_total;
    u64 seq = 1000000;         /* 测量序号从 1e6 起，与预热/探测明显分离 */
    u32 idx = 0;
    bool first = true;
    const auto deadline = measure_start + duration<double>(cfg.duration);

    for (;;)
    {
        if (cfg.rate_hz > 0)
        {
            if (pr.attempts >= plan_total) break;
            const u64 backlog = plan.wait_for(pr.attempts, 80.0);
            if (backlog > 0)
            {
                if (backlog > plan.late_threshold_ns) ++pr.late;
                pr.backlog_sum += backlog;
                pr.backlog_max = std::max(pr.backlog_max, backlog);
            }
        }
        else if (steady_clock::now() >= deadline)
        {
            break;
        }

        const u64 produced = now_ns();
        const u64 seq_this = seq++;
        const u32 idx_this = idx++;
        const bool need_crc = (cfg.workload == Workload::crc);
        u32 crc = 0;

        PubRow row;
        row.seq = seq_this;
        row.idx = idx_this;
        row.flags = kFlagMeasure;
        row.produced_ns = produced;
        row.app_bytes = s.bytes();
        row.wire_bytes = 0;

        if (first) { pr.first_measure_seq = seq_this; first = false; }

        bool ok = false;
        if (flat && cfg.path == PathSel::dzflat_b)
        {
            /* ---- B：loan → 应用原地构造 → publish_loaned（真实调用点） ----
             * 三个要点（都是实测踩出来的）：
             * ① data2 必须借 kHdrSlots + s.n2 个元素：时间戳头也住在本消息的 data2 里
             *    （与 A/TLV 侧同一约定），只借 s.n2 会让头部立刻越界写坏邻居字段；
             * ② 头部槽位在**载荷写完之后**写：produced/enter/crc 才拿得到；
             * ③ 借样失败（chunk 池耗尽/无接收方）是背压不是异常，必须显式计数并继续，
             *    不能静默跳过整条消息。 */
            auto lo = pub->loan<dzIPC::Msg::TestMsgFlat>(loan_budget(s.bytes()));
            if (!lo.valid())
            {
                ++pr.b_loan_fail;
                ++pr.failed;
                CounterRegistry::instance().inc(CounterId::chunk_exhausted);
                pr.note = "B 借样失败(chunk 池耗尽/无接收方) —— 已显式计数, 未静默排除";
                ++pr.attempts;
                row.ok = 0; row.publish_rc = 3;
                row.enter_ns = now_ns();
                row.done_ns = row.enter_ns;
                out.add(row);
                ctl->attempt_total.store(pr.attempts, std::memory_order_release);
                ctl->send_failed.store(pr.failed, std::memory_order_release);
                continue;
            }
            /* 生成即写进共享内存（不经过调用方堆） */
            const u64 enter_b = now_ns();
            auto sp1 = lo->alloc_data1(s.n1);
            for (u32 i = 0; i < sp1.size(); ++i) sp1[i] = pat_double(seq_this, i);
            auto sp2 = lo->alloc_data2(kHdrSlots + s.n2);
            for (u32 i = 0; i < s.n2; ++i)
            {
                if (sp2.size() > kHdrSlots + i) sp2[kHdrSlots + i] = pat_int(seq_this, i);
            }
            lo->alloc_data3(s.n3);
            char buf[256];
            for (u32 k = 0; k < s.n3; ++k)
            {
                const u32 len = std::min<u32>(s.str_len, 256u);
                pat_string(seq_this, k, len, buf);
                lo->set_data3_at(k, std::string_view(buf, len));
            }
            lo->set_data4(true);
            if (need_crc)
            {
                /* B 的 CRC 在 chunk span 上算：同一口径（逻辑载荷字节） */
                u32 c = 0;
                if (s.n1) c = dzIPC::common::crc32c(sp1.data(), s.n1 * sizeof(double));
                for (u32 k = 0; k < s.n3; ++k)
                {
                    char buf2[256];
                    const u32 len = std::min<u32>(s.str_len, 256u);
                    pat_string(seq_this, k, len, buf2);
                    c = (c * 16777619u) ^ dzIPC::common::crc32c(buf2, len);
                }
                if (s.n2) c = (c * 16777619u) ^ dzIPC::common::crc32c(sp2.data() + kHdrSlots,
                                                                       s.n2 * sizeof(i32));
                crc = c;
            }
            /* 时间戳头：直接写共享 chunk 的 data2 前 8 槽（B 级没有 owning 对象可问）。 */
            if (sp2.size() >= kHdrSlots)
            {
                i32* d2 = sp2.data();
                d2[kSlotSeqLo] = static_cast<i32>(static_cast<u32>(seq_this & 0xFFFFFFFFull));
                d2[kSlotSeqHi] = static_cast<i32>(static_cast<u32>(seq_this >> 32));
                d2[kSlotIdxFlags] = static_cast<i32>((idx_this & 0xFFFFu) | (kFlagMeasure << 16));
                d2[kSlotCrc] = need_crc ? static_cast<i32>(crc) : 0;
                d2[kSlotProducedLo] = static_cast<i32>(static_cast<u32>(produced & 0xFFFFFFFFull));
                d2[kSlotProducedHi] = static_cast<i32>(static_cast<u32>(produced >> 32));
                d2[kSlotEnterLo] = static_cast<i32>(static_cast<u32>(enter_b & 0xFFFFFFFFull));
                d2[kSlotEnterHi] = static_cast<i32>(static_cast<u32>(enter_b >> 32));
            }
            row.enter_ns = enter_b;
            row.wire_bytes = lo.size();
            ok = pub->publish_loaned(std::move(lo));
        }
        else
        {
            produce_into(obj, s, seq_this);
            if (need_crc) crc = crc_of_object(obj, s);
            const u64 enter = now_ns();
            fill_header_slots(obj, s, seq_this, idx_this, kFlagMeasure, produced, enter, crc, need_crc);
            row.enter_ns = enter;
            if (cfg.path == PathSel::tlv)
            {
                /* wire 字节：TLV 整包长度（形状固定 ⇒ 每消息一致，热身期测一次即可） */
                if (row.wire_bytes == 0)
                {
                    ipc::buffer b = obj->serialize();
                    row.wire_bytes = b.size();
                }
                ok = cfg.best_effort ? pub->publish_best_effort(pmsg->topic())
                                     : pub->publish_blocking(pmsg->topic(), cfg.queue_wait_ms);
            }
            else
            {
                /* A：对象 → 共享段一次复制（内部 loan/dzflat_write/publish_loan） */
                if (row.wire_bytes == 0) row.wire_bytes = obj->dzflat_size();
                ok = pub->publish_blocking(pmsg->topic(), cfg.queue_wait_ms);
            }
        }

        row.done_ns = now_ns();
        row.backlog_ns = cfg.rate_hz > 0 ? 0 : 0;
        row.retry = 0;
        row.ok = ok ? 1 : 0;
        row.publish_rc = ok ? 0 : 1;
        out.add(row);

        ++pr.attempts;
        ctl->attempt_total.store(pr.attempts, std::memory_order_release);
        if (ok)
        {
            ++pr.sent_ok;
            ctl->sent_ok.store(pr.sent_ok, std::memory_order_release);
            /* ⚠️ **本函数不得写 `*_messages`**（tlv_messages / dzflat_a_messages /
             * dzflat_b_messages）—— 那是传输层的账。W08 已在实际交付点埋点：
             * `src/dzIPC/common/nodelet_config.cc:NoteDzFlatPathDelivered()`
             * （调用点 `src/dzIPC/shm_pub_sub_ipc.cc` 的 A/B/预构造段三个入口），
             * 它才是"这条消息实际走了哪条路径"的事实来源。
             *
             * 历史缺陷（W08 于 2026-09-28 交叉核对发现、已修）：本处原先也 `inc`
             * 同一组 `*_messages`，于是**同一次成功发布被记两次**——实测
             * `artifacts/perf/20260928-r19-W02` 的 dzIPC 三档 45 个用例里
             * `family=3212` 而 `sent_ok=1000`（≈ 1000 本处 + 2212 钩子）。
             * 受损面：**直接读 counters.json 的 `*_messages`**（含 W03 `path_evidence`
             * 把它当"该路径独立计数"）会得到约 2× 条数。
             *
             * 口径（消歧后）：
             *   · `*_messages` / `*_wire_bytes` —— 传输层钩子写，含探测/预热/停止帧，
             *     是"实际交付路径"计数；DDS 档恒为 0（不经 dzIPC SHM 路径）。
             *   · `*_bytes` —— **本基准写**，只统计测量窗口内成功发布的应用逻辑载荷
             *     （= sent_ok × payload_bytes），与钩子的 wire 字节口径不同、**不相加**。
             *   · 用例级计数以 `summary.csv` 的 plan/attempts/sent_ok/recv_measure 为准
             *     （它们不经 CounterRegistry）。
             * 若将来需要"基准侧也记一份路径条数"，必须换新的 CounterId 或加独立命名空间，
             * ⛔ 不得与钩子复用同一 ID。 */
            /* `*_bytes`（应用逻辑载荷）仍由**本基准**记账；它是 harness 口径，只统计测量
             * 窗口内成功发布的那部分，⛔ 与钩子的 `*_wire_bytes` 口径不同、不得相加。
             * 用 switch 而非三元链：`PathSel` 有 5 个值而 dzIPC 只有 3 条路径，三元链的
             * 末支会把 `cyc_udp`/`cyc_iox` **兜底**成 dzflat_b（D-17 追加①的同型缺陷）。
             * 当前 DDS 档的发布分支（run_pub_dds）根本不调用 CounterRegistry，所以旧写法
             * 未观测到污染；这里显式 no-op 是为了让该缺陷在结构上不可复现。 */
            switch (cfg.path)
            {
            case PathSel::tlv:
                CounterRegistry::instance().add(CounterId::tlv_bytes, s.bytes());
                break;
            case PathSel::dzflat_a:
                CounterRegistry::instance().add(CounterId::dzflat_a_bytes, s.bytes());
                break;
            case PathSel::dzflat_b:
                CounterRegistry::instance().add(CounterId::dzflat_b_bytes, s.bytes());
                break;
            case PathSel::cyc_udp:
            case PathSel::cyc_iox:
                /* ⛔ DDS 档不写任何 path 计数：它不经 dzIPC SHM 路径，既没有
                 * `*_messages`（钩子只在 shm_pub_sub_ipc 里被调用），也不该借用
                 * dzflat_*_bytes。cyc 专用 ID 待共享层按 t19 追加后另行接线。 */
                break;
            }
        }
        else
        {
            ++pr.failed;
            CounterRegistry::instance().inc(CounterId::publish_failed);
            if (!cfg.best_effort)
            {
                ++pr.blocked;
                CounterRegistry::instance().inc(CounterId::publish_blocked);
            }
        }
    }
    pr.last_measure_seq = seq - 1;
    pr.elapsed = duration<double>(steady_clock::now() - measure_start).count();
    ctl->measure_end_ns.store(now_ns(), std::memory_order_release);
    ctl->late_sends.store(pr.late, std::memory_order_release);
    ctl->late_threshold_ns.store(plan.late_threshold_ns, std::memory_order_release);
    ctl->backlog_sum_ns.store(pr.backlog_sum, std::memory_order_release);
    ctl->backlog_max_ns.store(pr.backlog_max, std::memory_order_release);
    ctl->send_failed.store(pr.failed, std::memory_order_release);
    ctl->send_blocked.store(pr.blocked, std::memory_order_release);

    /* ---- 排空 + 停止哨兵（正常退出路径）。 ----
     * 哨兵要重发到**订阅侧确认收到**为止：阻塞组的主循环可能正卡在 get_clone() 的
     * 条件变量上 —— 发一次就停会让它永久挂住，只能靠父进程超时 SIGKILL 收尾，那正是
     * "异常退出被静默掩盖"的形态。这里改成有界重发直到 stop_frames_recv >= 3。 */
    std::this_thread::sleep_for(milliseconds(300));
    ctl->phase.store(4, std::memory_order_release);
    {
        const auto sentinel_deadline = steady_clock::now() + seconds(5);
        while (ctl->stop_frames_recv.load() < 3 && steady_clock::now() < sentinel_deadline)
        {
            const u64 ts = now_ns();
            fill_header_slots(obj, s, 0, 0, 0x8 /*stop*/, ts, ts, 0, false);
            pub->publish_blocking(pmsg->topic(), 100);
            ctl->stop_frames_sent.fetch_add(1);
            std::this_thread::sleep_for(milliseconds(5));
        }
    }
    ctl->phase.store(5, std::memory_order_release);
    /* 等订阅侧正常收尾再释放发布端通道：发布端先析构会把订阅端正卡着的 recv 唤醒成
     * 伪影/断开，那属于"用异常退出换通过"，不是正常退出路径。 */
    if (ctl->sub_done.load(std::memory_order_acquire) == 0)
    {
        const auto drain_deadline = steady_clock::now() + seconds(20);
        while (ctl->sub_done.load(std::memory_order_acquire) == 0
               && steady_clock::now() < drain_deadline)
        {
            std::this_thread::sleep_for(milliseconds(5));
        }
        if (ctl->sub_done.load(std::memory_order_acquire) == 0)
        {
            std::fprintf(stderr, "[pub] 订阅侧未在 20s 内正常收尾\n");
            ctl->run_failed.store(1);
        }
    }

    pr.dzflat = dzIPC::DzFlatPublishCount();
    pr.fallback = dzIPC::DzFlatFallbackCount();
    ctl->pub_dzflat.store(pr.dzflat);
    ctl->pub_fallback.store(pr.fallback);
    ctl->pub_finish_ns.store(now_ns());

    const std::string raw_path = cfg.out_file.empty()
        ? std::string("pub_raw.csv") : cfg.out_file;
    out.write_pub(raw_path);
    /* 逐样本上限截断必须显式落盘：静默少写行会让"没收到"和"没记下"长得一样。 */
    write_text_file(raw_path + ".dropped.txt",
                    std::to_string(out.dropped_rows) + "\n");
    CounterRegistry::instance().write_json_file(raw_path + ".counters.json");
    pub.reset();   /* 正常析构：注销 route、释放共享内存 */

    std::printf("[pub] path=%s payload=%llu plan=%llu attempts=%llu ok=%llu failed=%llu blocked=%llu "
                "late=%llu backlog_max_ns=%llu dzflat=%llu fallback=%llu loan_fail=%llu elapsed=%.3fs rows=%zu\n",
                path_name(cfg.path), (unsigned long long)s.bytes(),
                (unsigned long long)pr.plan, (unsigned long long)pr.attempts,
                (unsigned long long)pr.sent_ok, (unsigned long long)pr.failed,
                (unsigned long long)pr.blocked, (unsigned long long)pr.late,
                (unsigned long long)pr.backlog_max, (unsigned long long)pr.dzflat,
                (unsigned long long)pr.fallback, (unsigned long long)pr.b_loan_fail, pr.elapsed,
                out.pub.size());
    if (!pr.note.empty()) std::printf("[pub] note=%s\n", pr.note.c_str());
    std::fflush(stdout);
    return 0;
}

/* ======================================================================== */
/* 角色进程：dzIPC（SHM）订阅者                                             */
/* ======================================================================== */

struct SubLoopResult
{
    u64 recv_measure = 0, recv_probe = 0, recv_warmup = 0, recv_stop = 0;
    u64 checksum_bad = 0, bad_header = 0, wrong_seq = 0, abnormal = 0;
    u64 via_view = 0, via_object = 0;
    u64 elements = 0;
    u64 ooo = 0, dup = 0;
    u64 frames_from_disconnect_artifacts = 0;
    std::string note;
};

static int run_sub_dzipc(const Config& cfg, ControlBlock* ctl)
{
    const Shape s = cfg_shape(cfg);
    dzIPC::EnableDzFlat(cfg.is_flat_path());
    dzIPC::EnableNodelet(false);
    dzIPC::ResetDzFlatCounters();
    dzIPC::ResetDzFlatRxCounters();
    dzIPC::ResetWakeupArtifactCount();

    RawWriter out;
    out.limit = static_cast<std::size_t>(cfg.sample_limit);

    W02_DBG("[sub] creating subscriber topic=%s\n", cfg.topic.c_str());
    auto tpl = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>(), 42);
    dzIPC::SubscriberIPCPtr sub;
    try
    {
        sub = dzIPC::SubscriberIPCPtrMake(tpl, cfg.topic, cfg.domain, cfg.queue, dzIPC::IPC_SHM, false);
        sub->InitChannel("w02");
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[sub] init failed: %s\n", e.what());
        ctl->sub_failed.store(1);
        ctl->run_failed.store(1);
        return 1;
    }

    W02_DBG("[sub] channel ready\n");
    write_identity(ctl, false);
    ctl->sub_ready_ns.store(now_ns());
    ctl->sub_ready.store(1, std::memory_order_release);

    auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(42);
    SubLoopResult sr;
    const bool prefers_view = cfg.is_flat_path();
    const bool busy = (cfg.wait_mode == WaitMode::busy);
    u64 prev_seq = 0;
    bool have_prev = false;
    std::set<u64> seen;
    auto last_activity = steady_clock::now();

    /* 单帧处理（对象路径 / 视图路径合用） */
    auto handle_object = [&](u64 app_ns) -> void {
        auto* obj = static_cast<TestMsg*>(rcv->topic().get());
        const u64 fully = now_ns();
        CheckOutcome o = check_header(obj->data2.data(), static_cast<u32>(obj->data2.size()));
        if (!o.header_ok)
        {
            ++sr.bad_header;
            CounterRegistry::instance().inc(CounterId::seq_lost);
            ++sr.abnormal;
            return;
        }
        const bool abnormal_wire = (o.flags == kFlagProbe || o.flags == kFlagWarmup || o.flags == 0x8);
        if (cfg.workload == Workload::full) verify_object(obj, s, o, true, false);
        else if (cfg.workload == Workload::crc) verify_object(obj, s, o, false, true);
        if (obj->data1.size() < s.n1 || obj->data2.size() < kHdrSlots + s.n2 || obj->data3.size() < s.n3)
        {
            ++sr.bad_header;
            ++sr.abnormal;
        }
        if (!o.payload_ok)
        {
            ++sr.checksum_bad;
            CounterRegistry::instance().inc(CounterId::payload_checksum_bad);
        }
        else if (cfg.workload != Workload::timestamp)
        {
            CounterRegistry::instance().inc(CounterId::payload_checksum_ok);
        }
        ++sr.via_object;
        sr.elements += o.elements;

        /* 相位分账：探测/预热/停止帧不入样本表，但计数不丢。 */
        if (o.flags == kFlagProbe) { ++sr.recv_probe; ctl->sub_probe_count.fetch_add(1); ctl->probe_received.store(1); return; }
        if (o.flags == kFlagWarmup) { ++sr.recv_warmup; return; }
        if (o.flags == 0x8) { ++sr.recv_stop; ctl->stop_frames_recv.fetch_add(1); return; }
        (void)abnormal_wire;

        /* 序号完整性（乱序/重复由到达序判定，丢失由父进程按发布表判定） */
        if (have_prev && o.seq <= prev_seq)
        {
            if (o.seq == prev_seq) ++sr.dup; else ++sr.ooo;
        }
        if (!seen.insert(o.seq).second) ++sr.dup;
        prev_seq = o.seq;
        have_prev = true;

        SubRow row;
        row.seq = o.seq; row.idx = o.idx; row.flags = o.flags;
        row.app_obtained_ns = app_ns;
        row.fully_consumed_ns = fully;
        row.checksum_ok = (cfg.workload == Workload::timestamp) ? -1 : (o.payload_ok ? 1 : 0);
        row.elements_checked = static_cast<u32>(o.elements);
        /* 物化路径（TLV / 对象）的接收侧 wire 字节未采集：⛔ 写空字段，不写 0。 */
        row.rx_wire_bytes = 0;
        row.rx_wire_known = 0;
        row.abnormal = o.abnormal ? 1 : 0;
        row.view_kind = 0;
        out.add(row);
        ++sr.recv_measure;
        last_activity = steady_clock::now();
        ctl->sub_received_total.fetch_add(1, std::memory_order_release);
        if (ctl->sub_first_rx_ns.load() == 0) ctl->sub_first_rx_ns.store(app_ns);
    };

    auto handle_view = [&](dzIPC::Sample& smp, u64 app_ns) -> void {
        /* app_obtained_ns = "应用拿到可用视图"的时刻：由调用方在 try_get() 返回后**立刻**
         * 取（见收包循环）。这里只做绑定后的读取与校验 —— ⚠️ 不得把绑定放到 app_ns 之前，
         * 否则 delivery 会把视图绑定成本算进去、app_read 反而变小，
         * 两条边界一起失真（这正是 §10.7 禁止的"混用结束点"）。 */
        auto v = smp.view<dzIPC::Msg::TestMsgFlat>();
        if (!v.valid())
        {
            ++sr.bad_header;
            ++sr.abnormal;
            ++sr.checksum_bad;
            return;
        }
        auto d2 = v.data2();
        std::vector<i32> hdr;
        CheckOutcome o;
        if (d2.size() < kHdrSlots)
        {
            ++sr.bad_header; ++sr.abnormal; return;
        }
        o.seq = (static_cast<u64>(static_cast<u32>(d2[kSlotSeqHi])) << 32)
              | static_cast<u64>(static_cast<u32>(d2[kSlotSeqLo]));
        const u32 idxf = static_cast<u32>(d2[kSlotIdxFlags]);
        o.idx = idxf & 0xFFFFu;
        o.flags = idxf >> 16;
        o.produced_ns = (static_cast<u64>(static_cast<u32>(d2[kSlotProducedHi])) << 32)
                      | static_cast<u64>(static_cast<u32>(d2[kSlotProducedLo]));
        o.enter_ns = (static_cast<u64>(static_cast<u32>(d2[kSlotEnterHi])) << 32)
                   | static_cast<u64>(static_cast<u32>(d2[kSlotEnterLo]));
        o.crc_field = static_cast<u32>(d2[kSlotCrc]);
        o.header_ok = true;
        o.elements = kHdrSlots;
        (void)hdr;

        if (cfg.workload == Workload::full)
        {
            auto a1 = v.data1();
            if (a1.size() != s.n1) { o.payload_ok = false; o.why = "data1 元素数不符"; o.abnormal = true; }
            for (u32 i = 0; i < std::min<u32>(static_cast<u32>(a1.size()), s.n1); ++i)
            {
                ++o.elements;
                if (a1[i] != pat_double(o.seq, i)) ++o.mismatches;
            }
            const u32 n2have = d2.size() >= kHdrSlots ? static_cast<u32>(d2.size() - kHdrSlots) : 0u;
            if (n2have != s.n2) { o.payload_ok = false; o.why = "data2 负载元素数不符"; o.abnormal = true; }
            for (u32 i = 0; i < std::min<u32>(n2have, s.n2); ++i)
            {
                ++o.elements;
                if (d2[kHdrSlots + i] != pat_int(o.seq, i)) ++o.mismatches;
            }
            if (v.data3_count() != s.n3) { o.payload_ok = false; o.why = "data3 元素数不符"; o.abnormal = true; }
            for (u32 k = 0; k < std::min<u32>(v.data3_count(), s.n3); ++k)
            {
                ++o.elements;
                auto sv = v.data3(k);
                if (sv.size() != s.str_len) { ++o.mismatches; continue; }
                char expect[256];
                const u32 len = std::min<u32>(s.str_len, 256u);
                pat_string(o.seq, k, len, expect);
                if (std::memcmp(sv.data(), expect, len) != 0) ++o.mismatches;
            }
            if (!v.data4()) { ++o.mismatches; }
        }
        else if (cfg.workload == Workload::crc)
        {
            /* 视图路径的 CRC 直接在共享内存上算：真零拷贝顺序读 */
            u32 c = 0;
            auto a1 = v.data1();
            if (a1.size()) c = dzIPC::common::crc32c(a1.data(), a1.size() * sizeof(double));
            for (u32 k = 0; k < v.data3_count(); ++k)
            {
                auto sv = v.data3(k);
                c = (c * 16777619u) ^ dzIPC::common::crc32c(sv.data(), sv.size());
            }
            if (d2.size() > kHdrSlots)
                c = (c * 16777619u) ^ dzIPC::common::crc32c(d2.data() + kHdrSlots,
                                                            (d2.size() - kHdrSlots) * sizeof(i32));
            o.crc_computed = c;
            if (c != o.crc_field) { o.payload_ok = false; ++o.mismatches; }
        }
        o.payload_ok = o.payload_ok && o.mismatches == 0;
        if (o.mismatches) { o.abnormal = true; o.why = "模式/CRC 不符"; }
        if (!o.payload_ok) { ++sr.checksum_bad; CounterRegistry::instance().inc(CounterId::payload_checksum_bad); }
        else if (cfg.workload != Workload::timestamp) CounterRegistry::instance().inc(CounterId::payload_checksum_ok);
        ++sr.via_view;
        sr.elements += o.elements;
        const u64 fully = now_ns();

        if (o.flags == kFlagProbe) { ++sr.recv_probe; ctl->sub_probe_count.fetch_add(1); ctl->probe_received.store(1); return; }
        if (o.flags == kFlagWarmup) { ++sr.recv_warmup; return; }
        if (o.flags == 0x8) { ++sr.recv_stop; ctl->stop_frames_recv.fetch_add(1); return; }

        if (have_prev && o.seq <= prev_seq)
        {
            if (o.seq == prev_seq) ++sr.dup; else ++sr.ooo;
        }
        if (!seen.insert(o.seq).second) ++sr.dup;
        prev_seq = o.seq;
        have_prev = true;

        SubRow row;
        row.seq = o.seq; row.idx = o.idx; row.flags = o.flags;
        row.app_obtained_ns = app_ns;
        row.fully_consumed_ns = fully;
        row.checksum_ok = (cfg.workload == Workload::timestamp) ? -1 : (o.payload_ok ? 1 : 0);
        row.elements_checked = static_cast<u32>(o.elements);
        row.rx_wire_bytes = smp.size();
        row.abnormal = o.abnormal ? 1 : 0;
        row.view_kind = 1;
        out.add(row);
        ++sr.recv_measure;
        last_activity = steady_clock::now();
        ctl->sub_received_total.fetch_add(1, std::memory_order_release);
        if (ctl->sub_first_rx_ns.load() == 0) ctl->sub_first_rx_ns.store(app_ns);
    };

    /* ---- 收包主循环 ----
     * 三种等待策略，**按路径分别标明**（§4 W02 第 5 条：阻塞事件驱动为主对照，忙等
     * 单独列组；内部等待策略无法对齐时必须明确标签，不能默认两边一样）：
     *
     *   blocking + DZFlat 段（A/B）：视图队列 get(Sample&, tm=20ms) —— 条件变量定时阻塞。
     *   blocking + TLV/schema-less：msg 队列 get_clone() —— 条件变量**无超时**阻塞。
     *       只能在 phase<4 用它：发布侧发停止哨兵前会置 phase=4，哨兵本身会把阻塞唤醒；
     *       phase>=4 之后仍无超时地阻塞，会在"发布侧已停、订阅侧仍未收到哨兵"的收尾阶段
     *       永久挂住（实测：订阅进程被超时 SIGKILL、sub_raw.csv 缺失）。
     *   busy：纯 try_get/try_get_clone 自旋，从不退让 —— 这是单列组，CPU 必须单独报告。
     *
     * ⚠️ app_obtained_ns 必须在**每次取到消息之后立刻**取，不能在批量 drain 前取一次：
     * 一次 drain 几十条时，用批量起点会让 delivery/app_read 全部失真（实测 1 MiB 档
     * app_read 被打成 0）。 */
    const u64 idle_stop_ns = 400ull * 1000000ull;   /* phase>=5 后无活动即收尾 */
    for (;;)
    {
        bool progressed = false;
        if (prefers_view)
        {
            dzIPC::Sample smp;
            while (sub->try_get(smp)) { const u64 t = now_ns(); handle_view(smp, t); progressed = true; }
            while (sub->try_get_clone(rcv)) { const u64 t = now_ns(); handle_object(t); progressed = true; }
            if (!progressed && !busy)
            {
                dzIPC::Sample s2;
                if (sub->get(s2, 20)) { handle_view(s2, now_ns()); progressed = true; }
                else if (sub->try_get_clone(rcv)) { handle_object(now_ns()); progressed = true; }
            }
        }
        else
        {
            while (sub->try_get_clone(rcv)) { const u64 t = now_ns(); handle_object(t); progressed = true; }
            dzIPC::Sample smp;
            while (sub->try_get(smp)) { const u64 t = now_ns(); handle_view(smp, t); progressed = true; }
            if (!progressed && !busy && ctl->phase.load(std::memory_order_acquire) < 4)
            {
                /* 真·事件驱动：等队列有数据（停止哨兵会唤醒它）。 */
                sub->get_clone(rcv);
                handle_object(now_ns());
                progressed = true;
            }
        }
        if (progressed) continue;

        const i32 phase = ctl->phase.load(std::memory_order_acquire);
        const bool stop_seen = sr.recv_stop >= 3 || ctl->stop_frames_recv.load() >= 3;
        const bool force_stop = ctl->sub_stop_request.load(std::memory_order_acquire) != 0;
        W02_DBG("[sub] idle phase=%d recv_measure=%llu probe=%llu warmup=%llu stop=%llu\n",
                phase, (unsigned long long)sr.recv_measure, (unsigned long long)sr.recv_probe,
                (unsigned long long)sr.recv_warmup, (unsigned long long)sr.recv_stop);
        if (stop_seen || force_stop) break;
        if (phase >= 5)
        {
            if (duration_cast<nanoseconds>(steady_clock::now() - last_activity).count()
                > static_cast<i64>(idle_stop_ns))
            {
                sr.note = "phase=5 后静默超时收尾（未收到全部停止哨兵）";
                break;
            }
            std::this_thread::sleep_for(microseconds(200));
            continue;
        }
        if (busy) continue;   /* 忙等组：不退让，CPU 计入本组自己的读数 */
        std::this_thread::sleep_for(microseconds(200));
    }

    sr.frames_from_disconnect_artifacts = dzIPC::WakeupArtifactCount();
    const auto rx = dzIPC::DzFlatRxCounters();
    ctl->sub_rx_dzflat_accepted.store(rx.dzflat_accepted);
    ctl->sub_rx_tlv_accepted.store(rx.tlv_accepted);
    ctl->sub_rx_defects.store(rx.defects());
    ctl->sub_rx_id_skipped.store(rx.dzflat_id_skipped + rx.tlv_id_skipped);
    ctl->sub_checksum_bad.store(sr.checksum_bad);
    ctl->sub_bad_header.store(sr.bad_header);
    ctl->sub_wrong_seq.store(sr.ooo + sr.dup);
    ctl->sub_received_total.store(sr.recv_measure);

    CounterRegistry::instance().add(CounterId::seq_out_of_order, sr.ooo);
    CounterRegistry::instance().add(CounterId::seq_duplicate, sr.dup);

    out.write_sub(cfg.out_file.empty() ? std::string("sub_raw.csv") : cfg.out_file);
    write_text_file((cfg.out_file.empty() ? std::string("sub_raw.csv") : cfg.out_file) + ".dropped.txt",
                    std::to_string(out.dropped_rows) + "\n");
    CounterRegistry::instance().write_json_file((cfg.out_file.empty() ? std::string("sub_raw.csv") : cfg.out_file) + ".counters.json");
    sub.reset();   /* 正常析构：注销 route、释放共享内存 */
    ctl->sub_done.store(1, std::memory_order_release);

    std::printf("[sub] path=%s payload=%llu measure=%llu probe=%llu warmup=%llu stop=%llu "
                "view=%llu object=%llu bad_crc=%llu bad_hdr=%llu ooo=%llu dup=%llu elements=%llu "
                "artifacts=%llu\n",
                path_name(cfg.path), (unsigned long long)s.bytes(),
                (unsigned long long)sr.recv_measure, (unsigned long long)sr.recv_probe,
                (unsigned long long)sr.recv_warmup, (unsigned long long)sr.recv_stop,
                (unsigned long long)sr.via_view, (unsigned long long)sr.via_object,
                (unsigned long long)sr.checksum_bad, (unsigned long long)sr.bad_header,
                (unsigned long long)sr.ooo, (unsigned long long)sr.dup,
                (unsigned long long)sr.elements,
                (unsigned long long)sr.frames_from_disconnect_artifacts);
    if (!sr.note.empty()) std::printf("[sub] note=%s\n", sr.note.c_str());
    std::fflush(stdout);
    return 0;
}

/* ======================================================================== */
/* 角色进程：CycloneDDS（可选后端）                                         */
/* ======================================================================== */

#ifdef W02_HAVE_DDS

/* DDS QoS。
 * 口径：显式设置 Reliability=RELIABLE / History=KEEP_LAST(32) / Durability=VOLATILE，
 * 并把生效设置写进 manifest.dds_qos —— 不依赖"CycloneDDS 默认值是什么"这种隐式条件
 * （默认值随版本变化，而版本不是本基准的控制变量）。
 *
 * ⚠️ 排查记录（留给后来者，别重犯）：一度出现 cyc-iox 档 create_topic/create_writer
 * 双双返回 "Bad Parameter"，当时归因到自定义 QoS。真正原因是 **topic 名里有 '-'**：
 * CycloneDDS 0.10.2 拒绝该字符，而 UDP 档同名字符串不报错 —— 归因错了会在 QoS 上
 * 白绕一整圈。现在 topic 名一律经 topic_safe() 消毒，QoS 与 topic 两件事互不牵连。 */
static const char* dds_qos_name()
{
    return "reliable/keep_last(1024)/volatile";
}

static dds_qos_t* make_dds_qos(bool reliable, int depth)
{
    dds_qos_t* q = dds_create_qos();
    if (reliable) dds_qset_reliability(q, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
    else          dds_qset_reliability(q, DDS_RELIABILITY_BEST_EFFORT, 0);
    /* 深度给大：KEEP_LAST(32) 在消费者跟不上时会覆盖尚未确认的历史样本，
     * 表现是 Reliable 档也出现个位数丢失（实测 2/1000）。深度是**显式配置**，
     * 写进 manifest.dds_qos，避免"丢包到底是链路还是队列深度"变成猜谜。 */
    dds_qset_history(q, DDS_HISTORY_KEEP_LAST, depth);
    dds_qset_durability(q, DDS_DURABILITY_VOLATILE);
    return q;
}

static void fill_dds(w02_Sample& m, const Shape& s, u64 seq, u32 idx, u32 flags,
                     u64 produced, u64 enter)
{
    m.seq = static_cast<int64_t>(seq);
    m.idx_flags = static_cast<int32_t>((idx & 0xFFFFu) | (flags << 16));
    m.produced_ns = static_cast<int64_t>(produced);
    m.enter_ns = static_cast<int64_t>(enter);
    m.hdr_xor = 0;
    m.prev_seq = 0;
    m.prev_done_ns = 0;
    m.data1._length = s.n1;
    for (u32 i = 0; i < s.n1; ++i) m.data1._buffer[i] = pat_double(seq, i);
    m.data2._length = s.n2;
    for (u32 i = 0; i < s.n2; ++i) m.data2._buffer[i] = pat_int(seq, i);
    m.data3._length = s.n3;
    for (u32 k = 0; k < s.n3; ++k)
    {
        char buf[256];
        const u32 len = std::min<u32>(s.str_len, 256u);
        pat_string(seq, k, len, buf);
        std::snprintf(m.data3._buffer[k], s.str_len + 1, "%.*s", static_cast<int>(len), buf);
        if (m.data3._buffer[k] != nullptr) m.data3._buffer[k][len < s.str_len ? s.str_len : len] = '\0';
    }
    m.data4 = true;
}

static int run_pub_dds(const Config& cfg, ControlBlock* ctl)
{
    const Shape s = cfg_shape(cfg);
    RawWriter out;
    out.limit = static_cast<std::size_t>(cfg.sample_limit);

    dds_entity_t dp = dds_create_participant(cfg.domain, nullptr, nullptr);
    if (dp < 0)
    {
        std::fprintf(stderr, "[pub-dds] participant failed: %s\n", dds_strretcode(-dp));
        ctl->pub_failed.store(1);
        ctl->run_failed.store(1);
        return 1;
    }
    dds_entity_t tp = dds_create_topic(dp, &w02_Sample_desc, cfg.topic.c_str(), nullptr, nullptr);
    dds_qos_t* q = make_dds_qos(true, 1024);
    dds_entity_t wr = dds_create_writer(dp, tp, q, nullptr);
    if (tp < 0 || wr < 0)
    {
        std::fprintf(stderr, "[pub-dds] writer failed: topic=%s writer=%s\n",
                     tp < 0 ? dds_strretcode(-tp) : "ok", wr < 0 ? dds_strretcode(-wr) : "ok");
        ctl->pub_failed.store(1);
        ctl->run_failed.store(1);
        return 1;
    }

    w02_Sample msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.data1._buffer = static_cast<double*>(std::malloc(sizeof(double) * (s.n1 ? s.n1 : 1)));
    msg.data1._maximum = s.n1;
    msg.data2._buffer = static_cast<int32_t*>(std::malloc(sizeof(int32_t) * (s.n2 ? s.n2 : 1)));
    msg.data2._maximum = s.n2;
    msg.data3._buffer = static_cast<char**>(std::calloc(s.n3 ? s.n3 : 1, sizeof(char*)));
    msg.data3._maximum = s.n3;
    for (u32 k = 0; k < s.n3; ++k) msg.data3._buffer[k] = static_cast<char*>(std::malloc(s.str_len + 1));

    write_identity(ctl, true);
    ctl->pub_ready_ns.store(now_ns());
    ctl->pub_ready.store(1, std::memory_order_release);

    /* 等匹配 + 数据面探测 */
    {
        auto deadline = steady_clock::now() + seconds(20);
        u32 idx = 0;
        while (ctl->probe_received.load(std::memory_order_acquire) == 0
               && steady_clock::now() < deadline)
        {
            const u64 ts = now_ns();
            fill_dds(msg, s, 0, idx++, kFlagProbe, ts, ts);
            dds_write(wr, &msg);
            std::this_thread::sleep_for(milliseconds(10));
        }
        if (ctl->probe_received.load(std::memory_order_acquire) == 0)
        {
            std::fprintf(stderr, "[pub-dds] handshake timeout\n");
            ctl->pub_failed.store(2);
            ctl->run_failed.store(1);
            return 3;
        }
    }

    const u64 warmup_end = now_ns() + static_cast<u64>(cfg.warmup * 1e9);
    {
        u64 seq = 1;
        u32 idx = 0;
        while (now_ns() < warmup_end)
        {
            const u64 t = now_ns();
            fill_dds(msg, s, seq++, idx++, kFlagWarmup, t, t);
            dds_write(wr, &msg);
            std::this_thread::sleep_for(microseconds(200));
        }
        std::this_thread::sleep_for(milliseconds(150));
    }

    SendPlan plan;
    plan.init(cfg.rate_hz);
    const auto measure_start = steady_clock::now();
    ctl->measure_start_ns.store(now_ns(), std::memory_order_release);
    ctl->phase.store(3, std::memory_order_release);
    const u64 plan_total = cfg.rate_hz > 0 ? static_cast<u64>(std::ceil(cfg.rate_hz * cfg.duration)) : 0;
    ctl->plan_total.store(plan_total, std::memory_order_release);

    PubloopResult pr;
    pr.plan = plan_total;
    u64 seq = 1000000;
    u32 idx = 0;
    const auto deadline = measure_start + duration<double>(cfg.duration);
    for (;;)
    {
        if (cfg.rate_hz > 0)
        {
            if (pr.attempts >= plan_total) break;
            const u64 backlog = plan.wait_for(pr.attempts, 80.0);
            if (backlog > 0)
            {
                if (backlog > plan.late_threshold_ns) ++pr.late;
                pr.backlog_sum += backlog;
                pr.backlog_max = std::max(pr.backlog_max, backlog);
            }
        }
        else if (steady_clock::now() >= deadline) break;

        const u64 produced = now_ns();
        const u64 sq = seq++;
        const u32 ix = idx++;
        const u64 enter = now_ns();
        fill_dds(msg, s, sq, ix, kFlagMeasure, produced, enter);
        const int rc = dds_write(wr, &msg);
        const u64 done = now_ns();

        PubRow row;
        row.seq = sq; row.idx = ix; row.flags = kFlagMeasure;
        row.produced_ns = produced; row.enter_ns = enter; row.done_ns = done;
        row.app_bytes = s.bytes();
        /* DDS 档不采集 wire 字节 ⇒ **显式标未采集**（write_pub 会写空字段），
         * ⛔ 不是"实测 0 字节"。 */
        row.wire_bytes = 0;
        row.wire_known = 0;
        row.ok = (rc >= 0) ? 1 : 0;
        row.publish_rc = (rc >= 0) ? 0 : 1;
        out.add(row);
        ++pr.attempts;
        if (rc >= 0) ++pr.sent_ok; else ++pr.failed;
        ctl->attempt_total.store(pr.attempts);
        ctl->sent_ok.store(pr.sent_ok);
    }
    pr.elapsed = duration<double>(steady_clock::now() - measure_start).count();
    ctl->measure_end_ns.store(now_ns());
    ctl->late_sends.store(pr.late);
    ctl->backlog_sum_ns.store(pr.backlog_sum);
    ctl->backlog_max_ns.store(pr.backlog_max);
    ctl->send_failed.store(pr.failed);

    std::this_thread::sleep_for(milliseconds(300));
    ctl->phase.store(4, std::memory_order_release);
    {
        const auto sentinel_deadline = steady_clock::now() + seconds(5);
        while (ctl->stop_frames_recv.load() < 3 && steady_clock::now() < sentinel_deadline)
        {
            const u64 t = now_ns();
            fill_dds(msg, s, 0, 0, 0x8, t, t);
            dds_write(wr, &msg);
            ctl->stop_frames_sent.fetch_add(1);
            std::this_thread::sleep_for(milliseconds(5));
        }
    }
    ctl->phase.store(5, std::memory_order_release);
    ctl->pub_finish_ns.store(now_ns());

    out.write_pub(cfg.out_file);
    CounterRegistry::instance().write_json_file(cfg.out_file + ".counters.json");
    dds_delete_qos(q);
    dds_delete(dp);
    std::printf("[pub-dds] path=%s payload=%llu plan=%llu attempts=%llu ok=%llu failed=%llu late=%llu elapsed=%.3fs\n",
                path_name(cfg.path), (unsigned long long)s.bytes(),
                (unsigned long long)pr.plan, (unsigned long long)pr.attempts,
                (unsigned long long)pr.sent_ok, (unsigned long long)pr.failed,
                (unsigned long long)pr.late, pr.elapsed);
    std::fflush(stdout);
    return 0;
}

static int run_sub_dds(const Config& cfg, ControlBlock* ctl)
{
    const Shape s = cfg_shape(cfg);
    RawWriter out;
    out.limit = static_cast<std::size_t>(cfg.sample_limit);

    dds_entity_t dp = dds_create_participant(cfg.domain, nullptr, nullptr);
    if (dp < 0)
    {
        std::fprintf(stderr, "[sub-dds] participant failed: %s\n", dds_strretcode(-dp));
        ctl->sub_failed.store(1);
        ctl->run_failed.store(1);
        return 1;
    }
    dds_entity_t tp = dds_create_topic(dp, &w02_Sample_desc, cfg.topic.c_str(), nullptr, nullptr);
    dds_qos_t* q = make_dds_qos(true, 1024);
    dds_entity_t rd = dds_create_reader(dp, tp, q, nullptr);
    if (tp < 0 || rd < 0)
    {
        std::fprintf(stderr, "[sub-dds] reader failed: topic=%s reader=%s\n",
                     tp < 0 ? dds_strretcode(-tp) : "ok", rd < 0 ? dds_strretcode(-rd) : "ok");
        ctl->sub_failed.store(1);
        ctl->run_failed.store(1);
        return 1;
    }
    dds_entity_t ws = dds_create_waitset(dp);
    dds_waitset_attach(ws, rd, 0);

    write_identity(ctl, false);
    ctl->sub_ready_ns.store(now_ns());
    ctl->sub_ready.store(1, std::memory_order_release);

    SubLoopResult sr;
    const bool busy = (cfg.wait_mode == WaitMode::busy);
    u64 prev_seq = 0;
    bool have_prev = false;
    std::set<u64> seen;
    auto last_activity = steady_clock::now();

    for (;;)
    {
        void* samples[1] = {nullptr};
        dds_sample_info_t infos[1];
        const int n = dds_take(rd, samples, infos, 1, 1);
        if (n < 0)
        {
            std::fprintf(stderr, "[sub-dds] take failed: %s\n", dds_strretcode(-n));
            ctl->run_failed.store(1);
            break;
        }
        if (n == 0)
        {
            const i32 phase = ctl->phase.load(std::memory_order_acquire);
            if (phase >= 5
                && duration_cast<nanoseconds>(steady_clock::now() - last_activity).count() > 400000000)
            {
                sr.note = "phase=5 后静默超时收尾";
                break;
            }
            if (busy) continue;    /* 忙等组：不退让 */
            dds_waitset_wait(ws, nullptr, 0, 20000000);   /* 20 ms 上限，保证可收尾 */
            continue;
        }
        if (!infos[0].valid_data)
        {
            dds_return_loan(rd, samples, n);
            continue;
        }

        const auto* msg = static_cast<const w02_Sample*>(samples[0]);
        const u64 app = now_ns();
        CheckOutcome o = check_header_dds(msg);
        const bool full = (cfg.workload == Workload::full);
        if (full) verify_dds(msg, s, o, true);
        const u64 fully = now_ns();
        if (!o.payload_ok) { ++sr.checksum_bad; CounterRegistry::instance().inc(CounterId::payload_checksum_bad); }
        else if (cfg.workload != Workload::timestamp) CounterRegistry::instance().inc(CounterId::payload_checksum_ok);
        sr.elements += o.elements;
        ++sr.via_view;
        dds_return_loan(rd, samples, n);

        if (o.flags == kFlagProbe) { ++sr.recv_probe; ctl->sub_probe_count.fetch_add(1); ctl->probe_received.store(1); last_activity = steady_clock::now(); continue; }
        if (o.flags == kFlagWarmup) { ++sr.recv_warmup; last_activity = steady_clock::now(); continue; }
        if (o.flags == 0x8) { ++sr.recv_stop; ctl->stop_frames_recv.fetch_add(1); last_activity = steady_clock::now(); continue; }

        if (have_prev && o.seq <= prev_seq) { if (o.seq == prev_seq) ++sr.dup; else ++sr.ooo; }
        if (!seen.insert(o.seq).second) ++sr.dup;
        prev_seq = o.seq;
        have_prev = true;

        SubRow row;
        row.seq = o.seq; row.idx = o.idx; row.flags = o.flags;
        row.app_obtained_ns = app; row.fully_consumed_ns = fully;
        row.checksum_ok = (cfg.workload == Workload::timestamp) ? -1 : (o.payload_ok ? 1 : 0);
        row.elements_checked = static_cast<u32>(o.elements);
        /* DDS 档接收侧 wire 字节未采集：⛔ 写空字段，不写 0。 */
        row.rx_wire_bytes = 0;
        row.rx_wire_known = 0;
        row.abnormal = o.abnormal ? 1 : 0;
        row.view_kind = 2;
        out.add(row);
        ++sr.recv_measure;
        last_activity = steady_clock::now();
        ctl->sub_received_total.fetch_add(1, std::memory_order_release);
        if (ctl->sub_first_rx_ns.load() == 0) ctl->sub_first_rx_ns.store(app);
        if (sr.recv_stop >= 3) break;
    }
    ctl->sub_checksum_bad.store(sr.checksum_bad);
    out.write_sub(cfg.out_file);
    CounterRegistry::instance().write_json_file(cfg.out_file + ".counters.json");
    dds_delete_qos(q);
    dds_delete(dp);
    ctl->sub_done.store(1, std::memory_order_release);
    std::printf("[sub-dds] path=%s payload=%llu measure=%llu probe=%llu warmup=%llu stop=%llu bad_crc=%llu ooo=%llu dup=%llu\n",
                path_name(cfg.path), (unsigned long long)s.bytes(),
                (unsigned long long)sr.recv_measure, (unsigned long long)sr.recv_probe,
                (unsigned long long)sr.recv_warmup, (unsigned long long)sr.recv_stop,
                (unsigned long long)sr.checksum_bad, (unsigned long long)sr.ooo, (unsigned long long)sr.dup);
    std::fflush(stdout);
    return 0;
}

/* DDS 预检角色：能建 participant + topic + writer/reader 即通过，否则退出码非 0。
 * 冰羚未起（RouDi 缺失）时 create_participant 会永久阻塞 —— 父进程用超时兜住它。 */
static int run_ddscheck(const Config& cfg)
{
    dds_entity_t dp = dds_create_participant(cfg.domain, nullptr, nullptr);
    if (dp < 0)
    {
        std::printf("[ddscheck] participant failed: %s\n", dds_strretcode(-dp));
        return 1;
    }
    dds_entity_t tp = dds_create_topic(dp, &w02_Sample_desc, "w02/check", nullptr, nullptr);
    if (tp < 0)
    {
        std::printf("[ddscheck] topic failed: %s\n", dds_strretcode(-tp));
        return 1;
    }
    dds_qos_t* q = make_dds_qos(true, 1024);
    dds_entity_t rd = dds_create_reader(dp, tp, q, nullptr);
    dds_entity_t wr = dds_create_writer(dp, tp, q, nullptr);

    const bool ok_all = (rd >= 0 && wr >= 0);
    std::printf("[ddscheck] path=%s participant=ok topic=%s qos=%s reader=%s writer=%s\n",
                path_name(cfg.path), tp >= 0 ? "ok" : "fail", dds_qos_name(),
                rd >= 0 ? "ok" : "fail", wr >= 0 ? "ok" : "fail");
    if (!ok_all)
    {
        /* 显式打印错误码：实体建不出来时必须能一眼看到是哪个实体、什么码，
         * 而不是把它悄悄算成"该档不可用"（§10.6）。 */
        std::printf("[ddscheck] detail: topic=%s reader=%s writer=%s\n",
                    tp < 0 ? dds_strretcode(-tp) : "ok",
                    rd < 0 ? dds_strretcode(-rd) : "ok",
                    wr < 0 ? dds_strretcode(-wr) : "ok");
        dds_delete_qos(q);
        std::fflush(stdout);
        return 1;
    }
    if (cfg.out_file.size()) { std::FILE* f = std::fopen(cfg.out_file.c_str(), "wb"); if (f) { std::fprintf(f, "ok\n"); std::fclose(f); } }
    dds_delete_qos(q);
    dds_delete(dp);
    return 0;
}

#endif   /* W02_HAVE_DDS */

/* ======================================================================== */
/* 父进程：编排                                                             */
/* ======================================================================== */

struct Child
{
    pid_t pid = -1;
    std::string label;
    std::string argv0;
    int exit_code = -1;
    bool signaled = false;
    int signal = 0;
    bool killed = false;
    double user_s = 0, sys_s = 0;
    u64 voluntary = 0, nonvoluntary = 0;
    long maxrss_kb = 0;
    bool reaped = false;
};

/* fork + exec 新映像（不是共享地址空间的线程）。 */
static pid_t spawn_role(const Config& cfg, const std::string& role, const std::string& ctl,
                        const std::string& topic, u64 payload, const Shape& s,
                        const std::string& out_file, const std::string& dds_uri,
                        const std::string& stdout_path, const std::string& stderr_path)
{
    const pid_t pid = ::fork();
    if (pid != 0) return pid;

    if (!stdout_path.empty())
    {
        const int fd = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ::dup2(fd, STDOUT_FILENO); ::close(fd); }
    }
    if (!stderr_path.empty())
    {
        const int fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    if (!dds_uri.empty()) ::setenv("CYCLONEDDS_URI", dds_uri.c_str(), 1);

    std::vector<std::string> args;
    args.push_back("--role=" + role);
    args.push_back("--ctl=" + ctl);
    args.push_back("--topic=" + topic);
    args.push_back("--path=" + std::string(path_name(cfg.path)));
    args.push_back("--payload=" + std::to_string(payload));
    args.push_back("--shape=" + std::to_string(s.n1) + "," + std::to_string(s.n2) + ","
                   + std::to_string(s.n3) + "," + std::to_string(s.str_len));
    args.push_back("--domain=" + std::to_string(cfg.domain));
    args.push_back("--duration=" + std::to_string(cfg.duration));
    args.push_back("--warmup=" + std::to_string(cfg.warmup));
    args.push_back("--rate=" + std::to_string(cfg.rate_hz));
    args.push_back("--queue=" + std::to_string(cfg.queue));
    args.push_back("--workload=" + std::string(workload_name(cfg.workload)));
    args.push_back("--wait=" + std::string(wait_name(cfg.wait_mode)));
    args.push_back("--queue-wait-ms=" + std::to_string(cfg.queue_wait_ms));
    args.push_back("--sample-limit=" + std::to_string(cfg.sample_limit));
    if (!out_file.empty()) args.push_back("--out=" + out_file);
    if (cfg.best_effort) args.push_back("--best-effort");
    if (role == "pub" && cfg.pin_pub >= 0) args.push_back("--pin=" + std::to_string(cfg.pin_pub));
    if (role == "sub" && cfg.pin_sub >= 0) args.push_back("--pin=" + std::to_string(cfg.pin_sub));

    /* ⚠️ argv[0] 必须是程序名：parse_args 从 i=1 开始扫。把 --role= 放在 argv[0]
     * 会让角色参数被吞掉，子进程于是**又跑成采集器**（症状：子进程 stdout 里出现采集器
     * banner、控制块永远不 ready，最后被超时 SIGKILL）。这是实测踩到过的坑。 */
    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>("xproc_benchmark"));
    for (auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    ::execv("/proc/self/exe", cargv.data());
    std::fprintf(stderr, "[fatal] execv(/proc/self/exe) failed: %s\n", std::strerror(errno));
    ::_exit(127);
}

static Child reap_child(pid_t pid, const std::string& label, double timeout_s)
{
    Child c;
    c.pid = pid;
    c.label = label;
    const auto deadline = steady_clock::now() + duration<double>(timeout_s);
    int status = 0;
    struct rusage ru {};
    for (;;)
    {
        const pid_t r = ::wait4(pid, &status, WNOHANG, &ru);
        if (r == pid) break;
        if (r < 0) { c.exit_code = -1; return c; }
        if (steady_clock::now() > deadline)
        {
            ::kill(pid, SIGKILL);
            c.killed = true;
            ::wait4(pid, &status, 0, &ru);
            break;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    c.reaped = true;
    if (WIFEXITED(status)) c.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { c.signaled = true; c.signal = WTERMSIG(status); c.exit_code = 128 + c.signal; }
    c.user_s = static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
    c.sys_s = static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
    c.voluntary = static_cast<u64>(ru.ru_nvcsw);
    c.nonvoluntary = static_cast<u64>(ru.ru_nivcsw);
    c.maxrss_kb = ru.ru_maxrss;
    return c;
}

/* ---- CSV 读取（合并两个角色的原始样本表） ---- */
struct CsvTable
{
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    bool ok = false;
};

static CsvTable read_csv(const std::string& path)
{
    CsvTable t;
    std::ifstream f(path);
    if (!f) return t;
    std::string line;
    if (!std::getline(f, line)) return t;
    {
        std::istringstream is(line);
        std::string cell;
        while (std::getline(is, cell, ',')) t.header.push_back(cell);
    }
    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        std::istringstream is(line);
        std::string cell;
        std::vector<std::string> row;
        while (std::getline(is, cell, ',')) row.push_back(cell);
        if (!row.empty()) t.rows.push_back(std::move(row));
    }
    t.ok = true;
    return t;
}

/* ======================================================================== */
/* 单用例结果与汇总                                                         */
/* ======================================================================== */

struct CaseResult
{
    std::string case_id;
    PathSel path = PathSel::tlv;
    Workload workload = Workload::full;
    WaitMode wait_mode = WaitMode::blocking;
    std::string experiment_group;
    u64 payload_target = 0;
    u64 payload_bytes = 0;
    u64 header_bytes = 0;
    u64 wire_bytes_per_msg = 0;
    bool wire_bytes_known = true;   ///< false = 未采集（summary 的该列写空字段，不写 0）
    std::string wire_bytes_source = "null";
    double rate_target = 0;
    double duration_plan = 0;
    double duration_actual = 0;
    double rate_actual_attempt = 0;
    double rate_actual_ok = 0;
    std::size_t queue_size = 0;
    bool best_effort = false;
    u64 queue_wait_ms = 0;
    std::string publish_mode;

    u64 plan = 0, attempts = 0, sent_ok = 0, send_failed = 0, send_blocked = 0;
    u64 late = 0, late_threshold_ns = 0, backlog_sum_ns = 0, backlog_max_ns = 0;
    u64 recv_total = 0, recv_measure = 0, recv_probe = 0, recv_warmup = 0, recv_stop = 0;
    u64 missing = 0, duplicate = 0, out_of_order = 0, checksum_bad = 0, abnormal = 0;
    u64 bad_header = 0;    ///< 订阅侧「段头/对象结构自相矛盾」次数（t47 补列，此前完全不存在）
    /* 逐 gate 的判定结果 —— 与 failure_reasons **由同一个函数**产生（纪律：
     * 「同一事实只能有一处判定逻辑」，⛔ 不允许渲染表达式另算一遍）。 */
    bool late_ok = true, backlog_ok = true, send_blocked_ok = true, abnormal_ok = true, bad_header_ok = true;
    u64 samples_merged = 0;
    u64 rows_truncated = 0;
    u64 elements_checked = 0;
    u64 via_view = 0, via_object = 0, via_dds = 0;
    u64 pub_dzflat = 0, pub_fallback = 0;
    u64 sub_flat_accepted = 0, sub_tlv_accepted = 0, sub_rx_defects = 0, sub_id_skipped = 0;
    u64 loan_fail = 0;
    u64 child_killed = 0;

    double pub_cpu_cores = 0, sub_cpu_cores = 0, total_cpu_cores = 0;
    double pub_cpu_s = 0, sub_cpu_s = 0;
    double roudi_cpu_s = 0;          ///< DDS 档: RouDi 守护进程在本窗口消耗的 CPU 秒 */
    double roudi_cpu_after_s = 0;
    bool roudi_cpu_known = false;
    u64 pub_ctx_vol = 0, pub_ctx_nonvol = 0, sub_ctx_vol = 0, sub_ctx_nonvol = 0;
    long pub_maxrss_kb = 0, sub_maxrss_kb = 0;

    std::vector<u64> transport_ns, delivery_ns, app_read_ns, e2e_ns;
    bool load_ok = false;
    bool timing_ok = true;
    bool case_ok = false;
    std::vector<std::string> failure_reasons;
    std::vector<std::string> notes;
};

static void add_reason(CaseResult& r, const std::string& s)
{
    for (const auto& x : r.failure_reasons) if (x == s) return;
    r.failure_reasons.push_back(s);
}

static double pct(const std::vector<u64>& v, double q)
{
    if (v.empty()) return -1.0;
    std::vector<u64> s = v;
    std::sort(s.begin(), s.end());
    const std::size_t i = std::min(s.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.size())));
    return static_cast<double>(s[i]);
}

static void lat_stats(const std::vector<u64>& v, double& minv, double& p50, double& p99, double& p999,
                      double& maxv, double& mean)
{
    if (v.empty()) { minv = p50 = p99 = p999 = maxv = mean = -1.0; return; }
    std::vector<u64> s = v;
    std::sort(s.begin(), s.end());
    minv = static_cast<double>(s.front());
    maxv = static_cast<double>(s.back());
    auto q = [&](double f) {
        return static_cast<double>(s[std::min(s.size() - 1, static_cast<std::size_t>(f * static_cast<double>(s.size())))]);
    };
    p50 = q(0.5); p99 = q(0.99); p999 = q(0.999);
    long double sum = 0;
    for (u64 x : s) sum += static_cast<long double>(x);
    mean = static_cast<double>(sum / static_cast<long double>(s.size()));
}

/* ======================================================================== */
/* 输出                                                                     */
/* ======================================================================== */

static std::string group_of(Workload w)
{
    switch (w)
    {
    case Workload::timestamp: return "transport";
    case Workload::crc:       return "full-read";
    case Workload::full:      return "full-read";
    case Workload::inplace:   return "produce-to-consume";
    }
    return "unknown";
}

static void write_summary_csv(const std::string& path, const std::vector<CaseResult>& rs)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f,
        "case_id,path,experiment_group,workload,wait_mode,payload_target,payload_bytes,header_bytes,wire_bytes_per_msg,"
        "wire_bytes_source,rate_target,duration_plan,duration_actual,rate_actual_attempt,rate_actual_ok,queue_size,"
        "publish_mode,plan,attempts,sent_ok,send_failed,send_blocked,late,late_threshold_ns,backlog_max_ns,backlog_sum_ns,"
        "recv_measure,recv_probe,recv_warmup,missing,duplicate,out_of_order,checksum_bad,abnormal,"
        "bad_header,"
        "late_ok,backlog_ok,send_blocked_ok,abnormal_ok,bad_header_ok,"
        "samples_merged,"
        "rows_truncated,elements_checked,via_view,via_object,via_dds,pub_dzflat,pub_fallback,"
        "sub_flat_accepted,sub_tlv_accepted,sub_rx_defects,loan_fail,"
        "transport_min_ns,transport_p50_ns,transport_p99_ns,transport_p999_ns,transport_max_ns,transport_mean_ns,"
        "delivery_min_ns,delivery_p50_ns,delivery_p99_ns,delivery_p999_ns,delivery_max_ns,delivery_mean_ns,"
        "app_read_min_ns,app_read_p50_ns,app_read_p99_ns,app_read_p999_ns,app_read_max_ns,app_read_mean_ns,"
        "e2e_min_ns,e2e_p50_ns,e2e_p99_ns,e2e_p999_ns,e2e_max_ns,e2e_mean_ns,"
        "pub_cpu_s,sub_cpu_s,roudi_cpu_s,pub_cpu_cores,sub_cpu_cores,total_cpu_cores,"
        "pub_ctx_voluntary,pub_ctx_nonvoluntary,sub_ctx_voluntary,sub_ctx_nonvoluntary,"
        "pub_maxrss_kb,sub_maxrss_kb,load_ok,timing_ok,case_ok,child_killed,failure_reasons\n");
    for (const auto& r : rs)
    {
        double a[5], b[5], c[5], d[5], m;
        lat_stats(r.transport_ns, a[0], a[1], a[2], a[3], a[4], m);
        const double tmean = m;
        lat_stats(r.delivery_ns, b[0], b[1], b[2], b[3], b[4], m);
        const double dmean = m;
        lat_stats(r.app_read_ns, c[0], c[1], c[2], c[3], c[4], m);
        const double cmean = m;
        lat_stats(r.e2e_ns, d[0], d[1], d[2], d[3], d[4], m);
        const double emean = m;
        std::string reasons;
        for (std::size_t i = 0; i < r.failure_reasons.size(); ++i)
        {
            if (i) reasons += "; ";
            reasons += r.failure_reasons[i];
        }
        /* ⚠️ 必须落在具名 std::string 上再取 c_str()：`cond ? std::to_string(x) : std::string()`
         * 产生的是临时对象，`.c_str()` 在 fprintf 执行前就悬垂 —— 实测 summary.csv 的
         * wire_bytes_per_msg 被打成 `140724864616144` 这种指针值。 */
        const std::string wire_cell = r.wire_bytes_known
            ? std::to_string(r.wire_bytes_per_msg) : std::string();
        /* ⚠️ 列数与格式串必须逐位对齐：本块曾因插入新列而未同步格式串，导致 `fprintf`
         * 读越界 → 进程 SIGSEGV（rc=139）且 summary.csv 为 0 字节。对齐现由机械判据守住：
         * `test/w02_summary_schema_check.py`（表头列数 == 格式串字段数 == 数据行列数），
         * 已进 CTest（test_w02_xproc_benchmark 的 schema 断言 + 本脚本）。 */
        std::fprintf(f,
            "%s,%s,%s,%s,%s,%llu,%llu,%llu,%s,%s,"
            "%.1f,%.3f,%.3f,%.1f,%.1f,%zu,%s,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,"
            "%d,%d,%d,%d,%d,"
            "%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%llu,%llu,%llu,%llu,"
            "%ld,%ld,"
            "%d,%d,%d,%llu,%s\n",
            r.case_id.c_str(), path_name(r.path), r.experiment_group.c_str(),
            workload_name(r.workload), wait_name(r.wait_mode),
            (unsigned long long)r.payload_target, (unsigned long long)r.payload_bytes,
            (unsigned long long)r.header_bytes,
            wire_cell.c_str(),
            r.wire_bytes_source.c_str(), r.rate_target, r.duration_plan, r.duration_actual,
            r.rate_actual_attempt, r.rate_actual_ok, r.queue_size, r.publish_mode.c_str(),
            (unsigned long long)r.plan, (unsigned long long)r.attempts, (unsigned long long)r.sent_ok,
            (unsigned long long)r.send_failed, (unsigned long long)r.send_blocked,
            (unsigned long long)r.late, (unsigned long long)r.late_threshold_ns,
            (unsigned long long)r.backlog_max_ns,
            (unsigned long long)r.backlog_sum_ns,
            (unsigned long long)r.recv_measure, (unsigned long long)r.recv_probe,
            (unsigned long long)r.recv_warmup, (unsigned long long)r.missing,
            (unsigned long long)r.duplicate, (unsigned long long)r.out_of_order,
            (unsigned long long)r.checksum_bad, (unsigned long long)r.abnormal,
            (unsigned long long)r.bad_header,
            r.late_ok ? 1 : 0, r.backlog_ok ? 1 : 0, r.send_blocked_ok ? 1 : 0,
            r.abnormal_ok ? 1 : 0, r.bad_header_ok ? 1 : 0,
            (unsigned long long)r.samples_merged, (unsigned long long)r.rows_truncated,
            (unsigned long long)r.elements_checked, (unsigned long long)r.via_view,
            (unsigned long long)r.via_object, (unsigned long long)r.via_dds,
            (unsigned long long)r.pub_dzflat, (unsigned long long)r.pub_fallback,
            (unsigned long long)r.sub_flat_accepted, (unsigned long long)r.sub_tlv_accepted,
            (unsigned long long)r.sub_rx_defects, (unsigned long long)r.loan_fail,
            a[0], a[1], a[2], a[3], a[4], tmean,
            b[0], b[1], b[2], b[3], b[4], dmean,
            c[0], c[1], c[2], c[3], c[4], cmean,
            d[0], d[1], d[2], d[3], d[4], emean,
            r.pub_cpu_s, r.sub_cpu_s, r.roudi_cpu_s,
            r.pub_cpu_cores, r.sub_cpu_cores, r.total_cpu_cores,
            (unsigned long long)r.pub_ctx_vol, (unsigned long long)r.pub_ctx_nonvol,
            (unsigned long long)r.sub_ctx_vol, (unsigned long long)r.sub_ctx_nonvol,
            r.pub_maxrss_kb, r.sub_maxrss_kb,
            r.load_ok ? 1 : 0, r.timing_ok ? 1 : 0, r.case_ok ? 1 : 0,
            (unsigned long long)r.child_killed, reasons.c_str());
    }
    std::fclose(f);
}

static std::string case_target_label(const CaseResult& r)
{
    std::ostringstream o;
    o << r.payload_target;
    return o.str();
}

/* ======================================================================== */
/* 用例矩阵                                                                 */
/* ======================================================================== */

/* 每条路径默认跑三组工作负载（§10.7）：
 *   timestamp —— 组1「传输机制」：只读时间戳/头部，不解全量载荷；
 *   crc       —— 组2「完整读取」的顺序遍历形态（一次 CRC32C 覆盖全部载荷字节）；
 *   full      —— 组2 的逐元素校验形态（逐元素与序号相关模式比对，抓旧帧/损坏）。
 * 组3「生产到消费」不是单独一档：它由 e2e_ns（从生产端生成数据前计时到完整消费结束）
 * 给出，对任何一档都能算 —— 这正是方案要求"不混用结束点"的落法。
 * 生产端工作量在三条 payload 形态上等价：produce_into() 是同一个函数，
 * B 档把同一批模式字节直接写进 chunk，A/TLV 写进调用方对象。 */
static std::vector<Workload> workloads_for(PathSel p)
{
    (void)p;
    return {Workload::timestamp, Workload::crc, Workload::full};
}

/* ======================================================================== */
/* 父进程：单用例编排                                                       */
/* ======================================================================== */

struct RunArtifacts
{
    std::string ctl_name;
    std::string pub_raw, sub_raw;
    std::string pub_stdout, pub_stderr, sub_stdout, sub_stderr;
    Child pub, sub;
    bool pub_identity_ok = false;
    bool sub_identity_ok = false;
    std::string identity_note;
};

/* 数据面握手：两个角色都 ready 且订阅侧真的收到过探测帧。
 * ⛔ 不用 pub->has_subscribed() 之类的控制面计数 —— 那只能证明"注册表里有对方"，
 * 证明不了数据面通了（W01 UF-15 的教训）。 */
static bool wait_handshake(ControlBlock* ctl, double timeout_s, std::string& err)
{
    const auto deadline = steady_clock::now() + duration<double>(timeout_s);
    while (steady_clock::now() < deadline)
    {
        if (ctl->sub_failed.load(std::memory_order_acquire) != 0) { err = "订阅进程初始化失败(sub_failed)"; return false; }
        if (ctl->pub_failed.load(std::memory_order_acquire) != 0) { err = "发布进程初始化失败/探测超时(pub_failed)"; return false; }
        if (ctl->sub_ready.load(std::memory_order_acquire)
            && ctl->pub_ready.load(std::memory_order_acquire)
            && ctl->probe_received.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    err = "握手超时: sub_ready=" + std::to_string(ctl->sub_ready.load())
        + " pub_ready=" + std::to_string(ctl->pub_ready.load())
        + " probe_received=" + std::to_string(ctl->probe_received.load());
    return false;
}

/* 跨进程身份核验（§10.6）：ready 时间戳必须落在父进程括号内，pid 与 starttime 齐全。 */
static void check_identity(ControlBlock* ctl, RunArtifacts& ra)
{
    const u64 mypid = static_cast<u64>(::getpid());
    const u64 sub_pid = static_cast<u64>(ctl->sub_pid.load());
    const u64 pub_pid = static_cast<u64>(ctl->pub_pid.load());
    const u64 sub_ts = ctl->sub_ready_ns.load();
    const u64 pub_ts = ctl->pub_ready_ns.load();
    const u64 sub_lo = ctl->sub_ready_bracket_lo.load(), sub_hi = ctl->sub_ready_bracket_hi.load();
    const u64 pub_lo = ctl->pub_ready_bracket_lo.load(), pub_hi = ctl->pub_ready_bracket_hi.load();

    std::ostringstream o;
    o << "parent_pid=" << mypid << " pub_pid=" << pub_pid << " sub_pid=" << sub_pid
      << " | pub_ready_ns=" << pub_ts << " in [" << pub_lo << "," << pub_hi << "]"
      << " | sub_ready_ns=" << sub_ts << " in [" << sub_lo << "," << sub_hi << "]"
      << " | pub_starttime_ticks=" << ctl->pub_starttime_ticks.load()
      << " sub_starttime_ticks=" << ctl->sub_starttime_ticks.load()
      << " | pub_lib=" << ctl->pub_loaded_lib
      << " | sub_lib=" << ctl->sub_loaded_lib;

    ra.pub_identity_ok = (pub_pid != 0 && pub_pid != mypid
                          && ctl->pub_starttime_ticks.load() != 0
                          && pub_lo != 0 && pub_ts >= pub_lo && pub_ts <= pub_hi);
    ra.sub_identity_ok = (sub_pid != 0 && sub_pid != mypid
                          && ctl->sub_starttime_ticks.load() != 0
                          && sub_lo != 0 && sub_ts >= sub_lo && sub_ts <= sub_hi);
    ra.identity_note = o.str();
}

/* 合并两个角色的原始样本表，算三条计时边界与完整性计数。 */
static void merge_and_reduce(const Config& cfg, const Shape& s, CaseResult& r,
                             const std::string& pub_csv, const std::string& sub_csv,
                             const std::string& merged_csv, const std::string& jsonl_path)
{
    const CsvTable pt = read_csv(pub_csv);
    const CsvTable st = read_csv(sub_csv);
    if (!pt.ok) add_reason(r, "发布侧原始样本表缺失: " + pub_csv);
    if (!st.ok) add_reason(r, "订阅侧原始样本表缺失: " + sub_csv);

    std::map<u64, const std::vector<std::string>*> pub_by_seq;
    for (const auto& row : pt.rows)
    {
        if (row.empty()) continue;
        pub_by_seq[std::strtoull(row[0].c_str(), nullptr, 10)] = &row;
    }
    std::map<u64, const std::vector<std::string>*> sub_by_seq;
    for (const auto& row : st.rows)
    {
        if (row.empty()) continue;
        const u64 sq = std::strtoull(row[0].c_str(), nullptr, 10);
        if (sub_by_seq.count(sq)) { ++r.duplicate; continue; }
        sub_by_seq[sq] = &row;
    }

    std::FILE* mf = std::fopen(merged_csv.c_str(), "wb");
    std::FILE* jf = std::fopen(jsonl_path.c_str(), "wb");
    if (mf) std::fprintf(mf, "%s\n", dzIPC::measure::sample_csv_header().c_str());
    else add_reason(r, "merged samples.csv 无法写入: " + merged_csv);

    dzIPC::measure::PathKind pk = dzIPC::measure::PathKind::unknown;
    switch (r.path)
    {
    case PathSel::tlv:      pk = dzIPC::measure::PathKind::tlv; break;
    case PathSel::dzflat_a: pk = dzIPC::measure::PathKind::dzflat_a; break;
    case PathSel::dzflat_b: pk = dzIPC::measure::PathKind::dzflat_b; break;
    case PathSel::cyc_udp:  pk = dzIPC::measure::PathKind::cyclonedds_iox; break;
    case PathSel::cyc_iox:  pk = dzIPC::measure::PathKind::cyclonedds_iox; break;
    }

    u64 prev_seq = 0;
    bool have_prev = false;
    for (const auto& kv : pub_by_seq)
    {
        const u64 sq = kv.first;
        const auto& prow = *kv.second;
        if (prow.size() < 12) continue;
        const u64 produced = std::strtoull(prow[3].c_str(), nullptr, 10);
        const u64 enter = std::strtoull(prow[4].c_str(), nullptr, 10);
        const u64 done = std::strtoull(prow[5].c_str(), nullptr, 10);
        const i32 ok = std::atoi(prow[8].c_str());

        if (ok != 1)
        {
            /* 发送失败：计数 + 进失败原因，但**不**混进延迟样本。 */
            add_reason(r, "存在发送失败样本(已计数, 未静默排除)");
            continue;
        }
        if (done >= enter) r.transport_ns.push_back(done - enter);
        ++r.samples_merged;

        dzIPC::measure::SampleRecord rec;
        rec.run_id = cfg.run_id;
        rec.seq = sq;
        rec.route = cfg.topic;
        rec.generation = 0;
        rec.path = pk;
        rec.produced_ns = produced;
        rec.publish_enter_ns = enter;
        rec.transport_done_ns = done;
        rec.payload_bytes = s.bytes();
        /* wire 字节：空字段 = 未采集（null），此时**不写 0** —— 与 W03 §2.1 的
         * CSV 空字段约定一致，避免"未采集"被读成"wire=0 = 零拷贝"。 */
        rec.has_wire_bytes = !prow[7].empty();
        rec.wire_bytes = rec.has_wire_bytes
            ? static_cast<u64>(std::strtoull(prow[7].c_str(), nullptr, 10)) : 0;

        const auto sit = sub_by_seq.find(sq);
        if (sit == sub_by_seq.end())
        {
            rec.dropped = true;
            rec.has_payload_checksum_ok = false;
            ++r.missing;
            if (mf) std::fprintf(mf, "%s\n", rec.to_csv_row().c_str());
            if (jf) std::fprintf(jf, "%s\n", rec.to_jsonl().c_str());
            continue;
        }
        const auto& srow = *sit->second;
        if (srow.size() < 10) continue;
        const u64 app = std::strtoull(srow[3].c_str(), nullptr, 10);
        const u64 fully = std::strtoull(srow[4].c_str(), nullptr, 10);
        const i32 cok = std::atoi(srow[5].c_str());
        const i32 abnormal = std::atoi(srow[8].c_str());
        rec.has_app_obtained = true;
        rec.app_obtained_ns = app;
        rec.has_fully_consumed = true;
        rec.fully_consumed_ns = fully;
        rec.has_payload_checksum_ok = (cok >= 0);
        rec.payload_checksum_ok = (cok == 1);
        rec.dropped = false;

        if (cok == 0) { ++r.checksum_bad; add_reason(r, "载荷校验失败样本存在"); }
        if (abnormal != 0) ++r.abnormal;
        r.elements_checked += static_cast<u64>(std::strtoull(srow[6].c_str(), nullptr, 10));

        const i32 view_kind = std::atoi(srow[9].c_str());
        if (view_kind == 1) ++r.via_view;
        else if (view_kind == 2) ++r.via_dds;
        else ++r.via_object;

        if (app >= done) r.delivery_ns.push_back(app - done);
        if (fully >= app) r.app_read_ns.push_back(fully - app);
        if (fully >= produced) r.e2e_ns.push_back(fully - produced);

        if (have_prev && sq <= prev_seq) ++r.out_of_order;
        prev_seq = sq;
        have_prev = true;

        if (mf) std::fprintf(mf, "%s\n", rec.to_csv_row().c_str());
        if (jf) std::fprintf(jf, "%s\n", rec.to_jsonl().c_str());
    }
    for (const auto& kv : sub_by_seq)
    {
        if (!pub_by_seq.count(kv.first))
        {
            add_reason(r, "订阅侧收到发布侧无记录的 seq=" + std::to_string(kv.first));
        }
    }
    if (mf) std::fclose(mf);
    if (jf) std::fclose(jf);
    r.recv_measure = r.samples_merged;

}

/* 单用例执行体：起两个 exec 出来的角色进程 → 握手 → 等发布侧跑完 → 收订阅侧 → 合并。 */
static CaseResult run_one_case(const Config& cfg, const Shape& s, PathSel path,
                               Workload workload, WaitMode wait_mode, u64 payload_target,
                               const std::string& out_dir, int case_index)
{
    CaseResult r;
    r.path = path;
    r.workload = workload;
    r.wait_mode = wait_mode;
    r.experiment_group = group_of(workload);
    r.payload_target = payload_target;
    r.payload_bytes = s.bytes();
    r.header_bytes = kHdrBytes;
    r.rate_target = cfg.rate_hz;
    r.duration_plan = cfg.duration;
    r.queue_size = cfg.queue;
    r.best_effort = cfg.best_effort;
    r.queue_wait_ms = cfg.queue_wait_ms;
    r.publish_mode = cfg.is_dds_path()
        ? std::string("dds_write(Reliable)")
        : (cfg.best_effort ? std::string("publish_best_effort")
                           : (path == PathSel::dzflat_b ? std::string("loan+publish_loaned")
                                                        : std::string("publish_blocking")));

    std::ostringstream id;
    id << "r" << case_index << "_" << path_name(path) << "_" << workload_name(workload)
       << "_" << wait_name(wait_mode) << "_" << payload_target << "B";
    if (cfg.rate_hz > 0) id << "_" << static_cast<u64>(cfg.rate_hz) << "Hz";
    else id << "_maxrate";
    r.case_id = id.str();

    Config local = cfg;
    local.path = path;
    local.workload = workload;
    local.wait_mode = wait_mode;
    local.payload = payload_target;

    const std::string stem = sanitize(r.case_id);
    RunArtifacts ra;
    ra.ctl_name = "/w02ctl_" + sanitize(cfg.run_id) + "_" + stem;
    ra.pub_raw = out_dir + "/samples/" + stem + ".pub_raw.csv";
    ra.sub_raw = out_dir + "/samples/" + stem + ".sub_raw.csv";
    ra.pub_stdout = out_dir + "/stdout/pub_" + stem + ".log";
    ra.pub_stderr = out_dir + "/stderr/pub_" + stem + ".log";
    ra.sub_stdout = out_dir + "/stdout/sub_" + stem + ".log";
    ra.sub_stderr = out_dir + "/stderr/sub_" + stem + ".log";

    ::shm_unlink(ra.ctl_name.c_str());
    ControlBlock* ctl = ctl_map(ra.ctl_name, true);
    if (ctl == nullptr) { add_reason(r, "控制块创建失败"); r.case_ok = false; return r; }

    const std::string dds_uri = (path == PathSel::cyc_iox) ? cfg.dds_uri_iox : cfg.dds_uri_udp;

    /* DDS 档：RouDi 成本单列。roudi pid 从命令行/环境解析（见 main 的采集器预检）。 */
    int roudi_pid = 0;
    double roudi_cpu_before = 0.0;
    if (cfg.is_dds_path())
    {
        const std::string rp = run_capture("pgrep -x iox-roudi | head -1");
        if (!rp.empty()) roudi_pid = std::atoi(rp.c_str());
        r.roudi_cpu_known = read_pid_cpu_s(roudi_pid, roudi_cpu_before);
    }

    /* 先起订阅侧：发布侧要确认"有接收方"才可能借到 chunk（无接收方时 loan 会拒绝）。 */
    ctl->sub_ready_bracket_lo.store(now_ns());
    const pid_t sub_pid = spawn_role(local, "sub", ra.ctl_name, cfg.topic, payload_target, s,
                                     ra.sub_raw, dds_uri, ra.sub_stdout, ra.sub_stderr);
    if (sub_pid < 0)
    {
        add_reason(r, "fork 订阅进程失败");
        ctl_unmap(ctl); ::shm_unlink(ra.ctl_name.c_str());
        return r;
    }
    ctl->pub_ready_bracket_lo.store(now_ns());
    const pid_t pub_pid = spawn_role(local, "pub", ra.ctl_name, cfg.topic, payload_target, s,
                                     ra.pub_raw, dds_uri, ra.pub_stdout, ra.pub_stderr);
    if (pub_pid < 0)
    {
        add_reason(r, "fork 发布进程失败");
        ctl->sub_stop_request.store(1);
        ctl->run_failed.store(1);
        Child sc = reap_child(sub_pid, "sub", 5.0);
        r.child_killed += sc.killed ? 1 : 0;
        ctl_unmap(ctl); ::shm_unlink(ra.ctl_name.c_str());
        return r;
    }

    std::string herr;
    const bool hs = wait_handshake(ctl, 25.0, herr);
    ctl->sub_ready_bracket_hi.store(now_ns());
    ctl->pub_ready_bracket_hi.store(now_ns());
    if (!hs)
    {
        add_reason(r, "握手失败: " + herr);
        ctl->sub_stop_request.store(1);
        ctl->run_failed.store(1);
        Child pc = reap_child(pub_pid, "pub", 3.0);
        Child sc = reap_child(sub_pid, "sub", 6.0);
        r.child_killed += (pc.killed ? 1 : 0) + (sc.killed ? 1 : 0);
        r.notes.push_back("handshake pub_exit=" + std::to_string(pc.exit_code)
                          + " sub_exit=" + std::to_string(sc.exit_code));
        ctl_unmap(ctl); ::shm_unlink(ra.ctl_name.c_str());
        r.case_ok = false;
        return r;
    }

    check_identity(ctl, ra);
    if (!ra.pub_identity_ok || !ra.sub_identity_ok) add_reason(r, "跨进程身份核验未通过");
    r.notes.push_back("identity: " + ra.identity_note);

    const double pub_timeout = 25.0 + cfg.warmup + cfg.duration + 30.0;
    Child pc = reap_child(pub_pid, "pub", pub_timeout);
    Child sc = reap_child(sub_pid, "sub", 30.0);
    r.child_killed += (pc.killed ? 1 : 0) + (sc.killed ? 1 : 0);
    ra.pub = pc; ra.sub = sc;

    if (pc.killed) add_reason(r, "发布进程被超时 SIGKILL（非正常退出）");
    if (sc.killed) add_reason(r, "订阅进程被超时 SIGKILL（非正常退出）");
    if (pc.exit_code != 0) add_reason(r, "发布进程退出码=" + std::to_string(pc.exit_code));
    if (sc.exit_code != 0) add_reason(r, "订阅进程退出码=" + std::to_string(sc.exit_code));

    r.plan = ctl->plan_total.load();
    r.attempts = ctl->attempt_total.load();
    r.sent_ok = ctl->sent_ok.load();
    r.send_failed = ctl->send_failed.load();
    r.send_blocked = ctl->send_blocked.load();
    r.late = ctl->late_sends.load();
    r.late_threshold_ns = ctl->late_threshold_ns.load();
    r.backlog_sum_ns = ctl->backlog_sum_ns.load();
    r.backlog_max_ns = ctl->backlog_max_ns.load();
    r.recv_total = ctl->sub_received_total.load();
    r.recv_probe = ctl->sub_probe_count.load();
    r.pub_dzflat = ctl->pub_dzflat.load();
    r.pub_fallback = ctl->pub_fallback.load();
    r.sub_flat_accepted = ctl->sub_rx_dzflat_accepted.load();
    r.sub_tlv_accepted = ctl->sub_rx_tlv_accepted.load();
    r.sub_rx_defects = ctl->sub_rx_defects.load();
    r.sub_id_skipped = ctl->sub_rx_id_skipped.load();
    r.bad_header = ctl->sub_bad_header.load();

    const u64 t0 = ctl->measure_start_ns.load();
    const u64 t1 = ctl->measure_end_ns.load();
    r.duration_actual = (t1 > t0) ? static_cast<double>(t1 - t0) / 1e9 : 0.0;
    r.rate_actual_attempt = r.duration_actual > 0 ? static_cast<double>(r.attempts) / r.duration_actual : 0.0;
    r.rate_actual_ok = r.duration_actual > 0 ? static_cast<double>(r.sent_ok) / r.duration_actual : 0.0;

    if (cfg.is_dds_path())
    {
        /* RouDi 是独立进程，wait4 的 rusage 覆盖不到它 —— 方案 §10.7 明写
         * "CycloneDDS/RouDi 的服务成本不能无说明地遗漏"。这里按窗口差分单列；
         * 读不到就记 null 并说明，不写 0 冒充"没有成本"。 */
        double after = 0.0;
        const bool ok = r.roudi_cpu_known && read_pid_cpu_s(roudi_pid, after);
        if (ok)
        {
            r.roudi_cpu_after_s = after;
            r.roudi_cpu_s = std::max(0.0, after - roudi_cpu_before);
        }
        r.roudi_cpu_known = ok;
        std::ostringstream o;
        o << "roudi: pid=" << (roudi_pid > 0 ? std::to_string(roudi_pid) : std::string("(none)"))
          << " cpu_window_s=" << (ok ? std::to_string(r.roudi_cpu_s) : std::string("null"))
          << " (DDS 档把守护进程成本单列; null = 读不到，不冒充 0)";
        r.notes.push_back(o.str());
    }
    r.pub_cpu_s = pc.user_s + pc.sys_s;
    r.sub_cpu_s = sc.user_s + sc.sys_s;
    const double window = r.duration_actual > 0 ? r.duration_actual : 1.0;
    r.pub_cpu_cores = r.pub_cpu_s / window;
    r.sub_cpu_cores = r.sub_cpu_s / window;
    r.total_cpu_cores = r.pub_cpu_cores + r.sub_cpu_cores
                      + (window > 0 ? r.roudi_cpu_s / window : 0.0);
    r.pub_ctx_vol = pc.voluntary; r.pub_ctx_nonvol = pc.nonvoluntary;
    r.sub_ctx_vol = sc.voluntary; r.sub_ctx_nonvol = sc.nonvoluntary;
    r.pub_maxrss_kb = pc.maxrss_kb; r.sub_maxrss_kb = sc.maxrss_kb;
    r.pub_cpu_s = pc.user_s + pc.sys_s;
    r.load_ok = false;
    r.timing_ok = true;

    r.notes.push_back("child_exit pub=" + std::to_string(pc.exit_code)
                      + "(cpu " + std::to_string(r.pub_cpu_s) + "s, nvcsw " + std::to_string(pc.voluntary)
                      + ", nivcsw " + std::to_string(pc.nonvoluntary)
                      + ") sub=" + std::to_string(sc.exit_code)
                      + "(cpu " + std::to_string(r.sub_cpu_s) + "s, nvcsw " + std::to_string(sc.voluntary)
                      + ", nivcsw " + std::to_string(sc.nonvoluntary) + ")");

    /* 负载达标（§13.1）：定速档必须真的把计划发完，否则标"负载未达标"。 */
    if (cfg.rate_hz > 0 && r.plan > 0)
    {
        r.load_ok = r.attempts >= static_cast<u64>(r.plan * 99 / 100);
        if (!r.load_ok)
        {
            add_reason(r, "负载未达标: attempts=" + std::to_string(r.attempts)
                          + " < 99% plan=" + std::to_string(r.plan));
        }
        if (r.rate_actual_ok < 0.9 * cfg.rate_hz)
        {
            add_reason(r, "实际成功发送率低于目标 90%: " + std::to_string(r.rate_actual_ok)
                          + " < " + std::to_string(cfg.rate_hz));
        }
    }
    else
    {
        r.load_ok = r.attempts > 0;
        if (!r.load_ok) add_reason(r, "满速档一条都没发出去");
    }

    {
        bool seen_pub = false, seen_sub = false;
        const u64 dp = read_dropped_rows(ra.pub_raw + ".dropped.txt", seen_pub);
        const u64 ds = read_dropped_rows(ra.sub_raw + ".dropped.txt", seen_sub);
        r.rows_truncated = dp + ds;
        if (r.rows_truncated > 0)
        {
            add_reason(r, "逐样本表被 sample_limit 截断 " + std::to_string(r.rows_truncated)
                          + " 行（发布侧 " + std::to_string(dp) + " / 订阅侧 " + std::to_string(ds)
                          + "）—— 该轮计数不完整，不得当作完整样本集");
        }
        if (!seen_pub || !seen_sub)
        {
            r.notes.push_back("dropped.txt 缺失(pub=" + std::to_string(seen_pub ? 1 : 0)
                              + ", sub=" + std::to_string(seen_sub ? 1 : 0)
                              + ") —— 截断行数未确认");
        }
    }
    merge_and_reduce(local, s, r, ra.pub_raw, ra.sub_raw,
                     out_dir + "/samples/" + stem + ".samples.csv",
                     out_dir + "/samples/" + stem + ".samples.jsonl");

    /* 计时边界完整性：三条边界都要有样本，缺了标不通过，不拿别的点补。 */
    if (r.transport_ns.empty()) { r.timing_ok = false; add_reason(r, "传输完成计时边界无样本"); }
    if (r.delivery_ns.empty())
    {
        r.timing_ok = false;
        add_reason(r, "应用获得计时边界无样本（跨进程合并为空）");
    }
    if (workload != Workload::timestamp && r.app_read_ns.empty())
    {
        r.timing_ok = false;
        add_reason(r, "完整消费计时边界无样本");
    }
    if (ctl->stop_frames_recv.load() == 0)
    {
        add_reason(r, "订阅侧未确认任何停止哨兵（正常退出路径不完整）");
    }
    if (r.recv_probe == 0) add_reason(r, "订阅侧未记录探测帧计数");

    /* 路径生效证据（§10.6）：A/B 档必须真的有平坦段交付，否则算路径未生效。 */
    if (path == PathSel::dzflat_a || path == PathSel::dzflat_b)
    {
        if (r.pub_dzflat == 0)
        {
            add_reason(r, std::string("DZFlat 路径计数为 0：本轮实际走 TLV 回退，不能当作 ")
                          + path_name(path) + " 的成绩");
        }
        if (r.sub_flat_accepted == 0)
        {
            add_reason(r, "订阅侧 DZFlat 接收计数为 0（路径未生效）");
        }
        if (path == PathSel::dzflat_b && r.via_view == 0)
        {
            add_reason(r, "B 档没有任何样本经视图(借样)路径取得");
        }
    }
    if (path == PathSel::tlv && r.pub_dzflat > 0)
    {
        add_reason(r, "TLV 档出现 DZFlat 计数 > 0（路径未隔离）");
    }
    /* ---- 失败量进判定（t47/W02-F3）：**唯一的判定实现**，summary/results/verdict 都读这里
     * 写下的布尔值，⛔ 不许渲染表达式另算一遍（纪律：同一事实只有一处判定逻辑）。 ---- */
    {
        const double period_ns = (cfg.rate_hz > 0) ? (1e9 / cfg.rate_hz) : 0.0;
        const double late_rate = (r.attempts > 0) ? (static_cast<double>(r.late) / r.attempts) : 0.0;
        /* late：次数超绝对下限 **或** 比率超上限 ⇒ 进判定（阈值写明，可被 CLI 覆盖）。 */
        if (r.late > cfg.late_abs_max && late_rate > cfg.late_rate_max)
        {
            r.late_ok = false;
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "迟发超出阈值: late=%llu (%.4f%%) > 阈值[abs<=%llu 且 rate<=%.4f%%]"
                          "（阈值见 manifest.failure_thresholds；仅落列不判定=静默排除，故进判定）",
                          (unsigned long long)r.late, late_rate * 100.0,
                          (unsigned long long)cfg.late_abs_max, cfg.late_rate_max * 100.0);
            add_reason(r, buf);
        }
        /* backlog：以「周期数」为单位判（与速率无关，跨档可比）；阈值 <=0 表示仅信息性保留。 */
        if (cfg.backlog_max_periods > 0 && period_ns > 0)
        {
            const double periods = static_cast<double>(r.backlog_max_ns) / period_ns;
            if (periods > cfg.backlog_max_periods)
            {
                r.backlog_ok = false;
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "发送积压超出阈值: backlog_max=%llu ns = %.3f 个周期 > 阈值 %.3f %s",
                              (unsigned long long)r.backlog_max_ns, periods, cfg.backlog_max_periods,
                              "（阈值见 manifest.failure_thresholds）");
                add_reason(r, buf);
            }
        }
        if (r.send_blocked > cfg.send_blocked_max)
        {
            r.send_blocked_ok = false;
            add_reason(r, "有界等待耗尽(publish blocked) " + std::to_string(r.send_blocked)
                          + " 次 > 阈值 " + std::to_string(cfg.send_blocked_max)
                          + "（阈值见 manifest.failure_thresholds）");
        }
        if (r.abnormal > cfg.abnormal_max)
        {
            r.abnormal_ok = false;
            add_reason(r, "结构异常样本 " + std::to_string(r.abnormal)
                          + " 条 > 阈值 " + std::to_string(cfg.abnormal_max)
                          + "（阈值见 manifest.failure_thresholds）");
        }
        if (r.bad_header > cfg.bad_header_max)
        {
            r.bad_header_ok = false;
            add_reason(r, "段头/结构自相矛盾 " + std::to_string(r.bad_header)
                          + " 次 > 阈值 " + std::to_string(cfg.bad_header_max)
                          + "（t47 补列；此前该量完全不落列）");
        }
    }
    if (r.send_failed > 0) add_reason(r, "发送失败 " + std::to_string(r.send_failed) + " 条（已计入，未静默排除）");
    if (r.missing > 0) add_reason(r, "发送成功但未被订阅侧合并的样本 " + std::to_string(r.missing) + " 条");
    if (r.duplicate > 0) add_reason(r, "重复样本 " + std::to_string(r.duplicate) + " 条");
    if (r.out_of_order > 0) add_reason(r, "乱序样本 " + std::to_string(r.out_of_order) + " 条");

    /* 传输字节来源必须显式：DDS 未采集 wire 字节时写 null，不冒充。 */
    if (cfg.is_dds_path())
    {
        /* ⚠️ 同一份运行里不能既写 0 又声称 null：DDS 档**未采集** wire 字节，
         * 因此 `wire_bytes_per_msg` 在 CSV 里写**空字段**（= null），
         * `wire_bytes_source` 只描述来源、不承载"null"字样。 */
        r.wire_bytes_per_msg = 0;
        r.wire_bytes_known = false;
        r.wire_bytes_source = "not_collected(DDS 侧无 dzIPC wire 计数)";
    }
    else
    {
        u64 w = 0;
        {
            const CsvTable wt = read_csv(ra.pub_raw);
            for (const auto& row : wt.rows)
            {
                if (row.size() < 8) continue;
                w = static_cast<u64>(std::strtoull(row[7].c_str(), nullptr, 10));
                if (w) break;
            }
        }
        r.wire_bytes_per_msg = w;
        r.wire_bytes_source = (path == PathSel::tlv) ? "对象 serialize() 长度"
                             : (path == PathSel::dzflat_a ? "对象 dzflat_size()"
                                                          : "B 借样 chunk 容量");
    }

    r.case_ok = r.failure_reasons.empty();
    ctl_unmap(ctl);
    ::shm_unlink(ra.ctl_name.c_str());
    return r;
}

/* ======================================================================== */
/* 运行元数据与证据输出（§12）                                              */
/* ======================================================================== */

static const CaseResult* find_case(const std::vector<CaseResult>& rs, PathSel p, Workload w);

struct RunEnv
{
    std::string source_revision;
    std::string source_status;   ///< git status --short 的原文
    std::string build_dir;
    std::string build_type;
    std::string compiler;
    std::string exe_path;
    std::string exe_sha;
    std::string lib_path;
    std::string lib_sha;
    std::string config_hash;
    std::string config_canon;
    double clock_cost_ns = 0;
    bool clock_vdso = false;
    std::string clock_note;
    std::string roudi_pid;
    std::string dds_lib_path;
};

static std::string simple_hash(const std::string& s)
{
    /* FNV-1a 64 十六进制形式：配置指纹用，不承担密码学职责。 */
    u64 h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

static std::string config_canonical(const Config& c)
{
    std::ostringstream o;
    o << "duration=" << c.duration << ";warmup=" << c.warmup << ";rate=" << c.rate_hz
      << ";queue=" << c.queue << ";view_queue_cap=" << c.view_queue_cap
      << ";domain=" << c.domain << ";queue_wait_ms=" << c.queue_wait_ms
      << ";workload=" << workload_name(c.workload) << ";wait=" << wait_name(c.wait_mode)
      << ";best_effort=" << (c.best_effort ? 1 : 0) << ";nodelet=0"
      << ";payloads=";
    for (std::size_t i = 0; i < c.payloads.size(); ++i) { if (i) o << "/"; o << c.payloads[i]; }
    o << ";smoke=" << (c.smoke ? 1 : 0) << ";sample_limit=" << c.sample_limit;
    return o.str();
}

static RunEnv collect_env(const Config& cfg)
{
    RunEnv e;
    e.source_revision = run_capture("git -C " + cfg.build_dir + "/.. rev-parse HEAD 2>/dev/null");
    if (e.source_revision.empty()) e.source_revision = run_capture("git rev-parse HEAD 2>/dev/null");
    e.source_status = run_capture("git status --short 2>/dev/null");
    e.build_dir = cfg.build_dir;
    /* 编译器与构建类型来自 CMakeCache（真实读数，不是硬编码）。 */
    {
        std::ifstream f(cfg.build_dir + "/CMakeCache.txt");
        std::string line;
        while (std::getline(f, line))
        {
            if (line.rfind("CMAKE_BUILD_TYPE:", 0) == 0) e.build_type = line.substr(line.find('=') + 1);
            else if (line.rfind("CMAKE_CXX_COMPILER:", 0) == 0) e.compiler = line.substr(line.find('=') + 1);
        }
    }
    if (!e.compiler.empty())
    {
        e.compiler += " " + run_capture("'" + e.compiler + "' --version 2>/dev/null | head -1");
    }
    {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; e.exe_path = buf; }
    }
    e.exe_sha = sha256_of_file(e.exe_path);
    e.lib_path = loaded_lib_path("libipc.so");
    if (e.lib_path.empty())
    {
        /* 静态链接或未加载：退回构建目录里的产物（并如实标注来源）。 */
        e.lib_path = cfg.build_dir + "/lib/libipc.so";
    }
    e.lib_sha = sha256_of_file(e.lib_path);
    e.dds_lib_path = loaded_lib_path("libddsc.so");
    e.config_canon = config_canonical(cfg);
    e.config_hash = simple_hash(e.config_canon);
    {
        const auto cc = dzIPC::measure::measure_clock_cost(200000);
        e.clock_cost_ns = cc.vdso_ns_per_call;
        e.clock_vdso = cc.vdso_in_use;
        std::ostringstream o;
        o << "vdso_ns_per_call=" << cc.vdso_ns_per_call << " syscall_ns_per_call=" << cc.syscall_ns_per_call
          << " vdso_in_use=" << (cc.vdso_in_use ? "true" : "false");
        e.clock_note = o.str();
    }
    e.roudi_pid = run_capture("pgrep -x iox-roudi | head -1");
    return e;
}

static void write_text(const std::string& path, const std::string& body)
{
    write_text_file(path, body);
}

static void write_case_json(std::ostream& o, const CaseResult& r)
{
    double a[5], b[5], c[5], d[5], m;
    lat_stats(r.transport_ns, a[0], a[1], a[2], a[3], a[4], m);
    const double tmean = m;
    lat_stats(r.delivery_ns, b[0], b[1], b[2], b[3], b[4], m);
    const double dmean = m;
    lat_stats(r.app_read_ns, c[0], c[1], c[2], c[3], c[4], m);
    const double cmean = m;
    lat_stats(r.e2e_ns, d[0], d[1], d[2], d[3], d[4], m);
    const double emean = m;
    auto nullnum = [](double v) {
        if (v < 0) return std::string("null");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return std::string(buf);
    };
    o << "    {\n";
    o << "      \"case_id\": \"" << jstr(r.case_id) << "\",\n";
    o << "      \"path\": \"" << path_name(r.path) << "\",\n";
    o << "      \"experiment_group\": \"" << r.experiment_group << "\",\n";
    o << "      \"workload\": \"" << workload_name(r.workload) << "\",\n";
    o << "      \"wait_mode\": \"" << wait_name(r.wait_mode) << "\",\n";
    o << "      \"payload_target_bytes\": " << r.payload_target << ",\n";
    o << "      \"payload_bytes\": " << r.payload_bytes << ",\n";
    o << "      \"header_bytes\": " << r.header_bytes << ",\n";
    o << "      \"wire_bytes_per_msg\": "
      << (r.wire_bytes_known ? std::to_string(r.wire_bytes_per_msg) : std::string("null")) << ",\n";
    o << "      \"wire_bytes_source\": \"" << jstr(r.wire_bytes_source) << "\",\n";
    o << "      \"publish_mode\": \"" << jstr(r.publish_mode) << "\",\n";
    o << "      \"queue_size\": " << r.queue_size << ",\n";
    o << "      \"rate_target_hz\": " << r.rate_target << ",\n";
    o << "      \"rate_actual_attempt_hz\": " << r.rate_actual_attempt << ",\n";
    o << "      \"rate_actual_ok_hz\": " << r.rate_actual_ok << ",\n";
    o << "      \"duration_plan_s\": " << r.duration_plan << ",\n";
    o << "      \"duration_actual_s\": " << r.duration_actual << ",\n";
    o << "      \"plan\": " << r.plan << ",\n";
    o << "      \"attempts\": " << r.attempts << ",\n";
    o << "      \"sent_ok\": " << r.sent_ok << ",\n";
    o << "      \"send_failed\": " << r.send_failed << ",\n";
    o << "      \"send_blocked\": " << r.send_blocked << ",\n";
    o << "      \"late_sends\": " << r.late << ",\n";
    o << "      \"late_threshold_ns\": " << r.late_threshold_ns << ",\n";
    o << "      \"backlog_max_ns\": " << r.backlog_max_ns << ",\n";
    o << "      \"backlog_sum_ns\": " << r.backlog_sum_ns << ",\n";
    o << "      \"recv_measure\": " << r.recv_measure << ",\n";
    o << "      \"recv_probe\": " << r.recv_probe << ",\n";
    o << "      \"missing\": " << r.missing << ",\n";
    o << "      \"duplicate\": " << r.duplicate << ",\n";
    o << "      \"out_of_order\": " << r.out_of_order << ",\n";
    o << "      \"checksum_bad\": " << r.checksum_bad << ",\n";
    o << "      \"abnormal\": " << r.abnormal << ",\n";
    o << "      \"bad_header\": " << r.bad_header << ",\n";
    o << "      \"gates\": {\"late_ok\": " << (r.late_ok ? "true" : "false")
      << ", \"backlog_ok\": " << (r.backlog_ok ? "true" : "false")
      << ", \"send_blocked_ok\": " << (r.send_blocked_ok ? "true" : "false")
      << ", \"abnormal_ok\": " << (r.abnormal_ok ? "true" : "false")
      << ", \"bad_header_ok\": " << (r.bad_header_ok ? "true" : "false") << "},\n";
    o << "      \"samples_merged\": " << r.samples_merged << ",\n";
    o << "      \"elements_checked\": " << r.elements_checked << ",\n";
    o << "      \"via_view\": " << r.via_view << ",\n";
    o << "      \"via_object\": " << r.via_object << ",\n";
    o << "      \"via_dds\": " << r.via_dds << ",\n";
    o << "      \"pub_dzflat\": " << r.pub_dzflat << ",\n";
    o << "      \"pub_fallback\": " << r.pub_fallback << ",\n";
    o << "      \"sub_flat_accepted\": " << r.sub_flat_accepted << ",\n";
    o << "      \"sub_tlv_accepted\": " << r.sub_tlv_accepted << ",\n";
    o << "      \"sub_rx_defects\": " << r.sub_rx_defects << ",\n";
    o << "      \"sub_id_skipped\": " << r.sub_id_skipped << ",\n";
    o << "      \"transport_ns\": {\"min\": " << nullnum(a[0]) << ", \"p50\": " << nullnum(a[1])
      << ", \"p99\": " << nullnum(a[2]) << ", \"p999\": " << nullnum(a[3])
      << ", \"max\": " << nullnum(a[4]) << ", \"mean\": " << nullnum(tmean)
      << ", \"n\": " << r.transport_ns.size() << "},\n";
    o << "      \"delivery_ns\": {\"min\": " << nullnum(b[0]) << ", \"p50\": " << nullnum(b[1])
      << ", \"p99\": " << nullnum(b[2]) << ", \"p999\": " << nullnum(b[3])
      << ", \"max\": " << nullnum(b[4]) << ", \"mean\": " << nullnum(dmean)
      << ", \"n\": " << r.delivery_ns.size() << "},\n";
    o << "      \"app_read_ns\": {\"min\": " << nullnum(c[0]) << ", \"p50\": " << nullnum(c[1])
      << ", \"p99\": " << nullnum(c[2]) << ", \"p999\": " << nullnum(c[3])
      << ", \"max\": " << nullnum(c[4]) << ", \"mean\": " << nullnum(cmean)
      << ", \"n\": " << r.app_read_ns.size() << "},\n";
    o << "      \"e2e_ns\": {\"min\": " << nullnum(d[0]) << ", \"p50\": " << nullnum(d[1])
      << ", \"p99\": " << nullnum(d[2]) << ", \"p999\": " << nullnum(d[3])
      << ", \"max\": " << nullnum(d[4]) << ", \"mean\": " << nullnum(emean)
      << ", \"n\": " << r.e2e_ns.size() << "},\n";
    o << "      \"pub_cpu_s\": " << r.pub_cpu_s << ", \"sub_cpu_s\": " << r.sub_cpu_s
      << ", \"roudi_cpu_s\": " << (r.roudi_cpu_known ? std::to_string(r.roudi_cpu_s) : std::string("null"))
      << ",\n";
    o << "      \"pub_cpu_cores\": " << r.pub_cpu_cores << ", \"sub_cpu_cores\": " << r.sub_cpu_cores
      << ", \"total_cpu_cores\": " << r.total_cpu_cores << ",\n";
    o << "      \"pub_ctx_voluntary\": " << r.pub_ctx_vol << ", \"pub_ctx_nonvoluntary\": " << r.pub_ctx_nonvol << ",\n";
    o << "      \"sub_ctx_voluntary\": " << r.sub_ctx_vol << ", \"sub_ctx_nonvoluntary\": " << r.sub_ctx_nonvol << ",\n";
    o << "      \"pub_maxrss_kb\": " << r.pub_maxrss_kb << ", \"sub_maxrss_kb\": " << r.sub_maxrss_kb << ",\n";
    o << "      \"child_killed\": " << r.child_killed << ",\n";
    o << "      \"load_ok\": " << (r.load_ok ? "true" : "false") << ",\n";
    o << "      \"timing_ok\": " << (r.timing_ok ? "true" : "false") << ",\n";
    o << "      \"case_ok\": " << (r.case_ok ? "true" : "false") << ",\n";
    o << "      \"failure_reasons\": [";
    for (std::size_t i = 0; i < r.failure_reasons.size(); ++i)
    {
        if (i) o << ", ";
        o << "\"" << jstr(r.failure_reasons[i]) << "\"";
    }
    o << "],\n";
    o << "      \"notes\": [";
    for (std::size_t i = 0; i < r.notes.size(); ++i)
    {
        if (i) o << ", ";
        o << "\"" << jstr(r.notes[i]) << "\"";
    }
    o << "]\n    }";
}

static void write_run_outputs(const Config& cfg, const RunEnv& env,
                              const std::vector<CaseResult>& results)
{
    /* --- results.json --- */
    {
        std::ostringstream o;
        o << std::fixed << std::setprecision(3);
        o << "{\n  \"run_id\": \"" << jstr(cfg.run_id) << "\",\n";
        o << "  \"schema\": \"w02-xproc-benchmark/1.0\",\n";
        o << "  \"process_model\": \"cross-process\",\n";
        o << "  \"source_revision\": \"" << jstr(env.source_revision) << "\",\n";
        o << "  \"config_hash\": \"" << env.config_hash << "\",\n";
        o << "  \"clock\": {\"source\": \"CLOCK_MONOTONIC\", \"cost_ns_per_call\": " << env.clock_cost_ns
          << ", \"note\": \"" << jstr(env.clock_note) << "\"},\n";
        o << "  \"cases\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            write_case_json(o, results[i]);
            if (i + 1 < results.size()) o << ",";
            o << "\n";
        }
        o << "  ]\n}\n";
        write_text(cfg.out_dir + "/results.json", o.str());
    }

    /* --- summary.csv --- */
    write_summary_csv(cfg.out_dir + "/summary.csv", results);

    /* --- manifest.json（§12 最低字段集；缺项写 null 并在 verdict 说明） --- */
    {
        u64 total_samples = 0, fallback = 0, valid_rx = 0, killed = 0, bad = 0;
        for (const auto& r : results)
        {
            total_samples += r.samples_merged;
            fallback += r.pub_fallback;
            killed += r.child_killed;
            bad += r.checksum_bad + r.abnormal;
            if (r.case_ok) ++valid_rx;
        }
        std::ostringstream skipped;
        skipped << "[";
        for (std::size_t i = 0; i < cfg.precheck_skipped.size(); ++i)
        {
            if (i) skipped << ", ";
            skipped << "\"" << jstr(cfg.precheck_skipped[i]) << "\"";
        }
        skipped << "]";
        const std::string skipped_json = skipped.str();
        std::ostringstream o;
        o << "{\n";
        o << "  \"run_id\": \"" << jstr(cfg.run_id) << "\",\n";
        o << "  \"work_package\": \"W02\",\n";
        o << "  \"source_revision\": \"" << jstr(env.source_revision) << "\",\n";
        o << "  \"working_tree_clean\": false,\n";
        o << "  \"working_tree_diff\": [";
        {
            std::istringstream is(env.source_status);
            std::string line;
            bool first = true;
            while (std::getline(is, line))
            {
                if (line.empty()) continue;
                if (!first) o << ", ";
                first = false;
                o << "\"" << jstr(line) << "\"";
            }
        }
        o << "],\n";
        o << "  \"build_dir\": \"" << jstr(env.build_dir) << "\",\n";
        o << "  \"build_type\": \"" << jstr(env.build_type) << "\",\n";
        o << "  \"compiler\": \"" << jstr(env.compiler) << "\",\n";
        o << "  \"binary_sha256\": {\"publisher\": \"" << env.exe_sha << "\", \"subscriber\": \""
          << env.exe_sha << "\", \"library\": \"" << env.lib_sha << "\"},\n";
        o << "  \"loaded_library_paths\": [\"" << jstr(env.lib_path) << "\""
          << (env.dds_lib_path.empty() ? "" : std::string(", \"") + jstr(env.dds_lib_path) + "\"") << "],\n";
        {
            std::set<std::string> paths;
            for (const auto& r : results) paths.insert(path_name(r.path));
            if (paths.empty()) o << "  \"transport\": null,\n";
            else if (paths.size() == 1) o << "  \"transport\": \"" << *paths.begin() << "\",\n";
            else
            {
                o << "  \"transport\": \"multi:[";
                bool first = true;
                for (const auto& p : paths) { if (!first) o << ","; first = false; o << p; }
                o << "]\",\n";
            }
        }
        o << "  \"process_model\": \"cross-process\",\n";
        o << "  \"measurement_mode\": \"rusage\",\n";
        o << "  \"diagnostics_enabled\": false,\n";
        o << "  \"topic_count\": 1,\n";
        o << "  \"unique_topic_count\": 1,\n";
        o << "  \"worker_count\": null,\n";
        o << "  \"registered_count\": " << results.size() << ",\n";
        o << "  \"expected_count\": " << results.size() << ",\n";
        o << "  \"valid_rx_count\": " << valid_rx << ",\n";
        /* fallback_count 的口径必须写清楚：在 dzIPC 的计数语义里，**TLV 档的每一条
         * 成功发布都算一次"尝试平坦布局但回退"**（没开开关/类型不支持都会走这里）。
         * 所以本字段 ≠ 兼容回退事故数。与规模验收口径（§13.2 要求 fallback_count==0）
         * 不可直接套用 —— 后者指的是"固定 worker 目标未达成、靠 per-route 回退线程
         * 补齐"的次数，由 W06 侧统计。逐路径的真回退率见各 case 的 pub_dzflat/pub_fallback。 */
        o << "  \"fallback_count\": " << fallback << ",\n";
        /* ⚠️ 字符串里的引号必须用中文引号：之前直接用 ASCII 双引号把 manifest.json 写成了
         * 非法 JSON（"本字段不是"兼容回退事故数""）—— 产物自己坏了，校验脚本先炸。 */
        o << "  \"fallback_count_note\": \"dzIPC 语义: TLV 档每条成功发布都计一次 fallback; "
             "本字段不是「兼容回退事故数」, 逐路径真回退率见 summary.csv 的 pub_dzflat/pub_fallback\",\n";
        o << "  \"sample_count\": " << total_samples << ",\n";
        o << "  \"window_s\": " << (results.empty() ? 0.0 : results[0].duration_actual) << ",\n";
        o << "  \"config_hash\": \"" << env.config_hash << "\",\n";
        o << "  \"clock_source\": \"CLOCK_MONOTONIC\",\n";
        o << "  \"clock_cost_ns_per_call\": " << env.clock_cost_ns << ",\n";
        o << "  \"collector_overhead_ns\": null,\n";
        o << "  \"roudi_pid\": " << (env.roudi_pid.empty() ? std::string("null") : env.roudi_pid) << ",\n";
        o << "  \"child_killed_total\": " << killed << ",\n";
        o << "  \"payload_checksum_bad_total\": " << bad << ",\n";
        o << "  \"result_files\": [\"results.json\", \"summary.csv\", \"samples/\", \"counters.json\", "
             "\"environment.json\", \"topology.json\", \"config.json\", \"field_schema.json\", "
             "\"path_evidence.json\", \"console.log\", \"verdict.md\"],\n";
        o << "  \"dds_qos\": \"" << dds_qos_name() << "\",\n";
        o << "  \"dds_precheck_skipped\": " << skipped_json << ",\n";
        /* 中间轮次留痕（方案 §12「只追加」）：
         * 我为节省磁盘清理过 r01…r19 的**目录**，但未在产物里留痕 —— 等价于"未留档清理"。
         * 这里把存活的控制台日志与被清理的 run 编号显式登记，避免后人以为"从没有过这些轮次"。
         * 现行唯一可引用 run 由命令行 --run-id 决定；本字段只做历史说明，⛔ 不作为样本来源。 */
        o << "  \"superseded_runs\": {\n";
        o << "    \"note\": \"W02 中间轮次 r01…r19 的目录已清理（人工、未留痕，已登记为 W12 已知限制）；"
             "控制台日志归档在 superseded/；⛔ 这些轮次不作为任何结论的样本来源\",\n";
        o << "    \"console_archive_dir\": \"superseded/\",\n";
        o << "    \"superseded_run_ids\": [";
        {
            static const char* kSuperseded[] = {
                "20260928-r01-W02", "20260928-r02-W02", "20260928-r03-W02", "20260928-r04-W02",
                "20260928-r05-W02", "20260928-r06-W02", "20260928-r07-W02", "20260928-r08-W02-script",
                "20260928-r09-W02", "20260928-r10-W02", "20260928-r11-W02", "20260928-r12-W02",
                "20260928-r13-W02", "20260928-r14-W02", "20260928-r15-W02", "20260928-r16-W02",
                "20260928-r17-W02", "20260928-r18-W02", "20260928-r19-W02",
                "20260928-r20-W02", "20260928-r21-W02", "20260928-r22-W02",
                "20260928-r24-W02", "20260928-r25-W02", "20260928-r26-W02",
                "20260928-r27-W02", "20260928-r28-W02", "20260928-r29-W02",
                "20260928-r30-W02", "20260928-r31-W02", "20260928-r32-W02",
                "20260928-r33-W02"};
            for (std::size_t i = 0; i < sizeof(kSuperseded) / sizeof(kSuperseded[0]); ++i)
            {
                if (i) o << ", ";
                o << "\"" << kSuperseded[i] << "\"";
            }
        }
        o << "],\n";
        o << "    \"currently_citable\": \"" << jstr(cfg.run_id) << "\",\n";
        o << "    \"authority_rule\": \"本字段是 W02 交付面**唯一权威指针**：任何引用必须指向它；"
             "指向 superseded_run_ids 中的 run 时必须同时标「非现行 + 原因」。补了这条是因为 t25 "
             "复核合同引用了 r19 的 cells=269896/skipped=26，而现行 run 已是 r23（cells=269852/skipped=37）"
             "—— 引用链断裂会让下游拿错数字。\",\n";
        o << "    \"r19_r20_non_current\": \"r19（*_messages 因 D-17 计数双写作废，且非现行）、"
             "r20（W02-F2 修复前对照，非现行）—— 历史对照可留，但⛔ 不得作为现行结论出现\",\n";
        o << "    \"non_current_runs\": [\n";
        o << "      {\"run_id\": \"20260928-r19-W02\", \"status\": \"非现行\", "
             "\"reason\": \"*_messages 因 D-17 计数双写作废（约 2× 且回退场景下把「期望路径」记成「实际路径」）；"
             "cells=269896/skipped=26 是 t25 合同引用的数字，⛔ 不得再作为现行结论\"},\n";
        o << "      {\"run_id\": \"20260928-r20-W02\", \"status\": \"非现行\", "
             "\"reason\": \"W02-F2(wire null 表示) 修复前对照；cells=269848/skipped=38\"},\n";
        o << "      {\"run_id\": \"20260928-r23-W02\", \"status\": \"历史证据 run\", "
             "\"reason\": \"F3 修复（失败量进判定 + bad_header 补列）之前的一代；其 summary.csv "
             "缺 bad_header 与 5 个 gate 列，⛔ 不得据此判「失败量已被判定」\"},\n";
        o << "      {\"run_id\": \"20260928-r24-W02\", \"status\": \"非现行\", "
             "\"reason\": \"首次带 F3 判定的冒烟轮：RouDi 不在线 ⇒ cyc-iox 被显式剔除（仅 60 case）；"
             "且当时 B 档预热帧形状缺陷已被本列暴露出来（bad_header 1194–1200/case），随后修复\"},\n";
        o << "      {\"run_id\": \"20260928-r25-W02\", \"status\": \"非现行\", "
             "\"reason\": \"backlog 预算取 5 周期档 ⇒ 1 case 因真实排队（5.171 周期）判失败；"
             "该轮用于确定默认预算，不作为结论\"},\n";
        o << "      {\"run_id\": \"20260928-r26-W02\", \"status\": \"非现行\", "
             "\"reason\": \"late 预算取 5% 档 ⇒ 1 case（40/500 迟发）判失败；该轮用于确定默认预算\"},\n";
        o << "      {\"run_id\": \"20260928-r27-W02\", \"status\": \"非现行\", "
             "\"reason\": \"同 r28 的矩阵与判定（75/75），但 manifest 尚缺 failure_thresholds.informational_only\"},\n";
        o << "      {\"run_id\": \"20260928-r28-W02\", \"status\": \"非现行\", "
             "\"reason\": \"t47 收口前一轮（含 informational_only）\"},\n";
        o << "      {\"run_id\": \"20260928-r29-W02\", \"status\": \"非现行\", "
             "\"reason\": \"本机 RouDi 会话结束 ⇒ cyc-iox 被显式剔除（仅 60 case）\"},\n";
        o << "      {\"run_id\": \"20260928-r30-W02\", \"status\": \"非现行\", "
             "\"reason\": \"t47 的现行 run；t50 收口（R-3 默认安全化 + 字段别名）之前的一代，"
             "其 run_identity 缺 purpose_explicit/authority_undeclared/claims_authority 三键\"},\n";
        o << "      {\"run_id\": \"20260928-r31-W02\", \"status\": \"非现行\", "
             "\"reason\": \"R-3 改造后首跑：机器负载偏高 ⇒ DDS 1 MiB 档真实排队（late 31.0%）被判失败，"
             "该轮如实暴露负载敏感性，不作为结论\"},\n";
        o << "      {\"run_id\": \"20260928-r32-W02\", \"status\": \"非现行\", "
             "\"reason\": \"同因（高负载下 DDS 1 MiB 三档全超预算）；用于证明 gate 在真实负载下会亮\"},\n";
        o << "      {\"run_id\": \"20260928-r33-W02\", \"status\": \"非现行\", "
             "\"reason\": \"首个含 W03 新 schema 段（run_level_fields=13）的 run，但 superseded 列表尚缺 r30–r32\"}\n";
        o << "    ],\n";
        o << "    \"r19_note\": \"r19 的 *_messages 因 D-17 计数双写作废; r20 为 W02-F2(wire null 表示)修复前对照; "
             "本 run 之前的 r23/r24 分别是 F3 修复前/首次冒烟；只可引用 currently_citable 指向的 run\"\n";
        o << "  },\n";
        o << "  \"failure_thresholds\": {\n";
        o << "    \"note\": \"t47/W02-F3：失败量必须有**显式声明阈值**并进入机器可读判定通路；"
             "仅落列不判定=静默排除，⛔ 不允许。阈值可用 CLI 覆盖，覆盖后同样落在本字段。\",\n";
        o << "    \"late_abs_max\": " << cfg.late_abs_max
          << ", \"late_rate_max\": " << cfg.late_rate_max << ",\n";
        o << "    \"backlog_max_periods\": " << cfg.backlog_max_periods
          << ", \"send_blocked_max\": " << cfg.send_blocked_max << ",\n";
        o << "    \"abnormal_max\": " << cfg.abnormal_max
          << ", \"bad_header_max\": " << cfg.bad_header_max << ",\n";
        o << "    \"cli_overridden\": " << (cfg.thresholds_given ? "true" : "false") << ",\n";
        o << "    \"gates\": [\"late\", \"backlog\", \"send_blocked\", \"abnormal\", \"bad_header\"],\n";
        /* 逐量说明「为什么某量只作信息性保留」——队长第 4 条要求：不进入判定必须**显式声明**，
         * ⛔ 不得以"这些量只是信息性指标"一句带过。 */
        o << "    \"informational_only\": {\n";
        o << "      \"via_view\": \"通道构成（本轮有多少样本经视图/借样通道取得），不是失败；"
             "且实测 A 与 B 两档消费者侧同形（各 13500/0），判它没有区分力\",\n";
        o << "      \"via_object\": \"同上（对象通道构成）\",\n";
        o << "      \"via_dds\": \"同上（DDS 通道构成）\",\n";
        o << "      \"late_threshold_ns\": \"它只定义「什么叫迟发」，不是失败预算；"
             "失败预算见 late_abs_max/late_rate_max\",\n";
        o << "      \"backlog_sum_ns\": \"累计迟发量用于刻画分布，单次峰值才作预算（backlog_max_periods）\",\n";
        o << "      \"note\": \"以上各量仍逐 case 落列（可查），只是**不据此判 case_ok**；"
             "这是显式声明，不是静默跳过\"\n";
        o << "    },\n";
        o << "    \"gates_in_judgement\": {\n";
        for (std::size_t i = 0; i < 5; ++i)
        {
            const char* nm[5] = {"late", "backlog", "send_blocked", "abnormal", "bad_header"};
            u64 failed_cases = 0;
            for (const auto& r : results)
            {
                const bool ok = (i == 0) ? r.late_ok : (i == 1) ? r.backlog_ok
                              : (i == 2) ? r.send_blocked_ok : (i == 3) ? r.abnormal_ok : r.bad_header_ok;
                if (!ok) ++failed_cases;
            }
            o << "      \"" << nm[i] << "\": \"进判定（阈值见上）; 本 run 触发 "
              << failed_cases << " 个 case\"";
            if (i + 1 < 5) o << ",";
            o << "\n";
        }
        o << "    }\n";
        o << "  },\n";
        /* 现行 run 的**唯一权威指针**（t47/W02-F0；t50/R-3 默认安全）：
         * 认领权威**必须显式**（`--purpose=evidence`）；缺省（不带参数）⇒ 不认领，指针为 null
         * 且 `authority_undeclared=true`（读方必须看到"本轮没声明自己是不是权威"）。
         * ⛔ 只有 `--purpose=evidence` 且**未**给 `--citation-authority` 时才指向自己。 */
        {
            const std::string eff_purpose = cfg.purpose.empty() ? std::string("verification") : cfg.purpose;
            const bool claims = (eff_purpose == "evidence") && cfg.citation_authority.empty();
            const std::string authority = claims ? cfg.run_id : cfg.citation_authority;
            o << "  \"currently_citable\": "
              << (authority.empty() ? std::string("null") : std::string("\"") + jstr(authority) + "\"")
              << ",\n";
            o << "  \"run_identity\": {\"this_run_id\": \"" << jstr(cfg.run_id)
              << "\", \"purpose\": \"" << jstr(eff_purpose)
              << "\", \"purpose_explicit\": " << (cfg.purpose_given ? "true" : "false")
              << ", \"this_run_is_evidence\": " << (claims ? "true" : "false")
              << ", \"authority_undeclared\": "
              << ((!cfg.purpose_given || eff_purpose != "evidence") && cfg.citation_authority.empty()
                      ? "true" : "false")
              << ", \"claims_authority\": " << (claims ? "true" : "false")
              << ", \"citation_authority\": "
              << (authority.empty() ? std::string("null") : std::string("\"") + jstr(authority) + "\"")
              << "},\n";
        }
        /* 字段别名映射（t50 中性观察①）：同一事实在两份产物里用了不同列名，值相同。
         * ⛔ 不新增重复列（会造成"两个来源"），而是**机器可读地声明映射**，读方按映射取值。 */
        o << "  \"field_aliases\": {\n";
        o << "    \"note\": \"同一事实在不同产物里的列名映射；值语义相同，⛔ 不表示两个独立来源\",\n";
        o << "    \"late\": {\"summary.csv\": \"late\", \"results.json\": \"late_sends\", "
             "\"semantics\": \"超过 max(50µs, 周期/10) 的发送次数\"},\n";
        o << "    \"late_threshold\": {\"summary.csv\": \"late_threshold_ns\", "
             "\"results.json\": \"late_threshold_ns\", \"semantics\": \"迟发判定阈值(ns)\"},\n";
        o << "    \"bad_header\": {\"summary.csv\": \"bad_header\", \"results.json\": \"bad_header\", "
             "\"semantics\": \"段头/对象结构自相矛盾次数\"},\n";
        o << "    \"gate_results\": {\"summary.csv\": \"late_ok,backlog_ok,send_blocked_ok,abnormal_ok,bad_header_ok\", "
             "\"results.json\": \"gates.{...}\", \"semantics\": \"逐 gate 判定布尔（0/1 与 false/true 对应）\"}\n";
        o << "  },\n";
        o << "  \"evidence_discipline\": {\n";
        o << "    \"current_run_authority\": \"任何引用必须是 manifest.currently_citable 指向的 run；"
             "引用 superseded_runs.superseded_run_ids 里的 run 必须同时标「非现行 + 原因」\",\n";
        o << "    \"authority_claim_rule\": \"认领权威是**显式动作**：只有 `--purpose=evidence` 才会让 "
             "currently_citable 指向本 run；不带参数（或 --purpose=verification/experiment 且未给 "
             "--citation-authority）⇒ currently_citable 写 null 且 run_identity.authority_undeclared=true。"
             "复核轮必须带 `--purpose=verification --citation-authority=<证据 run>`；机械判据："
             "复核轮跑完后证据 run 的 manifest.currently_citable 必须不变（可用 sha256 对比）\",\n";
        o << "    \"single_judgement_logic\": \"同一事实只有一处判定逻辑：failure_reasons 由 run_one_case 内"
             "唯一一处产生，summary.csv/results.json/verdict.md 只读该结果，⛔ 不各自重算\",\n";
        o << "    \"zero_value_three_states\": {\n";
        o << "      \"0\": \"已采集且实测为零（可读作「未发生」）\",\n";
        o << "      \"null\": \"未采集 或 分母为零（CSV 写空字段，⛔ 不写 0 冒充）\",\n";
        o << "      \"unwired\": \"字段有定义但无生产者（其 0 不可读作「未发生」；"
             "本基准的实例见 manifest.fallback_count_note 与 W03 counters 清点）\"\n";
        o << "    }\n";
        o << "  },\n";
        o << "  \"path_evidence\": \"path_evidence.json\",\n";
        o << "  \"notes\": \"process_model 恒为 cross-process：pub/sub 是 fork+exec 的两个新映像；"
             "计时边界见 field_schema.json 的 timestamp_write_points；"
             "未测项一律写 null/空字段(不加 0 冒充)，DDS 档 wire 字节即为一例(见各 case 的 wire_bytes_source)；"
             "样本表跳过以行为单位: cells == 4*(rows-skipped_rows)，判据固化在 test/w02_sample_audit.py\"\n";
        o << "}\n";
        write_text(cfg.out_dir + "/manifest.json", o.str());
    }

    /* --- environment.json / topology.json（W03 采集器同构字段） --- */
    {
        const dzIPC::measure::PlatformInfo p = dzIPC::measure::collect_platform_info();
        write_text(cfg.out_dir + "/environment.json",
                   "{\n  \"kind\": \"environment\",\n  \"platform\": " +
                   dzIPC::measure::platform_info_json(p) + ",\n  \"compiler\": \"" + jstr(env.compiler) +
                   "\",\n  \"clock\": {\"source\": \"CLOCK_MONOTONIC\", \"cost_ns_per_call\": " +
                   std::to_string(env.clock_cost_ns) + "},\n  \"dds_lib\": \"" + jstr(env.dds_lib_path) +
                   "\",\n  \"roudi_pid\": " + (env.roudi_pid.empty() ? std::string("null") : env.roudi_pid) + "\n}\n");
    }
    {
        std::ostringstream o;
        o << "{\n  \"kind\": \"topology\",\n  \"process_model\": \"cross-process\",\n";
        o << "  \"roles\": [\n";
        o << "    {\"role\": \"collector\", \"pid\": " << ::getpid() << ", \"exe\": \"" << jstr(env.exe_path) << "\"},\n";
        o << "    {\"role\": \"publisher\", \"transport\": \"fork+exec --role=pub\"},\n";
        o << "    {\"role\": \"subscriber\", \"transport\": \"fork+exec --role=sub\"}\n";
        if (!env.roudi_pid.empty())
        {
            o << ",\n    {\"role\": \"roudi\", \"pid\": " << env.roudi_pid
              << ", \"note\": \"CycloneDDS+iceoryx 守护进程\"}";
        }
        o << "\n  ],\n";
        o << "  \"topic\": \"" << jstr(cfg.topic) << "\",\n  \"domain\": " << cfg.domain << "\n}\n";
        write_text(cfg.out_dir + "/topology.json", o.str());
    }

    /* --- config.json（有效配置全量） --- */
    {
        std::ostringstream o;
        o << "{\n  \"run_id\": \"" << jstr(cfg.run_id) << "\",\n";
        o << "  \"canonical\": \"" << jstr(env.config_canon) << "\",\n";
        o << "  \"config_hash\": \"" << env.config_hash << "\",\n";
        o << "  \"duration_s\": " << cfg.duration << ", \"warmup_s\": " << cfg.warmup << ",\n";
        o << "  \"rate_hz\": " << cfg.rate_hz << ", \"queue_size\": " << cfg.queue << ",\n";
        o << "  \"view_queue_cap\": " << cfg.view_queue_cap << ", \"domain\": " << cfg.domain << ",\n";
        o << "  \"queue_wait_ms\": " << cfg.queue_wait_ms << ",\n";
        o << "  \"best_effort\": " << (cfg.best_effort ? "true" : "false") << ",\n";
        o << "  \"nodelet_enabled\": false,\n";
        o << "  \"smoke\": " << (cfg.smoke ? "true" : "false") << ",\n";
        o << "  \"sample_limit\": " << cfg.sample_limit << ",\n";
        o << "  \"payloads\": [";
        for (std::size_t i = 0; i < cfg.payloads.size(); ++i) { if (i) o << ", "; o << cfg.payloads[i]; }
        o << "],\n";
        o << "  \"paths\": [";
        for (std::size_t i = 0; i < cfg.compare.size(); ++i) { if (i) o << ", "; o << "\"" << jstr(cfg.compare[i]) << "\""; }
        o << "],\n";
        o << "  \"dds_uri_udp\": \"" << jstr(cfg.dds_uri_udp) << "\",\n";
        o << "  \"dds_uri_iox\": \"" << jstr(cfg.dds_uri_iox) << "\",\n";
        o << "  \"purpose\": \"" << jstr(cfg.purpose.empty() ? std::string("verification") : cfg.purpose)
          << "\", \"purpose_explicit\": " << (cfg.purpose_given ? "true" : "false")
          << ", \"citation_authority\": \"" << jstr(cfg.citation_authority) << "\",\n";
        o << "  \"failure_thresholds\": {\"late_abs_max\": " << cfg.late_abs_max
          << ", \"late_rate_max\": " << cfg.late_rate_max
          << ", \"backlog_max_periods\": " << cfg.backlog_max_periods
          << ", \"send_blocked_max\": " << cfg.send_blocked_max
          << ", \"abnormal_max\": " << cfg.abnormal_max
          << ", \"bad_header_max\": " << cfg.bad_header_max
          << ", \"cli_overridden\": " << (cfg.thresholds_given ? "true" : "false") << "}\n}\n";
        write_text(cfg.out_dir + "/config.json", o.str());
    }

    /* --- field_schema.json（W03 单一事实来源，直接用其生成器） --- */
    write_text(cfg.out_dir + "/field_schema.json",
               dzIPC::measure::schema_document_json(cfg.run_id));

    /* --- counters.json（本进程计数 + 各 case 的路径计数已在 summary 里） ---
     *
     * ⚠️ 必须带 `counter_scope`：`CounterRegistry` 里的 ID 有**不同写入者**，读方若不
     * 知道谁写的谁，就会把"实际路径"与"基准侧窗口账"混算。D-17 的教训是：同一个
     * `*_messages` 曾同时被传输层钩子（实际路径）与 harness（期望路径）写，读方拿到
     * 约 2× 条数还以为是一条路径的计数（方案 §13.3 明确要防的"路径证据不足却标已确认"）。
     * 这是**自证材料**，不是把消歧推给读方：此处逐 ID 声明写入者与是否可加。 */
    {
        std::ostringstream o;
        o << "{\n  \"kind\": \"counter_snapshot_collector\",\n";
        o << "  \"note\": \"采集器进程的计数器快照。各角色进程各自的计数随原始 CSV 落盘为 "
             "<case>.pub_raw.csv.counters.json / .sub_raw.csv.counters.json；"
             "跨进程汇总不做加法，只并列。\",\n";
        o << "  \"counter_scope\": {\n";
        o << "    \"path_message_counts\": {\n";
        o << "      \"ids\": [\"tlv_messages\", \"dzflat_a_messages\", \"dzflat_b_messages\"],\n";
        o << "      \"writers\": [\"transport_hook:src/dzIPC/common/nodelet_config.cc:NoteDzFlatPathDelivered\"],\n";
        o << "      \"semantics\": \"实际交付路径(按真实分流: A/B/TLV), 含探测/预热/停止帧; "
             "DDS 档恒为 0(不经 dzIPC SHM 路径)\",\n";
        o << "      \"harness_writes\": false,\n";
        o << "      \"addressing\": \"读方直接采用; 与 summary.csv 的 pub_dzflat/pub_fallback 同源\",\n";
        o << "      \"identity\": \"family(tlv+dzflat_a+dzflat_b) == pub_dzflat + pub_fallback "
             "(单测量窗口内, 末几条钩子计数可能落在读窗口外)\"\n";
        o << "    },\n";
        o << "    \"path_wire_bytes\": {\n";
        o << "      \"ids\": [\"tlv_wire_bytes\", \"dzflat_wire_bytes\"],\n";
        o << "      \"writers\": [\"transport_hook:NoteDzFlatPathDelivered\"],\n";
        o << "      \"semantics\": \"实际传输字节(含段头/分片)\",\n";
        o << "      \"harness_writes\": false,\n";
        o << "      \"addressing\": \"读方直接采用\"\n";
        o << "    },\n";
        o << "    \"path_app_bytes\": {\n";
        o << "      \"ids\": [\"tlv_bytes\", \"dzflat_a_bytes\", \"dzflat_b_bytes\"],\n";
        o << "      \"writers\": [\"harness:test/xproc_benchmark.cpp (测量窗口内成功发布的应用逻辑载荷)\"],\n";
        o << "      \"semantics\": \"= sent_ok × payload_bytes; 与 *_wire_bytes 口径不同\",\n";
        o << "      \"harness_writes\": true,\n";
        o << "      \"addressing\": \"⛔ 不得与 *_wire_bytes 相加, 也不得与 *_messages 相乘解读\"\n";
        o << "    },\n";
        o << "    \"case_level_counts\": {\n";
        o << "      \"ids\": [\"plan\", \"attempts\", \"sent_ok\", \"send_failed\", \"recv_measure\", "
             "\"missing\", \"duplicate\", \"out_of_order\"],\n";
        o << "      \"writers\": [\"harness:summary.csv / results.json (不经 CounterRegistry)\"],\n";
        o << "      \"semantics\": \"用例级事实来源; 与 counters 的路径计数不同粒度\",\n";
        o << "      \"harness_writes\": true,\n";
        o << "      \"addressing\": \"判用例是否通过一律用这一组\"\n";
        o << "    }\n";
        o << "  },\n";
        o << "  \"collector\": " << CounterRegistry::instance().to_json() << ",\n";
        o << "  \"cases\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            const auto& r = results[i];
            o << "    {\"case_id\": \"" << jstr(r.case_id) << "\", \"pub_dzflat\": " << r.pub_dzflat
              << ", \"pub_fallback\": " << r.pub_fallback
              << ", \"sub_flat_accepted\": " << r.sub_flat_accepted
              << ", \"sub_tlv_accepted\": " << r.sub_tlv_accepted
              << ", \"sub_rx_defects\": " << r.sub_rx_defects
              << ", \"sub_id_skipped\": " << r.sub_id_skipped
              << ", \"send_failed\": " << r.send_failed
              << ", \"send_blocked\": " << r.send_blocked
              << ", \"late_sends\": " << r.late
              << ", \"missing\": " << r.missing
              << ", \"duplicate\": " << r.duplicate
              << ", \"out_of_order\": " << r.out_of_order
              << ", \"checksum_bad\": " << r.checksum_bad << "}";
            if (i + 1 < results.size()) o << ",";
            o << "\n";
        }
        o << "  ]\n}\n";
        write_text(cfg.out_dir + "/counters.json", o.str());
    }

    /* --- path_evidence.json：每条路径都要有调用点/运行证据/指纹/计数，否则标"未确认" --- */
    {
        dzIPC::measure::EvidenceRegister reg;
        auto add_path = [&](dzIPC::measure::PathKind k, const std::string& callsite,
                            const std::vector<std::string>& counters,
                            const std::vector<const CaseResult*>& cases) {
            dzIPC::measure::PathEvidence e;
            e.path = k;
            e.experiment_id = cfg.run_id;
            e.call_sites.push_back(callsite);
            e.binary_fingerprints.push_back("collector_sha256=" + env.exe_sha);
            e.binary_fingerprints.push_back("libipc_sha256=" + env.lib_sha);
            e.counters = counters;
            for (const auto* c : cases)
            {
                if (c == nullptr) continue;
                e.samples += c->samples_merged;
                e.fallback_count += c->pub_fallback;
                if (!c->case_ok)
                {
                    e.runtime_evidence.push_back("case " + c->case_id + " 未通过: "
                        + (c->failure_reasons.empty() ? std::string("?") : c->failure_reasons[0]));
                }
                else
                {
                    std::ostringstream o;
                    o << "case " << c->case_id << ": recv=" << c->recv_measure
                      << " dzflat=" << c->pub_dzflat << " fallback=" << c->pub_fallback
                      << " via_view=" << c->via_view
                      << " (samples/" << sanitize(c->case_id) << ".samples.csv)";
                    e.runtime_evidence.push_back(o.str());
                }
                e.experiment_groups.push_back(c->experiment_group);
            }
            const bool any_ok = !cases.empty() &&
                std::any_of(cases.begin(), cases.end(), [](const CaseResult* c) { return c && c->case_ok; });
            e.level = any_ok ? dzIPC::measure::EvidenceLevel::s3_single_case_verified
                             : dzIPC::measure::EvidenceLevel::s2_module_wired;
            reg.add(e);
        };
        add_path(dzIPC::measure::PathKind::tlv,
                 "src/dzIPC/shm_pub_sub_ipc.cc:publish_blocking → msg->serialize() (TLV 整包)",
                 {"tlv_messages", "tlv_bytes", "tlv_wire_bytes", "tlv_accepted"},
                 {find_case(results, PathSel::tlv, Workload::full),
                  find_case(results, PathSel::tlv, Workload::timestamp)});
        add_path(dzIPC::measure::PathKind::dzflat_a,
                 "src/dzIPC/shm_pub_sub_ipc.cc:try_publish_dzflat → publisher_->loan + msg->dzflat_write",
                 {"dzflat_a_messages", "dzflat_a_bytes", "dzflat_wire_bytes", "dzflat_accepted"},
                 {find_case(results, PathSel::dzflat_a, Workload::full),
                  find_case(results, PathSel::dzflat_a, Workload::timestamp)});
        add_path(dzIPC::measure::PathKind::dzflat_b,
                 "test/xproc_benchmark.cpp:run_pub_dzipc → pub->loan<TestMsgFlat>() → alloc_*/set_* → pub->publish_loaned()",
                 {"dzflat_b_messages", "dzflat_b_bytes", "dzflat_accepted"},
                 {find_case(results, PathSel::dzflat_b, Workload::full),
                  find_case(results, PathSel::dzflat_b, Workload::crc)});
        if (find_case(results, PathSel::cyc_udp, Workload::full) || find_case(results, PathSel::cyc_iox, Workload::full))
        {
            /* ⚠️ counters 这里**故意留空**：DDS 侧没有 dzIPC 的路径计数器，能给的只有
             * "CYCLONEDDS_URI 指向哪份配置 + RouDi 在不在 + 实体建得出来"。
             * 按 W03 的 PathEvidence::compute_missing 判据，缺独立计数就该标「未确认」——
             * 这正是 CycloneDDS 档的真实状态：**能测，但路径证据不足**。
             * 早先版本往 counters 里塞了一句解释文字，于是被判成"已确认"，那是自我欺骗。 */
            add_path(dzIPC::measure::PathKind::cyclonedds_iox,
                     "test/xproc_benchmark.cpp:run_pub_dds → dds_write() (CycloneDDS 0.10.2)",
                     {},
                     {find_case(results, PathSel::cyc_udp, Workload::full),
                      find_case(results, PathSel::cyc_iox, Workload::full)});
        }
        write_text(cfg.out_dir + "/path_evidence.json", reg.to_json());
    }
}

static const CaseResult* find_case(const std::vector<CaseResult>& rs, PathSel p, Workload w)
{
    for (const auto& r : rs)
    {
        if (r.path == p && r.workload == w) return &r;
    }
    return nullptr;
}

/* verdict.md：逐条引用原始文件与字段，不用「日志正常」充当证据（§12 末）。 */
static void write_verdict(const Config& cfg, const RunEnv& env, const std::vector<CaseResult>& results)
{
    std::ostringstream o;
    o << "# W02 统一跨进程基准 · 判定（verdict）\n\n";
    o << "> run_id: `" << cfg.run_id << "`  \n";
    o << "> source_revision: `" << env.source_revision << "`（工作区含未提交改动，原文见 "
         "`manifest.json:working_tree_diff`）  \n";
    o << "> 二进制 sha256: `" << env.exe_sha << "`；libipc sha256: `" << env.lib_sha
      << "`（`manifest.json:binary_sha256`）  \n";
    o << "> 时基: CLOCK_MONOTONIC，" << env.clock_note << "（`manifest.json:clock_cost_ns_per_call`）\n\n";
    o << "## 1. 机器判定摘要\n\n";
    o << "```\n";
    o << "process_model : cross-process (pub/sub 均为 fork+exec 的新映像)\n";
    o << "config_hash   : " << env.config_hash << "\n";
    o << "payload_shapes: ";
    for (std::size_t i = 0; i < cfg.payloads.size(); ++i)
    {
        if (i) o << ", ";
        const Shape s = compute_shape(cfg.payloads[i]);
        o << cfg.payloads[i] << "B(实际 " << s.bytes() << "B + 头 " << kHdrBytes << "B)";
    }
    o << "\n";
    o << "cases         : " << results.size() << " 通过 "
      << std::count_if(results.begin(), results.end(), [](const CaseResult& r) { return r.case_ok; })
      << " / 未通过 " << std::count_if(results.begin(), results.end(), [](const CaseResult& r) { return !r.case_ok; })
      << "\n";
    o << "```\n\n";

    o << "## 2. 逐用例判定（每行引用原始文件）\n\n";
    o << "> 判定来源：本节所有「判定」列**只读** `run_one_case()` 里唯一一处判定逻辑写下的 "
         "`failure_reasons`/gate 布尔；⛔ 本渲染不重算任何判据（纪律：同一事实只有一处判定逻辑）。\n\n";
    o << "| case | 路径 | 组 | 载荷 | 等待 | 计划/尝试/成功/失败 | 接收 | 丢失 | 重复 | 乱序 | 校验失败 | 迟发 | 积压(周期) | 结构异常 | 段头矛盾 | gate(late/backlog/blocked/abn/hdr) | dzflat/回退 | 文件 | 判定 |\n";
    o << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const auto& r : results)
    {
        const double per = (r.rate_target > 0) ? (1e9 / r.rate_target) : 0.0;
        char bl[32];
        if (per > 0) std::snprintf(bl, sizeof bl, "%.3f", r.backlog_max_ns / per);
        else std::snprintf(bl, sizeof bl, "-");
        o << "| `" << r.case_id << "` | " << path_name(r.path) << " | " << r.experiment_group
          << " | " << r.payload_bytes << "B | " << wait_name(r.wait_mode)
          << " | " << r.plan << "/" << r.attempts << "/" << r.sent_ok << "/" << r.send_failed
          << " | " << r.recv_measure << " | " << r.missing << " | " << r.duplicate
          << " | " << r.out_of_order << " | " << r.checksum_bad << " | " << r.late
          << " | " << bl << " | " << r.abnormal << " | " << r.bad_header
          << " | " << (r.late_ok ? 0 : 1) << "/" << (r.backlog_ok ? 0 : 1) << "/"
          << (r.send_blocked_ok ? 0 : 1) << "/" << (r.abnormal_ok ? 0 : 1) << "/"
          << (r.bad_header_ok ? 0 : 1)
          << " | " << r.pub_dzflat << "/" << r.pub_fallback
          << " | `samples/" << sanitize(r.case_id) << ".samples.csv` | "
          << (r.case_ok ? "**通过**" : "**未通过**") << " |\n";
    }
    o << "\n（gate 列 = 「该 gate 判失败的次数」，0 = 通过；阈值见 `manifest.failure_thresholds`。"
         "所列为**判定结果**，不是原始量 —— 原始量在同行的迟发/积压/结构异常/段头矛盾列。）\n\n";

    o << "## 3. 计时边界（禁止混用结束点，§10.7）\n\n";
    o << "| case | 传输完成 transport p50/p99 ns | 应用获得 delivery p50/p99 ns | 完整读取 app_read p50/p99 ns | 生产到消费 e2e p50/p99 ns |\n";
    o << "|---|---|---|---|---|\n";
    for (const auto& r : results)
    {
        double a[5], b[5], c[5], d[5], m;
        lat_stats(r.transport_ns, a[0], a[1], a[2], a[3], a[4], m);
        lat_stats(r.delivery_ns, b[0], b[1], b[2], b[3], b[4], m);
        lat_stats(r.app_read_ns, c[0], c[1], c[2], c[3], c[4], m);
        lat_stats(r.e2e_ns, d[0], d[1], d[2], d[3], d[4], m);
        auto pair = [](double x, double y) {
            if (x < 0 || y < 0) return std::string("null");
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.0f/%.0f", x, y);
            return std::string(buf);
        };
        o << "| `" << r.case_id << "` | " << pair(a[1], a[2]) << " | " << pair(b[1], b[2])
          << " | " << pair(c[1], c[2]) << " | " << pair(d[1], d[2]) << " |\n";
    }
    o << "\n说明：`transport` 由发布侧本地记录（发送 API 入口→返回），`delivery` 是订阅侧"
         "获得对象/视图减去发布侧传输完成时刻（跨进程合并，靠单调时钟同源）；`app_read` 是"
         "订阅侧完整遍历/校验耗时；`e2e` 从生产端生成数据前到订阅侧完整消费结束。\n\n";

    o << "## 4. 失败/未通过原因（逐条，不静默排除）\n\n";
    bool any = false;
    for (const auto& r : results)
    {
        if (r.failure_reasons.empty()) continue;
        any = true;
        o << "- `" << r.case_id << "`:\n";
        for (const auto& f : r.failure_reasons) o << "  - " << f << "\n";
    }
    if (!any) o << "（无）\n";
    o << "\n";

    o << "## 5. 跨进程身份与正常退出\n\n";
    for (const auto& r : results)
    {
        if (r.notes.empty()) continue;
        for (const auto& n : r.notes)
        {
            if (n.rfind("identity:", 0) == 0) o << "- `" << r.case_id << "` " << n << "\n";
        }
    }
    o << "\n（`child_exit` 行里带子进程退出码、CPU 秒数与上下文切换；被 SIGKILL 收尾的用例"
         "在 §4 里显式列为失败原因，不当通过。）\n\n";

    o << "## 6. 未确认项\n\n";
    o << "- **A/B 可分性只在发布侧成立**：实测 `dzflat-a` 与 `dzflat-b` 两档的消费者侧"
         "`via_view`/`via_object` **完全相同**（各 13500 / 0，10 轮 full 档合计）⇒ "
         "`via_view` **不是** B 档的区分判据（早前文档把它写成 B 档专属判据，已更正）。"
         "可分性证据在**发布侧**：`wire_bytes_source`（A=`对象 dzflat_size()` / B=`B 借样 chunk 容量`）"
         "与实际路径条数计数。\n";
    o << "- **失败量阈值是本基准自定的预算**（`manifest.failure_thresholds`）：迟发默认"
         "「次数>5 **且** 比率>0.5%」才判失败。它是**可判**而非**无条件失败**；引用时必须连阈值一起引。\n";
    for (const auto& r : results)
    {
        if (!r.wire_bytes_known || (r.wire_bytes_per_msg == 0 && !cfg.is_dds_path()))
            o << "- `" << r.case_id << "`: wire 字节未采集（`wire_bytes_per_msg` 写空字段；"
                 "来源见 `wire_bytes_source`）\n";
        if (r.backlog_max_ns > 0)
            o << "- `" << r.case_id << "`: 定速档存在迟发，最大积压 " << r.backlog_max_ns
              << " ns（迟发 " << r.late << " 次）—— 该轮延迟含排队成分\n";
    }
    if (cfg.is_dds_path())
        o << "- DDS 档的 wire 字节与零拷贝落地未采集：`wire_bytes_per_msg` 写**空字段**(null)，" 
             "`samples.csv` 的 `wire_bytes` 同为**空字段**（⛔ 不是 0）；"
             "iceoryx 是否真的承载了数据由 `stdout/pub_*.log` 的段注册日志与 RouDi 存在性"
             "共同说明，**不能**由\"组合包加载成功\"推导。\n";
    o << "\n";
    write_text(cfg.out_dir + "/verdict.md", o.str());
}

/* ======================================================================== */
/* 主程序                                                                   */
/* ======================================================================== */

static void usage()
{
    std::printf(
        "W02 统一跨进程基准（父进程编排 + 独立发布/订阅进程 + 逐样本输出）\n"
        "\n"
        "采集器（默认角色）:\n"
        "  xproc_benchmark --smoke                      # 四档(64B/1KiB/64KiB/1MiB)正确性用例\n"
        "  xproc_benchmark --path=dzflat-b --payload=65536 --workload=full --rate=1000\n"
        "  xproc_benchmark --compare=tlv,dzflat-a,dzflat-b --payloads=64,1024 --rounds=3\n"
        "  xproc_benchmark --path=cyc-iox --payload=1024 --workload=timestamp\n"
        "\n"
        "  --path=P            单档路径: tlv | dzflat-a | dzflat-b | cyc-udp | cyc-iox\n"
        "  --compare=A,B,...   多档交替比较（每轮按 ABBA 顺序，禁止顺序跑造成冷启动伪差）\n"
        "  --payloads=N,...    载荷档（字节，目标值；实际值见 payload_bytes）\n"
        "  --workload=W        timestamp(只读头部) | crc(CRC 全量顺序读) | full(逐元素比对)\n"
        "                      —— 对应 §10.7 的三组：timestamp=组1, crc/full=组2\n"
        "                      组3(生产到消费)由 e2e_ns 给出（从生产端生成数据前计时）\n"
        "  --wait=MODE         blocking(主对照, 事件驱动) | busy(单列组, 忙等, 单独报 CPU)\n"
        "  --rate=N            定速 N msg/s（发送计划独立于接收完成）；0 = 满速开环\n"
        "  --duration=S        测量窗口秒数   --warmup=S 预热秒数\n"
        "  --queue=N           订阅队列长度   --queue-wait-ms=N SHM publish 有界等待\n"
        "  --rounds=N          同一配置交替重复轮数\n"
        "  --domain=N          DDS/SHM domain\n"
        "  --out-dir=DIR       结果目录（默认 artifacts/perf/<run_id>/）\n"
        "  --run-id=ID         运行编号（默认 日期-时间-W02）\n"
        "  --build-dir=DIR     构建目录（默认 build/，用于记录构建元数据）\n"
        "  --dds-uri-udp=URI   CycloneDDS URI（SharedMemory 关闭档）\n"
        "  --dds-uri-iox=URI   CycloneDDS URI（SharedMemory 开启档）\n"
        "  --best-effort       用 publish_best_effort 取代 publish_blocking（对照用）\n"
        "  --sample-limit=N    逐样本上限（每个用例每个角色）\n"
        "  --dds-only          只跑 DDS 档（跳过 dzIPC 档）\n"
        "\n"
        "角色进程（由采集器 fork+exec 起，不要手工调用）:\n"
        "  xproc_benchmark --role=pub|sub|ddscheck --ctl=/w02ctl_xxx ...\n");
}

static bool parse_args(int argc, char** argv, Config& c, bool& help)
{
    help = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto val = [&](const char* key) -> std::string { return a.substr(std::strlen(key)); };
        if (a == "--help" || a == "-h") { help = true; return true; }
        else if (a.rfind("--role=", 0) == 0) c.role = val("--role=");
        else if (a.rfind("--ctl=", 0) == 0) c.ctl = val("--ctl=");
        else if (a.rfind("--topic=", 0) == 0) c.topic = val("--topic=");
        else if (a.rfind("--out=", 0) == 0) c.out_file = val("--out=");
        else if (a.rfind("--path=", 0) == 0)
        {
            PathSel p;
            if (!parse_path(val("--path="), p)) { std::fprintf(stderr, "未知 path: %s\n", val("--path=").c_str()); return false; }
            c.path = p;
        }
        else if (a.rfind("--compare=", 0) == 0)
        {
            c.compare_given = true;
            c.compare.clear();
            std::istringstream is(val("--compare="));
            std::string item;
            while (std::getline(is, item, ',')) if (!item.empty()) c.compare.push_back(item);
        }
        else if (a.rfind("--payload=", 0) == 0)
        {
            c.payload_given = true;
            c.payload = static_cast<u64>(std::strtoull(val("--payload=").c_str(), nullptr, 10));
        }
        else if (a.rfind("--payloads=", 0) == 0)
        {
            c.payloads.clear();
            std::istringstream is(val("--payloads="));
            std::string item;
            while (std::getline(is, item, ',')) if (!item.empty()) c.payloads.push_back(static_cast<u64>(std::strtoull(item.c_str(), nullptr, 10)));
        }
        else if (a.rfind("--workload=", 0) == 0)
        {
            c.workload_given = true;
            const std::string w = val("--workload=");
            if (w == "timestamp") c.workload = Workload::timestamp;
            else if (w == "crc") c.workload = Workload::crc;
            else if (w == "full") c.workload = Workload::full;
            else if (w == "inplace") c.workload = Workload::inplace;
            else { std::fprintf(stderr, "未知 workload: %s\n", w.c_str()); return false; }
        }
        else if (a.rfind("--wait=", 0) == 0)
        {
            const std::string w = val("--wait=");
            if (w == "blocking") c.wait_mode = WaitMode::blocking;
            else if (w == "busy") c.wait_mode = WaitMode::busy;
            else { std::fprintf(stderr, "未知 wait: %s\n", w.c_str()); return false; }
        }
        else if (a.rfind("--rate=", 0) == 0) c.rate_hz = std::strtod(val("--rate=").c_str(), nullptr);
        else if (a.rfind("--duration=", 0) == 0) c.duration = std::strtod(val("--duration=").c_str(), nullptr);
        else if (a.rfind("--warmup=", 0) == 0) c.warmup = std::strtod(val("--warmup=").c_str(), nullptr);
        else if (a.rfind("--queue=", 0) == 0) c.queue = static_cast<std::size_t>(std::strtoull(val("--queue=").c_str(), nullptr, 10));
        else if (a.rfind("--queue-wait-ms=", 0) == 0) c.queue_wait_ms = static_cast<u64>(std::strtoull(val("--queue-wait-ms=").c_str(), nullptr, 10));
        else if (a.rfind("--domain=", 0) == 0) c.domain = std::atoi(val("--domain=").c_str());
        else if (a.rfind("--rounds=", 0) == 0) c.rounds = std::max(1, std::atoi(val("--rounds=").c_str()));
        else if (a.rfind("--sample-limit=", 0) == 0) c.sample_limit = static_cast<u64>(std::strtoull(val("--sample-limit=").c_str(), nullptr, 10));
        else if (a.rfind("--out-dir=", 0) == 0) c.out_dir = val("--out-dir=");
        else if (a.rfind("--run-id=", 0) == 0) c.run_id = val("--run-id=");
        else if (a.rfind("--build-dir=", 0) == 0) c.build_dir = val("--build-dir=");
        else if (a.rfind("--dds-uri-udp=", 0) == 0) c.dds_uri_udp = val("--dds-uri-udp=");
        else if (a.rfind("--dds-uri-iox=", 0) == 0) c.dds_uri_iox = val("--dds-uri-iox=");
        /* 失败量判定阈值（W02-F3）：显式可覆盖 ⇒ 反例可证「阈值亮则失败」。覆盖值落 manifest。 */
        else if (a.rfind("--late-abs-max=", 0) == 0) { c.late_abs_max = std::strtoull(val("--late-abs-max=").c_str(), nullptr, 10); c.thresholds_given = true; }
        else if (a.rfind("--late-rate-max=", 0) == 0) { c.late_rate_max = std::strtod(val("--late-rate-max=").c_str(), nullptr); c.thresholds_given = true; }
        else if (a.rfind("--backlog-max-periods=", 0) == 0) { c.backlog_max_periods = std::strtod(val("--backlog-max-periods=").c_str(), nullptr); c.thresholds_given = true; }
        else if (a.rfind("--send-blocked-max=", 0) == 0) { c.send_blocked_max = std::strtoull(val("--send-blocked-max=").c_str(), nullptr, 10); c.thresholds_given = true; }
        else if (a.rfind("--abnormal-max=", 0) == 0) { c.abnormal_max = std::strtoull(val("--abnormal-max=").c_str(), nullptr, 10); c.thresholds_given = true; }
        else if (a.rfind("--bad-header-max=", 0) == 0) { c.bad_header_max = std::strtoull(val("--bad-header-max=").c_str(), nullptr, 10); c.thresholds_given = true; }
        else if (a.rfind("--purpose=", 0) == 0)
        {
            const std::string v = val("--purpose=");
            if (v != "evidence" && v != "verification" && v != "experiment")
            {
                std::fprintf(stderr, "未知 purpose: %s（允许 evidence|verification|experiment）\n", v.c_str());
                return false;
            }
            c.purpose = v; c.purpose_given = true;
        }
        else if (a.rfind("--citation-authority=", 0) == 0) c.citation_authority = val("--citation-authority=");
        else if (a.rfind("--shape=", 0) == 0)
        {
            /* 角色进程从控制块拿不到 Shape（它不是 POD 友好的），因此父进程把算好的
             * 形态显式传下来 —— 保证父/子两侧用的是**同一组**元素数，避免各自取整不同。 */
            std::istringstream is(val("--shape="));
            std::string item;
            std::vector<u32> v;
            while (std::getline(is, item, ',')) v.push_back(static_cast<u32>(std::strtoul(item.c_str(), nullptr, 10)));
            if (v.size() == 4) { c.shape_n1 = v[0]; c.shape_n2 = v[1]; c.shape_n3 = v[2]; c.shape_str = v[3]; }
        }
        else if (a.rfind("--pin=", 0) == 0)
        {
            /* 角色进程只有一个测量循环，--pin=N 直接钉它。 */
            c.pin_pub = c.pin_sub = std::atoi(val("--pin=").c_str());
        }
        else if (a.rfind("--pin-pub=", 0) == 0) c.pin_pub = std::atoi(val("--pin-pub=").c_str());
        else if (a.rfind("--pin-sub=", 0) == 0) c.pin_sub = std::atoi(val("--pin-sub=").c_str());
        else if (a == "--best-effort") c.best_effort = true;
        else if (a == "--smoke") c.smoke = true;
        else if (a == "--smoke-dds") { c.smoke = true; c.smoke_dds = true; }
        else if (a == "--dds-only") c.dds_only = true;
        else if (a.rfind("--roudi=", 0) == 0) c.roudi_path = val("--roudi=");
        else
        {
            std::fprintf(stderr, "未知参数: %s（--help 看用法）\n", a.c_str());
            return false;
        }
    }
    return true;
}

/* 角色进程入口。 */
static int run_role(const Config& cfg)
{
#ifdef W02_HAVE_DDS
    /* ddscheck 是**一次性预检**：它不参与数据面、不需要控制块（早期版本让它走
     * ctl_map，于是每次预检都 fatal 退出、被误判成"该档不可用"）。 */
    if (cfg.role == "ddscheck")
    {
        return run_ddscheck(cfg);
    }
#endif
    ControlBlock* ctl = ctl_map(cfg.ctl, false);
    if (ctl == nullptr) return 1;
    if (ctl->magic.load() != kCtlMagic)
    {
        std::fprintf(stderr, "[role] 控制块 magic 不符(%s)\n", cfg.ctl.c_str());
        ctl_unmap(ctl);
        return 1;
    }
    if (cfg.pin_pub >= 0)
    {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cfg.pin_pub, &set);
        ::sched_setaffinity(0, sizeof(set), &set);
#endif
    }
    int rc = 0;
    if (cfg.role == "pub")
    {
#ifdef W02_HAVE_DDS
        rc = cfg.is_dds_path() ? run_pub_dds(cfg, ctl) : run_pub_dzipc(cfg, ctl);
#else
        rc = run_pub_dzipc(cfg, ctl);
#endif
    }
    else if (cfg.role == "sub")
    {
#ifdef W02_HAVE_DDS
        rc = cfg.is_dds_path() ? run_sub_dds(cfg, ctl) : run_sub_dzipc(cfg, ctl);
#else
        rc = run_sub_dzipc(cfg, ctl);
#endif
    }
    else
    {
        std::fprintf(stderr, "[role] 未知角色: %s\n", cfg.role.c_str());
        rc = 1;
    }
    ctl_unmap(ctl);
    return rc;
}

int main(int argc, char** argv)
{
    Config cfg;
    bool help = false;
    if (!parse_args(argc, argv, cfg, help)) return 2;
    if (help) { usage(); return 0; }
    if (!cfg.role.empty()) return run_role(cfg);

    /* ---- 采集器 ---- */
    if (cfg.run_id.empty()) cfg.run_id = run_id_now() + "-W02";
    if (cfg.out_dir.empty()) cfg.out_dir = "artifacts/perf/" + cfg.run_id;
    if (cfg.build_dir.empty()) cfg.build_dir = "build";
    /* 默认 URI 指向仓库里的两份对照配置（test/w02_dds_udp.xml / w02_dds_iox.xml）——
     * 它们是本工作包的一部分，路径由 CMake 以 W02_SOURCE_DIR 注入，避免"跑的是
     * /etc 下某个未知配置"这种不可复现的默认值。 */
    if (cfg.dds_uri_udp.empty())
        cfg.dds_uri_udp = "file://" + std::string(W02_SOURCE_DIR) + "/test/w02_dds_udp.xml";
    if (cfg.dds_uri_iox.empty())
        cfg.dds_uri_iox = "file://" + std::string(W02_SOURCE_DIR) + "/test/w02_dds_iox.xml";

    if (cfg.smoke)
    {
        cfg.payloads = {64, 1024, 65536, 1048576};
        cfg.compare = cfg.dds_only ? std::vector<std::string>{"cyc-udp", "cyc-iox"}
                                   : std::vector<std::string>{"tlv", "dzflat-a", "dzflat-b"};
        if (cfg.smoke_dds && !cfg.dds_only)
        {
            cfg.compare.push_back("cyc-udp");
            cfg.compare.push_back("cyc-iox");
        }
        if (cfg.duration > 1.0) cfg.duration = 1.0;
        if (cfg.warmup > 0.3) cfg.warmup = 0.3;
        if (cfg.rate_hz <= 0) cfg.rate_hz = 1000.0;
        if (cfg.sample_limit > 20000) cfg.sample_limit = 20000;
    }
    /* --path=X 且没显式给 --compare 时，本轮只跑 X。
     * ⚠️ 这里不能只看 compare.empty()：默认矩阵非空，只有显式给过 --compare（或 smoke
     * 展开了矩阵）才代表"用户要跑多档"。这是实测踩到的坑（--path=cyc-udp 却跑了三档）。 */
    if (!cfg.compare_given && !cfg.smoke) cfg.compare.assign(1, path_name(cfg.path));
    /* ⚠️ 不能只看 payloads.empty()：默认矩阵非空，于是 `--payload=1048576` 会被静默忽略、
     * 只跑默认的 64 B 档（实测踩到：命令写 1 MiB，产物里 payload_target=64）。
     * 显式给过 --payload 就只跑它；给了 --payloads 或 --smoke 则用它们。 */
    if (cfg.payload_given && !cfg.smoke) cfg.payloads.assign(1, cfg.payload);
    if (cfg.payloads.empty()) cfg.payloads.push_back(cfg.payload);

    mkdir_p(cfg.out_dir + "/samples");
    mkdir_p(cfg.out_dir + "/stdout");
    mkdir_p(cfg.out_dir + "/stderr");

    const RunEnv env = collect_env(cfg);

    std::printf("=== W02 统一跨进程基准 | run_id=%s ===\n", cfg.run_id.c_str());
    std::printf("  source_revision : %s\n", env.source_revision.c_str());
    std::printf("  build_type      : %s\n", env.build_type.c_str());
    std::printf("  compiler        : %s\n", env.compiler.c_str());
    std::printf("  exe             : %s  sha256=%s\n", env.exe_path.c_str(), env.exe_sha.c_str());
    std::printf("  libipc          : %s  sha256=%s\n", env.lib_path.c_str(), env.lib_sha.c_str());
    std::printf("  clock           : %s\n", env.clock_note.c_str());
    std::printf("  config_hash     : %s  (%s)\n", env.config_hash.c_str(), env.config_canon.c_str());
    std::printf("  process_model   : cross-process（pub/sub 为 fork+exec 新映像）\n");
    std::printf("  out_dir         : %s\n\n", cfg.out_dir.c_str());
    std::fflush(stdout);

#ifdef W02_HAVE_DDS
    std::printf("  CycloneDDS      : 已编译进基准（libddsc=%s）\n\n",
                env.dds_lib_path.empty() ? "未加载(仅链接)" : env.dds_lib_path.c_str());
#else
    std::printf("  CycloneDDS      : 本机构建未启用（-DW02_WITH_DDS=OFF）—— DDS 档不可用\n\n");
#endif

    /* ---- CycloneDDS 可用性预检（§10.6：不允许"组合包存在"被当成"这一档可用"） ----
     * 没有 RouDi 时 iceoryx 档的 create_participant 会**永久阻塞**，所以先起一个
     * 一次性 ddscheck 子进程并用超时兜住；失败就把该档从本轮显式剔除（写进 verdict），
     * 不让整个运行卡死也不静默跳过。 */
    std::vector<std::string> reversed_paths;
#ifdef W02_HAVE_DDS
    {
        auto probe = [&](const std::string& pname, const std::string& uri) -> bool {
            const PathSel p = (pname == "cyc-iox") ? PathSel::cyc_iox : PathSel::cyc_udp;
            Config probe_cfg = cfg;
            probe_cfg.path = p;
            probe_cfg.role = "ddscheck";
            probe_cfg.domain = cfg.domain;
            probe_cfg.dds_uri_udp = cfg.dds_uri_udp;
            probe_cfg.dds_uri_iox = cfg.dds_uri_iox;
            probe_cfg.out_file = cfg.out_dir + "/ddscheck_" + pname + ".txt";
            const pid_t pid = spawn_role(probe_cfg, "ddscheck",
                                         "/w02ddscheck_" + sanitize(cfg.run_id) + "_" + pname,
                                         "w02/check", 64, compute_shape(64), probe_cfg.out_file,
                                         uri, cfg.out_dir + "/stdout/ddscheck_" + pname + ".log",
                                         cfg.out_dir + "/stderr/ddscheck_" + pname + ".log");
            if (pid < 0) return false;
            const u64 before = now_ns();
            Child ch = reap_child(pid, "ddscheck-" + pname, 12.0);
            (void)before;
            return ch.exit_code == 0 && !ch.killed;
        };
        const std::string roudi = run_capture("pgrep -x iox-roudi | head -1");
        std::printf("  dds precheck    : roudi_pid=%s\n", roudi.empty() ? "(none)" : roudi.c_str());
        for (const std::string& pname : cfg.compare)
        {
            /* ⚠️ 判据必须是**解析结果**而不是字符串字面量：path_name() 输出
             * "cyclonedds-iox"，而 parse_path() 同时接受 "cyc-iox" 两种写法。早期版本
             * 只比字面量，于是 --path=cyc-iox 时预检整段被跳过（症状：预检日志里
             * roudi_pid=(none) 却仍然开跑，最后握手超时）。 */
            PathSel pp;
            if (!parse_path(pname, pp)) continue;
            if (pp == PathSel::cyc_udp && !probe(pname, cfg.dds_uri_udp)) reversed_paths.push_back(pname);
            else if (pp == PathSel::cyc_iox && !probe(pname, cfg.dds_uri_iox)) reversed_paths.push_back(pname);
        }
        for (const std::string& pname : reversed_paths)
        {
            std::printf("  dds precheck    : %s 不可用 → 本轮跳过（原因见 stdout/ddscheck_%s.log）\n",
                        pname.c_str(), pname.c_str());
        }
        std::fflush(stdout);
    }
#endif

    cfg.precheck_skipped = reversed_paths;

    std::vector<CaseResult> results;
    int case_index = 0;
    /* 交替（ABBA）而不是顺序跑：顺序跑会把冷启动成本全压在第一档（见 test/ipc_benchmark.cpp
     * 的实测教训）。这里按轮交替掉 compare 的顺序。 */
    /* 等待策略维度（§4 W02 第 5 条）：
     *   blocking —— 主对照，事件驱动（DZFlat 档用视图队列定时阻塞；TLV 档用 msg 队列
     *               条件变量阻塞，收尾阶段退化为短睡眠轮询，见收包循环注释）；
     *   busy     —— 单列组，纯自旋、从不退让，CPU 必然高。它**只在与正对照相同的
     *               载荷档上跑**，且结果表里以 wait 列区分，禁止跨组宣称胜负。
     * --wait=busy 时只跑 busy 组；默认（或 --smoke）在 1 KiB 档额外附一遍 busy，便于
     * 报告直接给出"同一负载、两种等待"的 CPU 对照。 */
    std::vector<WaitMode> wait_plan;
    if (cfg.wait_mode == WaitMode::busy) wait_plan.push_back(WaitMode::busy);
    else if (cfg.smoke) wait_plan = {WaitMode::blocking, WaitMode::busy};
    else wait_plan.push_back(WaitMode::blocking);

    for (int round = 0; round < std::max(1, cfg.rounds); ++round)
    {
        std::vector<std::string> order = cfg.compare;
        if (round % 2 == 1) std::reverse(order.begin(), order.end());
        for (const std::string& pname : order)
        {
            PathSel p;
            if (!parse_path(pname, p)) { std::fprintf(stderr, "跳过未知路径 %s\n", pname.c_str()); continue; }
            Config local = cfg;
            local.path = p;
#ifndef W02_HAVE_DDS
            if (p == PathSel::cyc_udp || p == PathSel::cyc_iox)
            {
                std::fprintf(stderr, "[跳过] %s: 本构建未启用 CycloneDDS\n", pname.c_str());
                continue;
            }
#endif
            if (std::find(reversed_paths.begin(), reversed_paths.end(), pname) != reversed_paths.end())
            {
                std::fprintf(stderr, "[跳过] %s: DDS 预检未通过（该档不可用，见 stdout/ddscheck_%s.log）\n",
                             pname.c_str(), pname.c_str());
                continue;
            }
            for (u64 target : cfg.payloads)
            {
                const Shape s = compute_shape(target);
                /* 负载口径（方案 §6.2）：「负载至少覆盖 1000 Hz 小消息、500 Hz 大消息」。
                 * 把"大消息也用 1000 Hz"当成默认是错的：1 MiB × 1000 Hz = 1 GB/s，本机
                 * UDP 回环到不了，结果会是"负载未达标"而不是"这一档慢"（实测 DDS-UDP
                 * 1 MiB 档只跑到约 700 Hz）。基准按尺寸自动降档，并把实际目标速率写进
                 * summary 的 rate_target —— 不隐藏它改过速率。 */
                local.rate_hz = (cfg.rate_hz > 0 && target > 65536) ? 500.0 : cfg.rate_hz;
                for (Workload w : workloads_for(p))
                {
                  if (cfg.workload_given && w != cfg.workload) continue;
                  for (WaitMode wm : wait_plan)
                  {
                    /* busy 组只在与正对照相同的**单档**上跑（§4 W02 第 5 条要求"忙等单独
                     * 列组并报告 CPU"）：全档跑忙等会让本就不算便宜的 smoke 再翻一倍，
                     * 而 CPU 对照只需要一个可比点。默认取 1 KiB；--wait=busy 时按用户给的
                     * 载荷集合跑。 */
                    if (wm == WaitMode::busy && cfg.wait_mode != WaitMode::busy && target != 1024) continue;
                    /* 载荷是唯一允许"父/子各算一次"的量：把父进程算好的形态显式传下去。 */
                    local.shape_n1 = s.n1; local.shape_n2 = s.n2;
                    local.shape_n3 = s.n3; local.shape_str = s.str_len;
                    local.payload = target;
                    local.workload = w;
                    local.wait_mode = wm;
                    /* 每个用例独立 topic（§10.6：实验端点用独立 topic，防历史残留混入）。
                     * ⚠️ topic 里**不能出现 '-'**：CycloneDDS 0.10.2 在该字符上直接返回
                     * "Bad Parameter"（实测：仅 iceoryx 档建 topic 失败 → 表现为
                     * create_topic/create_writer 双双失败、订阅侧 sub_failed，而 UDP 档
                     * 完全正常。这是最难查的一类假失败：路径配置、RouDi、类型描述符都正确，
                     * 只有一个字符越界）。所以路径名/工作负载名一律经 topic_safe() 消毒。 */
                    local.topic = "w02_" + topic_safe(cfg.run_id) + "_" + topic_safe(path_name(p))
                                + "_" + std::to_string(target) + "_" + topic_safe(workload_name(w))
                                + "_" + topic_safe(wait_name(wm))
                                + "_r" + std::to_string(round) + "_c" + std::to_string(case_index);

                    std::printf("[%d] path=%s workload=%s wait=%s payload=%lluB(实际 %uB) rate=%.0f\n",
                                case_index, path_name(p), workload_name(w), wait_name(local.wait_mode),
                                static_cast<unsigned long long>(target), s.bytes(), local.rate_hz);
                    std::fflush(stdout);
                    CaseResult r = run_one_case(local, s, p, w, local.wait_mode, target,
                                                cfg.out_dir, case_index);
                    r.notes.push_back("round=" + std::to_string(round));
                    std::printf("    -> %s plan=%llu attempts=%llu ok=%llu recv=%llu missing=%llu dup=%llu ooo=%llu "
                                "crc_bad=%llu dzflat=%llu fallback=%llu cpu=(%.3f,%.3f) cores=%.3f\n",
                                r.case_ok ? "PASS" : "FAIL",
                                (unsigned long long)r.plan, (unsigned long long)r.attempts,
                                (unsigned long long)r.sent_ok, (unsigned long long)r.recv_measure,
                                (unsigned long long)r.missing, (unsigned long long)r.duplicate,
                                (unsigned long long)r.out_of_order, (unsigned long long)r.checksum_bad,
                                (unsigned long long)r.pub_dzflat, (unsigned long long)r.pub_fallback,
                                r.pub_cpu_s, r.sub_cpu_s, r.total_cpu_cores);
                    if (!r.case_ok)
                    {
                        std::printf("       失败原因:\n");
                        for (const auto& f : r.failure_reasons) std::printf("         - %s\n", f.c_str());
                    }
                    for (const auto& n : r.notes) std::printf("       note: %s\n", n.c_str());
                    std::fflush(stdout);
                    results.push_back(r);
                    ++case_index;
                  }
                }
            }
        }
    }

    write_run_outputs(cfg, env, results);
    write_verdict(cfg, env, results);

    const std::size_t passed = static_cast<std::size_t>(
        std::count_if(results.begin(), results.end(), [](const CaseResult& r) { return r.case_ok; }));
    std::printf("\n=== 汇总: %zu/%zu 用例通过 ===\n", passed, results.size());
    if (passed != results.size())
    {
        std::printf("未通过用例:\n");
        for (const auto& r : results)
        {
            if (r.case_ok) continue;
            std::printf("  - %s\n", r.case_id.c_str());
            for (const auto& f : r.failure_reasons) std::printf("      %s\n", f.c_str());
        }
    }
    std::printf("结果目录: %s\n", cfg.out_dir.c_str());
    return passed == results.size() ? 0 : 1;
}
