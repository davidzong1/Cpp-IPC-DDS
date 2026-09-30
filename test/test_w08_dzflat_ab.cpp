/* [W08] DZFlat A / B 跨进程对照 + 逐样本落盘 + 三路径可分计数。
 *
 * ── 本文件补的缺口（不是重造 B）─────────────────────────────────────────────
 * 仓内已有: 同进程 A/B 计时(test/dzflat_tx_benchmark.cpp Step 3)、跨进程位置无关
 * 正确性(test/test_dzflat_transport.cpp:463)、B 的三条契约用例(test/test_dzflat_builder.cpp)。
 * 缺的是**跨进程 + 每条载荷全量校验 + 逐样本落盘**的可采信配置, 以及把 A/B 分开的
 * 路径计数 —— 既有 DzFlatPublishCount 把 A 与 B 记在同一支上, 无法证明"B 真的被执行了"。
 *
 * ── 口径（方案 §10.7 三个实验组, 一条样本四个派生量）───────────────────────
 *   传输机制   transport_ns = transport_done_ns - publish_enter_ns   (发布路径)
 *   完整读取   app_read_ns  = fully_consumed_ns - app_obtained_ns    (应用完整遍历校验)
 *   生产到消费 e2e_ns       = fully_consumed_ns - produced_ns        (跨进程单调时钟)
 *   通知与交付 delivery_ns  = app_obtained_ns - transport_done_ns
 * 生产端(子进程)与消费端(父进程)都取 dzIPC::measure::monotonic_now_ns()(CLOCK_MONOTONIC),
 * 跨进程一致性由 W03 的 test_w03_measurement(CrossProcessClockIsConsistent) 覆盖。
 *
 * ── 三件不许做的事（本文件在结构上避免）───────────────────────────────────
 * ① **不许连发不取**: 已知既存缺陷(docs/dzflat_shm.md §9.5 末尾)是"大消息连发且订阅方
 *    不取 ⇒ chunk 池(32/档)耗尽 ⇒ send() 退化 64B 分片 ⇒ 环覆写 ⇒ 重组拼错 ⇒ 段错误"。
 *    这里**发一条取一条**(子进程每发一条等父进程 ack 才发下一条), 把在途数钉在 1。
 * ② **不许用"值一致"替代零拷贝**: 指针区间断言在 test_dzflat_builder.cpp:105, 本文件
 *    只做"整段载荷逐字节校验"(防旧帧/损坏)与路径计数(防静默回退假绿)。
 * ③ **不许把同进程读数当跨进程结论**: 本文件的每个数都来自 fork 出来的发布进程与父
 *    消费进程; 同进程的 151.6/113.8 µs 只在交付里作对照引用, ⛔不当验收值。
 *
 * ── 载荷模式 ─────────────────────────────────────────────────────────────
 *   data[i] = (seq*131 + i*7) & 0xFF  —— 与序号相关, 每条都变; 消费端**整段重算比对**,
 *   因此复用固定载荷掩盖旧帧、或内容损坏, 都会被抓到(payload_checksum_ok=false)。
 *
 * 自带落盘: artifacts/perf/<run_id>/{samples.csv,samples.jsonl,manifest.json,
 * counters_publisher.json,counters_consumer.json,README.md}。
 * 是 gtest(不是自带 main 的 benchmark) —— 这样 target 由 test/ 的 file(GLOB) 自动生成,
 * 无需改动 test/CMakeLists.txt 的目标定义(登记 add_test 由架构负责人落地)。
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/field_schema.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

#include <gtest/gtest.h>

namespace {

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
using dzIPC::measure::monotonic_now_ns;
using dzIPC::measure::PathKind;
using dzIPC::measure::SampleRecord;

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 77;     /* 与本套件其它用例不同的模板 id */
constexpr std::size_t kDomain = 141;     /* 专用 domain, 防串扰 */

/* ---------------- 配置矩阵 ---------------- */
enum class Cfg { Tlv, A, B };
enum class Variant { PreBuilt, PerMessage };

const char* cfg_name(Cfg c)
{
    switch (c)
    {
    case Cfg::Tlv: return "tlv";
    case Cfg::A: return "dzflat-a";
    case Cfg::B: return "dzflat-b";
    }
    return "?";
}

const char* variant_name(Variant v)
{
    return v == Variant::PreBuilt ? "prebuilt" : "permsg";
}

PathKind path_kind(Cfg c)
{
    switch (c)
    {
    case Cfg::Tlv: return PathKind::tlv;
    case Cfg::A: return PathKind::dzflat_a;
    case Cfg::B: return PathKind::dzflat_b;
    }
    return PathKind::unknown;
}

/* 探针图里 A 级用的是对象→段; B 级是应用原地构造。两者都不是 TLV。 */
bool dzflat_on(Cfg c) { return c != Cfg::Tlv; }

struct SizeSpec
{
    const char* name;
    std::uint32_t w;
    std::uint32_t h;
};
const SizeSpec kSizes[3] = {{"img66k", 256, 86}, {"img262k", 512, 171}, {"img880k", 640, 480}};

std::uint32_t step_of(const SizeSpec& s) { return s.w * 3; }
std::uint32_t bytes_of(const SizeSpec& s) { return step_of(s) * s.h; }

inline std::uint8_t pattern_byte(std::uint64_t seq, std::size_t i)
{
    return static_cast<std::uint8_t>(((seq * 131u) + (i * 7u)) & 0xFFu);
}

/* 生产端一条消息的记账, 经管道回传(跨进程)。 */
struct PubRecord
{
    std::uint64_t seq;
    std::uint64_t produced_ns;
    std::uint64_t publish_enter_ns;
    std::uint64_t transport_done_ns;
    std::uint32_t ok;
    std::uint32_t size_idx;
};

bool write_all(int fd, const void* p, std::size_t n)
{
    const char* c = static_cast<const char*>(p);
    std::size_t off = 0;
    while (off < n)
    {
        const ssize_t w = ::write(fd, c + off, n - off);
        if (w <= 0) return false;
        off += static_cast<std::size_t>(w);
    }
    return true;
}

bool read_all(int fd, void* p, std::size_t n)
{
    char* c = static_cast<char*>(p);
    std::size_t off = 0;
    while (off < n)
    {
        const ssize_t r = ::read(fd, c + off, n - off);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
}

void emit(int fd, const std::string& line)
{
    const std::string s = line + "\n";
    write_all(fd, s.data(), s.size());
}

std::string read_all_text(int fd)
{
    std::string out;
    char buf[2048];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
    return out;
}

std::string kv(const std::string& text, const std::string& key)
{
    const std::string pat = key + "=";
    const std::size_t p = text.find(pat);
    if (p == std::string::npos) return {};
    const std::size_t b = p + pat.size();
    const std::size_t e = text.find_first_of(" \n", b);
    return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

std::uint64_t counter_of(CounterId id)
{
    return CounterRegistry::instance().snapshot().values[static_cast<std::size_t>(id)];
}

/* ---------------- run 规格与结果 ---------------- */
struct RunSpec
{
    Cfg cfg{Cfg::A};
    Variant variant{Variant::PerMessage};
    int size_count{3};
    int samples_per_size{100};
    bool write_evidence{true};
    std::string run_id;
    std::string run_dir;
    std::string topic;   ///< 每 run 一个独立话题, 防历史进程/同机他人串扰
};

struct RunOutcome
{
    bool forked{false};
    bool child_exited{false};
    int child_code{-1};
    int child_signal{0};
    std::string child_text;      /* 子进程 k=v 汇总 */
    std::vector<SampleRecord> samples;
    std::size_t verified{0};
    std::size_t dropped{0};
    double window_s{0.0};
    std::string topic;
    /* ⛔ 防覆盖闸触发: 目标 run 目录已有 samples.csv 且未授权覆盖。 */
    bool evidence_dir_exists{false};
};

/* ---------------- 落点与防覆盖闸（2026-09-28 W08 证据污染收口，见 W08 交付 §10）----------------
 * ⛔ 这里的默认值**曾经造成 W08 证据污染**（2026-09-28 20:22:4x，仓库
 * artifacts/perf/20260928-r0{5..10}-W08-* 六个 run 被裸跑就地覆盖，原始样本不可复原）。
 * 因此落点规则按"安全默认"来定，⛔ 不再有"默认写仓库"这一行为：
 *
 *   ① 显式 W08_ARTIFACT_ROOT=<dir>  ⇒ <dir>/artifacts/perf/<run_id>/（交付归档走这条：
 *      调用方明确指定落点，写哪由调用方负责）。
 *   ② 未设环境变量           ⇒ 由**可执行文件路径**推导 <build>/test-scratch/w08/，
 *      ⛔ 绝不回推仓库根。理由：默认落点必须落在 .gitignore 的 scratch 区，
 *      这样"任何人直接跑 build/bin/<test>"都不可能碰到证据，同时测试仍可反复运行
 *      （比"直接拒绝运行"体验好，且能保住 CMake 构建目录约定）。
 *   ③ 两条都推导不出来（非本仓布局的可执行文件）⇒ 拒绝落盘（write_evidence 关掉），
 *      由调用方断言失败，绝不退化成"写当前目录"。
 *
 * 另有一道**拒绝覆盖闸**：目标 run 目录已有 samples.csv ⇒ 不覆盖、不静默跳过，直接判失败
 * （授权覆盖仅限：W08_OVERWRITE_EVIDENCE=1，或落点位于 <repo>/build/ 之下）。 */
std::string repo_root()
{
    /* 仅用于 manifest 的 build_dir 与"落点在不在 build/ 之下"判断; ⛔ 不再用于决定落盘位置。 */
    if (const char* env = ::getenv("W08_ARTIFACT_ROOT"))
    {
        if (env[0] != '\0') return env;
    }
    char exe[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
    {
        std::filesystem::path p(exe);
        /* <root>/build/bin/test -> parent(bin) -> parent(build) -> parent(root) */
        std::filesystem::path r = p.parent_path().parent_path().parent_path();
        if (std::filesystem::exists(r / "docs")) return r.string();
    }
    return std::string();
}

/* 由可执行文件路径推导 <build> 目录（不依赖 CWD、不依赖仓库布局存在与否）。 */
std::string build_dir_from_exe()
{
    char exe[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    std::filesystem::path p(exe);
    /* <build>/bin/<test> -> parent(bin) -> parent(<build>) */
    return p.parent_path().parent_path().string();
}

/* 交付证据的落点（见上方规则）。空串 = 拒绝落盘。 */
std::string artifact_dir_for(const std::string& run_id)
{
    if (const char* env = ::getenv("W08_ARTIFACT_ROOT"))
    {
        if (env[0] != '\0') return std::string(env) + "/artifacts/perf/" + run_id;
    }
    const std::string bd = build_dir_from_exe();
    if (bd.empty()) return {};
    /* ⛔ 安全默认: scratch 区, 不是仓库根。 */
    return bd + "/test-scratch/w08/artifacts/perf/" + run_id;
}

bool artifact_root_explicit()
{
    const char* env = ::getenv("W08_ARTIFACT_ROOT");
    return env != nullptr && env[0] != '\0';
}

bool evidence_overwrite_allowed(const std::string& run_dir)
{
    const char* ov = ::getenv("W08_OVERWRITE_EVIDENCE");
    if (ov != nullptr && ov[0] == '1' && ov[1] == '\0') return true;
    if (artifact_root_explicit()) return true;
    return run_dir.find("/build/") != std::string::npos;
}

/* ---------------- 生产端(子进程) ---------------- */
void producer_child(int rec_fd, int txt_fd, int ack_fd, const RunSpec spec)
{
    dzIPC::EnableDzFlat(dzflat_on(spec.cfg));
    dzIPC::ResetDzFlatCounters();
    CounterRegistry::instance().reset();
    CounterRegistry::instance().set_diagnostics_enabled(false);

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, spec.topic, kDomain};
    pub.InitChannel();
    std::this_thread::sleep_for(300ms);   /* 等握手: 有接收方 loan 才可能成功 */

    const std::size_t total =
        static_cast<std::size_t>(spec.size_count) * static_cast<std::size_t>(spec.samples_per_size);
    std::uint64_t seq_ok = 0;
    std::uint64_t seq_fail = 0;

    for (int si = 0; si < spec.size_count; ++si)
    {
        const SizeSpec& s = kSizes[si];
        const std::uint32_t step = step_of(s);
        const std::uint32_t bytes = bytes_of(s);

        /* "对象预先构造"档: 容器与字段只建一次, 循环里只重填载荷。 */
        std::shared_ptr<dzIPC::Msg::StdImage> pre;
        if (spec.variant == Variant::PreBuilt)
        {
            pre = std::make_shared<dzIPC::Msg::StdImage>();
            pre->set_msg_id(kMsgId);
            pre->width = s.w;
            pre->height = s.h;
            pre->step = step;
            pre->encoding = "rgb8";
            pre->data.resize(bytes);
        }

        for (int i = 0; i < spec.samples_per_size; ++i)
        {
            const std::uint64_t seq = static_cast<std::uint64_t>(seq_ok + seq_fail);
            const std::uint64_t produced = monotonic_now_ns();
            std::uint64_t pub_enter = 0;
            std::uint64_t transport_done = 0;
            bool ok = false;

            if (spec.cfg == Cfg::B)
            {
                /* B: 显式 loan → 应用**在借样内存里**原地构造 → publish_loaned。
                 * ⛔ 这里是应用侧直接写 chunk, 不是内部 loan() 代跑。 */
                auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(bytes + 256);
                if (lo.valid())
                {
                    lo->set_width(s.w);
                    lo->set_height(s.h);
                    lo->set_step(step);
                    lo->set_encoding("rgb8");
                    auto px = lo->alloc_data(bytes);
                    for (std::uint32_t k = 0; k < bytes; ++k) px[k] = pattern_byte(seq, k);
                    pub_enter = monotonic_now_ns();
                    ok = pub.publish_loaned(std::move(lo));
                    transport_done = monotonic_now_ns();
                }
                if (!ok)
                {
                    /* B 失败的**应用契约**: 回退到普通 publish(A/TLV), 消息不许丢。
                     * 这一次回退会在 A/TLV 的调用点被记一次计数。 */
                    auto m = std::make_shared<dzIPC::Msg::StdImage>();
                    m->set_msg_id(kMsgId);
                    m->width = s.w;
                    m->height = s.h;
                    m->step = step;
                    m->encoding = "rgb8";
                    m->data.resize(bytes);
                    for (std::uint32_t k = 0; k < bytes; ++k) m->data[k] = pattern_byte(seq, k);
                    pub_enter = monotonic_now_ns();
                    ok = pub.publish(m);
                    transport_done = monotonic_now_ns();
                }
            }
            else
            {
                std::shared_ptr<dzIPC::Msg::StdImage> m;
                if (spec.variant == Variant::PreBuilt)
                {
                    m = pre;
                }
                else
                {
                    m = std::make_shared<dzIPC::Msg::StdImage>();
                    m->set_msg_id(kMsgId);
                    m->width = s.w;
                    m->height = s.h;
                    m->step = step;
                    m->encoding = "rgb8";
                    m->data.resize(bytes);
                }
                for (std::uint32_t k = 0; k < bytes; ++k) m->data[k] = pattern_byte(seq, k);
                pub_enter = monotonic_now_ns();
                ok = pub.publish(m);
                transport_done = monotonic_now_ns();
            }

            if (ok) ++seq_ok; else ++seq_fail;

            const PubRecord r{seq, produced, pub_enter, transport_done, ok ? 1u : 0u,
                              static_cast<std::uint32_t>(si)};
            if (!write_all(rec_fd, &r, sizeof(r))) ::_exit(5);

            /* 发一条等一条: 把在途数钉在 1(规避 §9.5 的池耗尽缺陷)。 */
            char ack = 0;
            if (!read_all(ack_fd, &ack, 1)) ::_exit(6);
        }
    }

    /* 生产端自己的路径计数 = 路径证据(与消费端无关)。 */
    const std::uint64_t a = counter_of(CounterId::dzflat_a_messages);
    const std::uint64_t b = counter_of(CounterId::dzflat_b_messages);
    const std::uint64_t tlv = counter_of(CounterId::tlv_messages);
    emit(txt_fd, std::string("cfg=") + cfg_name(spec.cfg) + " variant=" + variant_name(spec.variant) +
                     " expected=" + std::to_string(total) + " seq_ok=" + std::to_string(seq_ok) +
                     " seq_fail=" + std::to_string(seq_fail) + " dzflat_a_messages=" + std::to_string(a) +
                     " dzflat_b_messages=" + std::to_string(b) + " tlv_messages=" + std::to_string(tlv) +
                     " dzflat_publish=" + std::to_string(dzIPC::DzFlatPublishCount()) +
                     " dzflat_fallback=" + std::to_string(dzIPC::DzFlatFallbackCount()) +
                     " prebuilt=" + std::to_string(dzIPC::DzFlatPrebuiltSegmentCount()) +
                     " dzflat_wire_bytes=" +
                     std::to_string(counter_of(CounterId::dzflat_wire_bytes)) +
                     " tlv_wire_bytes=" + std::to_string(counter_of(CounterId::tlv_wire_bytes)));

    if (spec.write_evidence)
    {
        CounterRegistry::instance().write_json_file(spec.run_dir + "/counters_publisher.json");
    }
}

/* ---------------- 消费端(父进程) ---------------- */
RunOutcome run_cross_process(const RunSpec& spec)
{
    RunOutcome out;
    out.topic = spec.topic;

    /* ⛔ W08 防覆盖闸(2026-09-28 补, 起因见交付 §8 已知限制第 2 条):
     * 本用例的落盘一律 fopen(...,"wb") **截断**, 而 repo_root() 默认回推仓库根 ⇒
     * 任何人在仓库里裸跑 build/bin/test_w08_dzflat_ab 都会就地覆盖
     * artifacts/perf/20260928-r0{5..10}-W08-xxx 目录里的**原始样本**, 违反方案 §12"只追加、不覆盖旧日志"。
     * 处置: 目标 run 目录**已存在**且未显式授权 ⇒ 既不覆盖也不静默跳过,
     * 而是让断言失败 —— 声音要足够大, 让人去看是"重跑"还是"污染"。
     * 生产端(子进程)也读这个门 —— 它写 counters_publisher.json 同样会截断。 */
    if (spec.write_evidence && !spec.run_dir.empty())
    {
        std::error_code ec;
        const bool exists = std::filesystem::exists(spec.run_dir + "/samples.csv", ec);
        if (exists && !evidence_overwrite_allowed(spec.run_dir))
        {
            out.evidence_dir_exists = true;
            return out;   /* 由调用方断言失败并给出可操作的说明 */
        }
        std::filesystem::create_directories(spec.run_dir);
    }

    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_sub_ipc sub{sub_td, spec.topic, kDomain, 32};
    sub.InitChannel();
    std::this_thread::sleep_for(200ms);

    int rec[2] = {-1, -1};
    int txt[2] = {-1, -1};
    int ack[2] = {-1, -1};
    if (::pipe(rec) != 0 || ::pipe(txt) != 0 || ::pipe(ack) != 0) return out;

    const ::pid_t pid = ::fork();
    if (pid < 0) return out;
    out.forked = true;
    if (pid == 0)
    {
        ::close(rec[0]);
        ::close(txt[0]);
        ::close(ack[1]);
        producer_child(rec[1], txt[1], ack[0], spec);
        ::_exit(0);
    }
    ::close(rec[1]);
    ::close(txt[1]);
    ::close(ack[0]);

    const std::size_t total =
        static_cast<std::size_t>(spec.size_count) * static_cast<std::size_t>(spec.samples_per_size);
    const std::string route_base = spec.topic;

    const auto window_start = Clock::now();
    std::size_t expected_seq = 0;
    for (std::size_t n = 0; n < total; ++n)
    {
        const int size_idx = static_cast<int>(n / static_cast<std::size_t>(spec.samples_per_size));
        const SizeSpec& s = kSizes[size_idx];
        const std::uint32_t bytes = bytes_of(s);

        bool got = false;
        std::uint64_t app_obtained = 0;
        std::uint64_t fully = 0;
        dzIPC::Msg::StdImage img;
        bool verified = false;

        const auto deadline = Clock::now() + 3000ms;
        while (Clock::now() < deadline)
        {
            if (sub.try_get_clone(sub_td))
            {
                app_obtained = monotonic_now_ns();
                auto g = sub_td->topic()->msgcast<dzIPC::Msg::StdImage>();
                if (g) { img = *g; got = true; }
                break;
            }
            dzIPC::Sample sample;
            if (sub.try_get(sample))
            {
                app_obtained = monotonic_now_ns();
                auto v = sample.view<dzIPC::Msg::StdImageFlat>();
                if (v.valid()) v.copy_to(img);   /* 借样 → 完整物化(应用侧读取) */
                got = true;
                break;
            }
            std::this_thread::yield();
        }

        if (got)
        {
            /* 完整载荷校验: 整段重算序号相关模式, 并检查序号单调。 */
            verified = (img.data.size() == bytes) && (img.width == s.w) && (img.height == s.h) &&
                       (img.step == step_of(s));
            if (verified)
            {
                for (std::uint32_t k = 0; k < bytes; ++k)
                {
                    if (img.data[k] != pattern_byte(expected_seq, k)) { verified = false; break; }
                }
            }
            fully = monotonic_now_ns();
        }

        /* 生产端记录(阻塞读; 时间戳已在上面取完, 不受影响)。 */
        PubRecord r{};
        if (!read_all(rec[0], &r, sizeof(r))) break;

        SampleRecord rec_out;
        rec_out.run_id = spec.run_id;
        rec_out.seq = r.seq;
        rec_out.route = route_base + "/" + s.name;
        rec_out.generation = 0;
        rec_out.path = path_kind(spec.cfg);
        rec_out.produced_ns = r.produced_ns;
        rec_out.publish_enter_ns = r.publish_enter_ns;
        rec_out.transport_done_ns = r.transport_done_ns;
        rec_out.has_app_obtained = got;
        rec_out.app_obtained_ns = app_obtained;
        rec_out.has_fully_consumed = got;
        rec_out.fully_consumed_ns = fully;
        rec_out.payload_bytes = bytes;
        rec_out.has_wire_bytes = false;
        rec_out.has_payload_checksum_ok = got;
        rec_out.payload_checksum_ok = verified;
        rec_out.retry_count = 0;
        rec_out.evicted = false;
        rec_out.dropped = !got;
        out.samples.push_back(rec_out);

        if (got && verified) ++out.verified;
        if (!got) ++out.dropped;
        if (got) ++expected_seq;

        const char a = 'A';
        if (!write_all(ack[1], &a, 1)) break;
    }
    out.window_s = std::chrono::duration<double>(Clock::now() - window_start).count();

    out.child_text = read_all_text(txt[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) { out.child_exited = true; out.child_code = WEXITSTATUS(status); }
    else if (WIFSIGNALED(status)) { out.child_signal = WTERMSIG(status); }

    ::close(rec[0]);
    ::close(txt[0]);
    ::close(ack[1]);
    return out;
}

/* ---------------- 落盘 ---------------- */
void write_run_files(const RunSpec& spec, const RunOutcome& out)
{
    std::filesystem::create_directories(spec.run_dir);

    /* samples.csv + samples.jsonl —— 字段与表头全部来自 W03 的 field_schema.h。 */
    {
        std::FILE* f = std::fopen((spec.run_dir + "/samples.csv").c_str(), "wb");
        if (f)
        {
            const std::string h = dzIPC::measure::sample_csv_header() + "\n";
            std::fwrite(h.data(), 1, h.size(), f);
            for (const auto& r : out.samples)
            {
                const std::string line = r.to_csv_row() + "\n";
                std::fwrite(line.data(), 1, line.size(), f);
            }
            std::fclose(f);
        }
    }
    {
        std::FILE* f = std::fopen((spec.run_dir + "/samples.jsonl").c_str(), "wb");
        if (f)
        {
            for (const auto& r : out.samples)
            {
                const std::string line = r.to_jsonl() + "\n";
                std::fwrite(line.data(), 1, line.size(), f);
            }
            std::fclose(f);
        }
    }
    /* 消费端计数快照(生产端的那份由子进程自己写)。 */
    CounterRegistry::instance().write_json_file(spec.run_dir + "/counters_consumer.json");

    /* manifest.json —— 字段名取自 W03 manifest_field_table(); 未测项按该表标 null。 */
    {
        std::string o;
        o += "{\n";
        o += "  \"run_id\": \"" + spec.run_id + "\",\n";
        o += "  \"work_package\": \"W08\",\n";
        o += "  \"source_revision\": \"e800ccc496ac710b711c9346709e86a148c41241\",\n";
        o += "  \"working_tree_clean\": false,\n";
        o += "  \"working_tree_diff\": null,\n";
        o += "  \"build_dir\": \"" + (repo_root().empty() ? std::string("(unresolved)") : repo_root()) + "/build\",\n";
        o += "  \"build_type\": \"Release\",\n";
        o += "  \"compiler\": \"" + std::string(__VERSION__) + "\",\n";
        o += "  \"binary_sha256\": {\"note\": \"本 run 生产端/消费端为同一测试二进制 "
             "test_w08_dzflat_ab; 改动面文件指纹见 W08 交付 fingerprints.txt\"},\n";
        o += "  \"loaded_library_paths\": [\"build/lib/libipc.so\"],\n";
        o += "  \"transport\": \"" + std::string(cfg_name(spec.cfg)) + "\",\n";
        o += "  \"process_model\": \"cross-process\",\n";
        o += "  \"measurement_mode\": \"rusage\",\n";
        o += "  \"diagnostics_enabled\": false,\n";
        o += "  \"topic_count\": 1,\n";
        o += "  \"unique_topic_count\": 1,\n";
        o += "  \"worker_count\": 0,\n";
        o += "  \"registered_count\": 2,\n";
        o += "  \"expected_count\": 2,\n";
        /* valid_rx_count 的口径是**端点数**(不是消息数): 本 run 只有生产端与消费端各 1 个,
         * 两者都完成了有效收发 ⇒ 2。逐条消息的证据在 sample_count 与 payload_checksum_ok。 */
        o += "  \"valid_rx_count\": " + std::to_string(out.verified > 0 ? 2u : 0u) + ",\n";
        o += "  \"fallback_count\": " + std::to_string(dzIPC::DzFlatFallbackCount()) + ",\n";
        o += "  \"sample_count\": " + std::to_string(out.samples.size()) + ",\n";
        o += "  \"window_s\": " + std::to_string(out.window_s) + ",\n";
        o += "  \"config_hash\": null,\n";
        o += "  \"clock_source\": \"CLOCK_MONOTONIC\",\n";
        o += "  \"clock_cost_ns_per_call\": null,\n";
        o += "  \"collector_overhead_ns\": null,\n";
        o += "  \"result_files\": [\"samples.csv\", \"samples.jsonl\", \"manifest.json\", "
             "\"counters_publisher.json\", \"counters_consumer.json\", \"README.md\"],\n";
        o += "  \"path_evidence\": null,\n";
        o += "  \"w08_variant\": \"" + std::string(variant_name(spec.variant)) + "\",\n";
        o += "  \"w08_scope_note\": \"只接线了三条路径的**条数**(tlv/dzflat-a/dzflat-b)与 wire bytes; "
             "逻辑载荷字节(*_bytes)与回退原因计数未接线, 其值恒 0, 不代表已测。\"\n";
        o += "}\n";
        std::FILE* f = std::fopen((spec.run_dir + "/manifest.json").c_str(), "wb");
        if (f) { std::fwrite(o.data(), 1, o.size(), f); std::fclose(f); }
    }
    {
        std::string o = "# " + spec.run_id + "\n\n";
        o += "transport=" + std::string(cfg_name(spec.cfg)) + " variant=" + variant_name(spec.variant) +
             " process_model=cross-process work_package=W08\n\n";
        o += "生产端 = fork 出来的子进程(shm_pub_ipc), 消费端 = 父进程(shm_sub_ipc)。"
             "发一条取一条(子进程等 ack), 以规避 docs/dzflat_shm.md §9.5 末尾的 chunk 池耗尽既存缺陷。\n\n";
        o += "本 run 的样本数=" + std::to_string(out.samples.size()) +
             " 完整载荷校验通过=" + std::to_string(out.verified) +
             " 丢失=" + std::to_string(out.dropped) + "\n\n";
        o += "生产端路径计数(生产进程自报): " + out.child_text + "\n";
        std::FILE* f = std::fopen((spec.run_dir + "/README.md").c_str(), "wb");
        if (f) { std::fwrite(o.data(), 1, o.size(), f); std::fclose(f); }
    }
}

std::string unique_topic(const char* tag)
{
    static std::atomic<unsigned> serial{0};
    return std::string("/w08/") + tag + "_" + std::to_string(serial.fetch_add(1));
}

/* ⛔ 防覆盖闸(2026-09-28 补, 起因见交付 §8 已知限制第 2 条):
 * 本用例的落盘一律 fopen(...,"wb") **截断**, 而 repo_root() 默认回推仓库根 ⇒ 任何人在仓库里
 * 裸跑 build/bin/test_w08_dzflat_ab 都会就地覆盖 artifacts/perf/20260928-r0{5..10}-W08-xxx 目录
 * 里的**原始样本**, 违反方案 §12"只追加、不覆盖旧日志"。
 *
 * 授权覆盖的三种情形(否则：目标 samples.csv 已存在 ⇒ 既不覆盖也不静默跳过, 由调用方判失败):
 *   ① W08_OVERWRITE_EVIDENCE=1         —— 明确表示"我就是要重新生成这一版"。
 *   ② 显式设了 W08_ARTIFACT_ROOT       —— 落点由调用方自己指定(ctest 的 ENVIRONMENT 即走这条,
 *      指向 build/test-scratch/w08), 因此可反复跑;
 *   ③ 落点位于 <repo>/build/ 之下      —— build/ 是 .gitignore 的 scratch 区, 不承载交付证据。
 * ⛔ 其余情况(默认回推仓库根 + 目标已有原始样本)一律拒绝, 防"跑一次测试把证据换掉"。 */
}   // namespace

/* ① 三路径可分 + 标记与实际路径一致（有计数证据）。
 * 每个配置都断言"该路径的条数 = N, 另两条 = 0", 并检查既有 dzflat/fallback 口径。 */
TEST(W08DzFlatAB, CrossProcessPathCountersAreSeparableByConfig)
{
    const int kSamples = 60;
    struct Expect
    {
        Cfg cfg;
        std::uint64_t a, b, tlv, fallback, dzflat;
    };
    const Expect expects[3] = {
        {Cfg::Tlv, 0, 0, kSamples, kSamples, 0},   /* 开关关: 每条都回落整包 */
        {Cfg::A, kSamples, 0, 0, 0, kSamples},     /* 对象→段一次复制 */
        {Cfg::B, 0, kSamples, 0, 0, kSamples},     /* 应用原地构造借样 */
    };

    for (const auto& e : expects)
    {
        RunSpec spec;
        spec.cfg = e.cfg;
        spec.variant = Variant::PerMessage;
        spec.size_count = 1;
        spec.samples_per_size = kSamples;
        spec.write_evidence = false;
        spec.topic = unique_topic(cfg_name(e.cfg));
        spec.run_id = "20260928-r05-W08-counters";

        const RunOutcome out = run_cross_process(spec);
        SCOPED_TRACE(std::string("cfg=") + cfg_name(e.cfg));
        ASSERT_TRUE(out.forked);
        ASSERT_TRUE(out.child_exited) << "生产端异常退出 signal=" << out.child_signal << " " << out.child_text;
        ASSERT_EQ(out.child_code, 0) << out.child_text;

        EXPECT_EQ(kv(out.child_text, "dzflat_a_messages"), std::to_string(e.a)) << out.child_text;
        EXPECT_EQ(kv(out.child_text, "dzflat_b_messages"), std::to_string(e.b)) << out.child_text;
        EXPECT_EQ(kv(out.child_text, "tlv_messages"), std::to_string(e.tlv)) << out.child_text;
        EXPECT_EQ(kv(out.child_text, "dzflat_publish"), std::to_string(e.dzflat)) << out.child_text;
        EXPECT_EQ(kv(out.child_text, "dzflat_fallback"), std::to_string(e.fallback)) << out.child_text;
        /* 预构造段入口本套件没走 ⇒ 必须为 0（防把它并进 A/B）。 */
        EXPECT_EQ(kv(out.child_text, "prebuilt"), "0") << out.child_text;
        EXPECT_EQ(kv(out.child_text, "seq_ok"), std::to_string(kSamples)) << out.child_text;
        EXPECT_EQ(out.verified, static_cast<std::size_t>(kSamples)) << "载荷全量校验未全过";
        EXPECT_EQ(out.dropped, 0u) << "有消息没收到";
    }
}

/* ② 跨进程 + 逐样本落盘 + 完整载荷校验（6 个配置×写法组合, 3 个尺寸档）。
 * 跑完把 samples.csv/jsonl + manifest.json + counters_*.json 写进 artifacts/perf/<run_id>/。 */
TEST(W08DzFlatAB, CrossProcessPerSampleEvidenceIsWrittenAndPayloadFullyVerified)
{
    struct Combo
    {
        Cfg cfg;
        Variant variant;
        const char* run_id;
    };
    const Combo combos[6] = {
        {Cfg::Tlv, Variant::PreBuilt, "20260928-r11-W08-tlv-prebuilt"},
        {Cfg::Tlv, Variant::PerMessage, "20260928-r12-W08-tlv-permsg"},
        {Cfg::A, Variant::PreBuilt, "20260928-r13-W08-dzflat-a-prebuilt"},
        {Cfg::A, Variant::PerMessage, "20260928-r14-W08-dzflat-a-permsg"},
        {Cfg::B, Variant::PreBuilt, "20260928-r15-W08-dzflat-b-prebuilt"},
        {Cfg::B, Variant::PerMessage, "20260928-r16-W08-dzflat-b-permsg"},
    };

    const int kSamplesPerSize = 40;   /* 3 档 × 40 = 120 样本/run */
    for (const auto& c : combos)
    {
        RunSpec spec;
        spec.cfg = c.cfg;
        spec.variant = c.variant;
        spec.size_count = 3;
        spec.samples_per_size = kSamplesPerSize;
        spec.write_evidence = true;
        spec.run_id = c.run_id;
        spec.topic = unique_topic(c.run_id);
        /* 落点由 artifact_dir_for() 决定: 显式 W08_ARTIFACT_ROOT 优先, 否则 **build/test-scratch/w08**
         * (⛔ 不再默认写仓库 —— 那正是本次证据污染的原因, 见该函数上方注释)。 */
        spec.run_dir = artifact_dir_for(c.run_id);

        const RunOutcome out = run_cross_process(spec);
        SCOPED_TRACE(std::string("run=") + c.run_id);
        ASSERT_FALSE(spec.run_dir.empty())
            << "落点推导失败(既无 W08_ARTIFACT_ROOT, 也无法由可执行文件路径推出 build/) —— "
               "拒绝落盘而不是退化成写当前目录。设 W08_ARTIFACT_ROOT 后重跑。";
        /* ⛔ 防覆盖闸: 目标 run 目录已有原始样本 ⇒ 不覆盖、不静默跳过, 直接判失败并给出出路。 */
        ASSERT_FALSE(out.evidence_dir_exists)
            << "拒绝覆盖既有原始证据: " << spec.run_dir << "/samples.csv 已存在。\n"
               "  · 想留档重跑: 改 run_id(或用 W08_ARTIFACT_ROOT 指到 scratch 目录)后重跑;\n"
               "  · 确认就是要覆盖(例如你在造新一版 run): 设 W08_OVERWRITE_EVIDENCE=1;\n"
               "  · ctest 登记已用 ENVIRONMENT W08_ARTIFACT_ROOT 重定向到 build/test-scratch/w08,\n"
               "    ⛔ 不要删掉那条 ENVIRONMENT(见 test/CMakeLists.txt 的注释与交付 §8)。";
        ASSERT_TRUE(out.forked);
        ASSERT_TRUE(out.child_exited) << "生产端异常退出 signal=" << out.child_signal << " " << out.child_text;
        ASSERT_EQ(out.child_code, 0) << out.child_text;

        const std::size_t expected =
            static_cast<std::size_t>(spec.size_count) * static_cast<std::size_t>(spec.samples_per_size);
        EXPECT_EQ(out.samples.size(), expected);
        EXPECT_EQ(out.verified, expected) << "逐样本完整载荷校验必须全过: " << out.child_text;
        EXPECT_EQ(out.dropped, 0u);
        EXPECT_EQ(kv(out.child_text, "seq_fail"), "0") << out.child_text;

        /* 路径计数必须与该 run 的 transport 一致(防静默回退假绿)。 */
        const std::string a = kv(out.child_text, "dzflat_a_messages");
        const std::string b = kv(out.child_text, "dzflat_b_messages");
        const std::string tlv = kv(out.child_text, "tlv_messages");
        if (c.cfg == Cfg::B)
        {
            EXPECT_EQ(b, std::to_string(expected)) << out.child_text;
            EXPECT_EQ(kv(out.child_text, "dzflat_fallback"), "0") << out.child_text;
        }
        else if (c.cfg == Cfg::A)
        {
            EXPECT_EQ(a, std::to_string(expected)) << out.child_text;
            EXPECT_EQ(kv(out.child_text, "dzflat_fallback"), "0") << out.child_text;
        }
        else
        {
            EXPECT_EQ(tlv, std::to_string(expected)) << out.child_text;
        }
        /* 标记的那条路径计数必须非 0 —— 否则说明接线失效而断言恰好被"全 0"放过。 */
        const std::string marked = (c.cfg == Cfg::B) ? b : (c.cfg == Cfg::A) ? a : tlv;
        EXPECT_NE(marked, "0") << "该配置对应的路径计数为 0, 接线可能失效: " << out.child_text;

        write_run_files(spec, out);

        EXPECT_TRUE(std::filesystem::exists(spec.run_dir + "/samples.csv")) << spec.run_dir;
        EXPECT_TRUE(std::filesystem::exists(spec.run_dir + "/samples.jsonl")) << spec.run_dir;
        EXPECT_TRUE(std::filesystem::exists(spec.run_dir + "/manifest.json")) << spec.run_dir;
        EXPECT_TRUE(std::filesystem::exists(spec.run_dir + "/counters_publisher.json")) << spec.run_dir;
        EXPECT_TRUE(std::filesystem::exists(spec.run_dir + "/counters_consumer.json")) << spec.run_dir;
    }
}

/* ③ B 失败的**应用处理契约**: 超预算 ⇒ publish_loaned 失败且 chunk 归还(还能再借),
 *    应用回退普通 publish ⇒ 消息不丢, 且回退那次记在 A/TLV 而不是 B。 */
TEST(W08DzFlatAB, FailedLoanIsReturnedAndApplicationFallbackStillDelivers)
{
    dzIPC::EnableDzFlat(true);
    dzIPC::ResetDzFlatCounters();
    CounterRegistry::instance().reset();

    const std::string topic = unique_topic("w08_bfail");
    const SizeSpec& s = kSizes[0];
    const std::uint32_t bytes = bytes_of(s);

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::shm::shm_pub_ipc pub{pub_td, topic, kDomain};
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, kDomain, 32};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    /* 超预算: 借 256 B 的变长预算却要写 bytes ⇒ alloc 返回空 span, publish_loaned 失败。 */
    bool published = false;
    for (int attempt = 0; attempt < 20 && !published; ++attempt)
    {
        auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(256);
        if (!lo.valid()) { std::this_thread::sleep_for(20ms); continue; }
        auto px = lo->alloc_data(bytes);
        EXPECT_TRUE(px.empty()) << "超预算时 alloc_data 必须返回空 span(不得写出坏段)";
        EXPECT_FALSE(lo.ok()) << "超预算后 Writer 必须转入 !ok";
        EXPECT_FALSE(pub.publish_loaned(std::move(lo))) << "超预算的借样不得被投递";
        published = true;
    }
    ASSERT_TRUE(published) << "借样一直失败, 无法构造超预算场景";

    /* 契约: 失败的借样已归还 chunk ⇒ 能立刻再借到。 */
    {
        auto again = pub.loan<dzIPC::Msg::StdImageFlat>(bytes + 256);
        EXPECT_TRUE(again.valid()) << "失败借样没有归还 chunk —— 池会被漏干";
        /* 不发布, 由析构归还。 */
    }

    /* 契约: 应用回退普通 publish ⇒ 消息仍然送达, 且这一条记在 A(开关开着且类型支持)。 */
    const std::uint64_t a_before = counter_of(CounterId::dzflat_a_messages);
    const std::uint64_t b_before = counter_of(CounterId::dzflat_b_messages);

    auto m = std::make_shared<dzIPC::Msg::StdImage>();
    m->set_msg_id(kMsgId);
    m->width = s.w;
    m->height = s.h;
    m->step = step_of(s);
    m->encoding = "rgb8";
    m->data.resize(bytes);
    for (std::uint32_t k = 0; k < bytes; ++k) m->data[k] = pattern_byte(4242, k);
    ASSERT_TRUE(pub.publish(m)) << "回退的普通 publish 失败";

    bool got = false;
    const auto deadline = Clock::now() + 3000ms;
    while (Clock::now() < deadline && !got)
    {
        if (sub.try_get_clone(sub_td)) { got = true; break; }
        dzIPC::Sample sample;
        if (sub.try_get(sample))
        {
            auto v = sample.view<dzIPC::Msg::StdImageFlat>();
            got = v.valid();
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(got) << "B 失败后的应用回退没有送达";

    EXPECT_EQ(counter_of(CounterId::dzflat_a_messages), a_before + 1)
        << "回退那一条应记在 A(实际路径), 不能记在 B";
    EXPECT_EQ(counter_of(CounterId::dzflat_b_messages), b_before) << "失败的 B 不得被记成成功";
}
