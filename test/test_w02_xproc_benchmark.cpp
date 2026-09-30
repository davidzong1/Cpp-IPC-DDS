/* W02 统一跨进程基准 · 小规模正确性用例
 * ============================================================================
 * 交付依据：方案 §4 W02「交付：… 小规模正确性用例」+ 验收三条
 *   (a) 跨进程身份可核验；
 *   (b) 计时边界一致（三条边界各自的定义不得互换）；
 *   (c) 异常消息与发送失败不被静默排除；
 *   (d) 路径计数单写入者（D-17）；
 *   (e) 未测项用空字段而非 0（W02-F2 / W03 §2.1 的 CSV null 约定）。
 *
 * 分两层：
 *   ① HarnessSelfCheck —— 纯函数层（载荷形态、序号相关模式、topic 名消毒、
 *      时间戳头槽位）。这些是"父/子两侧算同一件事"的地基，跑几毫秒就能守；
 *   ② TinyCrossProcessRun —— 真起一次跨进程运行（TLV / 64 B / 0.3 s / 200 Hz），
 *      然后**从落盘产物反证**上面三条验收：身份括号、边界存在性与单调性、
 *      失败/丢失/重复计数与样本表自洽。
 *
 * 为什么用"跑一遍再看产物"而不是断言内部变量：基准的产物就是它唯一的对外契约
 * （方案 §12 的逐样本字段与 manifest 字段）。断言产物等于同时守住了格式与语义 ——
 * 内部重构只要产物不变就仍然通过。
 *
 * 依赖：基准可执行文件路径由 CMake 以 W02_BENCH_BIN 注入（见 test/CMakeLists.txt）。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "w02_pattern.h"

#ifndef W02_BENCH_BIN
#  define W02_BENCH_BIN "build/bin/xproc_benchmark"
#endif

namespace {

using namespace w02;

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return std::string();
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

/* 极简 CSV 读取（基准的样本表没有引号/逗号转义，直接按逗号切）。 */
std::vector<std::vector<std::string>> read_csv(const std::string& path)
{
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(path);
    if (!f) return rows;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty()) continue;
        std::istringstream is(line);
        std::string cell;
        std::vector<std::string> row;
        while (std::getline(is, cell, ',')) row.push_back(cell);
        rows.push_back(std::move(row));
    }
    return rows;
}

/* 从 results.json 里取某个 case 的字段值（不做完整 JSON 解析：产物是本文件要守的
 * 契约的一部分，键名固定；用字符串定位足以在破坏时立刻失败）。 */
std::string json_case_field(const std::string& json, const std::string& case_id,
                            const std::string& key)
{
    const std::size_t c = json.find("\"" + case_id + "\"");
    if (c == std::string::npos) return std::string();
    const std::size_t k = json.find("\"" + key + "\"", c);
    if (k == std::string::npos) return std::string();
    const std::size_t colon = json.find(':', k);
    if (colon == std::string::npos) return std::string();
    std::size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    std::size_t end = i;
    while (end < json.size() && json[end] != ',' && json[end] != '\n' && json[end] != '}') ++end;
    return json.substr(i, end - i);
}

}   // namespace

/* ======================================================================== */
/* ① 纯函数层                                                                */
/* ======================================================================== */

/* 载荷形态：字节数自洽 + 与实际生成的三段长度一致。
 * 这一条守的是"同一逻辑载荷"：A/TLV/DDS 三条路径用的是同一份 Shape，任一取整口径
 * 漂移都会让三档实际搬的字节数不同，比值就不可比了。 */
TEST(W02Harness, ShapeIsSelfConsistent)
{
    for (u64 target : {64ull, 1024ull, 65536ull, 1048576ull})
    {
        const Shape s = compute_shape(target);
        // 应用逻辑载荷 = 8*n1 + 4*n2 + str_len*n3 + 1(bool)
        EXPECT_EQ(s.bytes(), s.n1 * 8u + s.n2 * 4u + s.n3 * s.str_len + 1u) << "target=" << target;
        EXPECT_GT(s.n1, 0u) << "target=" << target;
        EXPECT_GT(s.n2, 0u) << "target=" << target;
        EXPECT_GT(s.n3, 0u) << "target=" << target;
        EXPECT_GT(s.str_len, 0u) << "target=" << target;
        // 时间戳头是独立于应用字节的固定 32 B（方案 §12 口径分开列）。
        EXPECT_EQ(kHdrBytes, 32u);
        EXPECT_EQ(s.dzipc_data2_size(), kHdrSlots + s.n2);
        // 目标指的是**应用逻辑载荷**（头另计）：实际值应当贴近目标，而不是它的一半。
        EXPECT_GE(s.bytes(), target - 8) << "target=" << target;
        EXPECT_LE(s.bytes(), target + 8) << "target=" << target;
    }
}

/* 序号相关模式：值随 (seq, i) 变化 —— 这是"旧帧/重复帧可被逐元素抓出来"的前提。
 * 若模式与 seq 无关（固定载荷），消费侧的 full 校验就失去意义（方案 §10.7 明写禁止）。 */
TEST(W02Harness, PayloadPatternIsSequenceDependent)
{
    const Shape s = compute_shape(1024);
    // 同一位置、不同 seq 必须不同（抽 8 个位置各验一次）
    for (u32 i = 0; i < 8 && i < s.n1; ++i)
    {
        EXPECT_NE(pat_double(1000, i), pat_double(1001, i)) << "i=" << i;
    }
    // 同一 seq、不同位置必须不同（否则逐元素比对抓不到段内错位）
    EXPECT_NE(pat_double(7, 0), pat_double(7, 1));
    EXPECT_NE(pat_int(7, 0), pat_int(7, 1));
    // 确定性：同一输入两次调用必须相同（否则生产/消费两侧永远对不上）
    EXPECT_EQ(pat_double(42, 3), pat_double(42, 3));
    EXPECT_EQ(pat_int(42, 3), pat_int(42, 3));
    // 字符串：长度精确，且随 seq 变化
    char a[16], b[16], c[16];
    pat_string(1000, 2, 8, a);
    pat_string(1000, 2, 8, b);
    pat_string(1001, 2, 8, c);
    EXPECT_EQ(std::string(a, 8), std::string(b, 8));
    EXPECT_NE(std::string(a, 8), std::string(c, 8));
}

/* 时间戳头是"不可混用结束点"的落点：头里**只有** produced/enter，没有 transport_done。
 * transport_done 由发布进程本地记录后按 seq 合并 —— 它不可能随同一条消息送出去。
 * 这条断言守的是那个设计决定不要在后续改动里被"顺手加进去"。 */
TEST(W02Harness, HeaderCarriesOnlyPreSendTimestamps)
{
    EXPECT_EQ(kHdrSlots, 8u);
    EXPECT_EQ(kSlotProducedLo, 4u);
    EXPECT_EQ(kSlotEnterLo, 6u);
    // 8 个槽位之后就是应用负载区（没有给"发布返回时刻"留位置）
    EXPECT_EQ(kHdrBytes, kHdrSlots * 4u);
}

/* ======================================================================== */
/* ② 真起一次跨进程运行，从产物反证三条验收                                   */
/* ======================================================================== */

/* 自检输出目录：放在基准二进制所在目录旁边，不污染源码树。 */
static std::string selftest_dir(const std::string& suffix)
{
    const std::string bin = W02_BENCH_BIN;
    const std::size_t slash = bin.find_last_of('/');
    const std::string d = (slash == std::string::npos) ? std::string(".") : bin.substr(0, slash);
    return d + "/w02_selftest_out" + suffix;
}

class TinyCrossProcessRun : public ::testing::Test
{
protected:
    static std::string out_dir() { return selftest_dir(""); }

    static void SetUpTestSuite()
    {
        std::system(("rm -rf " + out_dir()).c_str());
        const std::string cmd = std::string(W02_BENCH_BIN)
            + " --path=tlv --payload=64 --workload=full --duration=0.3 --warmup=0.1"
              " --rate=200 --sample-limit=500 --workload=full --out-dir=" + out_dir()
            + " --run-id=w02-selftest > " + out_dir() + ".log 2>&1";
        const int rc = std::system(cmd.c_str());
        ASSERT_EQ(rc, 0) << "基准未通过自检运行；见 " << out_dir() << ".log";
    }
};

/* 验收 (a)：跨进程身份可核验。
 * 判据取自 verdict.md 里的 identity 行（由父进程括号 + pid + starttime 组成），
 * 不做"打印了就算过"的弱断言：必须同时出现两个不同的 pid 与两个括号。 */
TEST_F(TinyCrossProcessRun, IdentityIsVerifiable)
{
    const std::string verdict = read_file(out_dir() + "/verdict.md");
    ASSERT_FALSE(verdict.empty()) << "verdict.md 缺失";
    ASSERT_TRUE(contains(verdict, "identity:")) << "verdict.md 未登记跨进程身份";
    ASSERT_TRUE(contains(verdict, "pub_pid=")) << verdict.substr(0, 400);
    ASSERT_TRUE(contains(verdict, "sub_pid=")) << verdict.substr(0, 400);
    ASSERT_TRUE(contains(verdict, "pub_starttime_ticks=")) << "缺 starttime（无法防 pid 复用）";
    ASSERT_TRUE(contains(verdict, "sub_lib=")) << "缺角色进程实际加载库路径";

    const std::string json = read_file(out_dir() + "/results.json");
    ASSERT_FALSE(json.empty());
    EXPECT_TRUE(contains(json, "\"process_model\": \"cross-process\""));
    EXPECT_TRUE(contains(json, "\"case_ok\": true")) << "自检用例未通过";
}

/* 验收 (b)：计时边界一致 —— 三条边界在产物里都存在、且各自的派生关系成立。
 * 派生关系来自 samples.csv 的列定义（方案 §12）：transport = done-enter、
 * delivery = app-done、app_read = fully-app、e2e = fully-produced。 */
TEST_F(TinyCrossProcessRun, TimingBoundariesAreConsistent)
{
    const auto rows = read_csv(out_dir() + "/samples/r0_tlv_full_blocking_64B_200Hz.samples.csv");
    ASSERT_GT(rows.size(), 2u) << "合并后的样本表为空";
    const auto& header = rows[0];
    auto col = [&](const char* name) -> std::size_t {
        for (std::size_t i = 0; i < header.size(); ++i)
            if (header[i] == name) return i;
        return header.size();
    };
    const std::size_t c_prod = col("produced_ns");
    const std::size_t c_enter = col("publish_enter_ns");
    const std::size_t c_done = col("transport_done_ns");
    const std::size_t c_app = col("app_obtained_ns");
    const std::size_t c_fully = col("fully_consumed_ns");
    const std::size_t c_tx = col("transport_ns");
    const std::size_t c_dv = col("delivery_ns");
    const std::size_t c_rd = col("app_read_ns");
    const std::size_t c_e2e = col("e2e_ns");
    ASSERT_LT(c_e2e, header.size()) << "样本表缺 e2e_ns 列";

    std::size_t checked = 0;
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        const auto& row = rows[r];
        if (row.size() <= c_e2e) continue;
        const u64 prod = std::strtoull(row[c_prod].c_str(), nullptr, 10);
        const u64 enter = std::strtoull(row[c_enter].c_str(), nullptr, 10);
        const u64 done = std::strtoull(row[c_done].c_str(), nullptr, 10);
        ASSERT_FALSE(row[c_app].empty()) << "app_obtained_ns 为空(该边界无样本)";
        ASSERT_FALSE(row[c_fully].empty()) << "fully_consumed_ns 为空(该边界无样本)";
        const u64 app = std::strtoull(row[c_app].c_str(), nullptr, 10);
        const u64 fully = std::strtoull(row[c_fully].c_str(), nullptr, 10);

        // 生产端顺序：生成数据 → 发布入口 → 传输完成
        EXPECT_LE(prod, enter) << "seq=" << row[1];
        EXPECT_LE(enter, done) << "seq=" << row[1];
        // 消费端顺序：应用获得 → 完整消费结束
        EXPECT_LE(app, fully) << "seq=" << row[1];
        // 派生列必须与绝对时刻一致（禁止用别的结束点冒充）
        EXPECT_EQ(std::strtoull(row[c_tx].c_str(), nullptr, 10), done - enter) << "seq=" << row[1];
        EXPECT_EQ(std::strtoull(row[c_dv].c_str(), nullptr, 10), app - done) << "seq=" << row[1];
        EXPECT_EQ(std::strtoull(row[c_rd].c_str(), nullptr, 10), fully - app) << "seq=" << row[1];
        EXPECT_EQ(std::strtoull(row[c_e2e].c_str(), nullptr, 10), fully - prod) << "seq=" << row[1];
        ++checked;
    }
    EXPECT_GT(checked, 0u) << "一条样本都没校验到";
}

/* 验收 (d)：**路径计数只有一个写入者**（D-17 回归闸）。

背景：`*_messages` 曾被传输层钩子（实际路径）与 harness（期望路径）双写，读方拿到约 2×
条数、且在回退场景下把"期望档"记成"实际档"，直接威胁 W03 `path_evidence` 的 confirmed
判定（方案 §13.3）。本用例把它做成**每次 ctest 都会跑**的判据：
  ① `counters.json:counter_scope.path_message_counts.harness_writes == false`（自证材料在位）；
  ② 恒等式 `family == pub_dzflat + pub_fallback` 成立（允许进程退出前最后几条钩子计数
     未被读窗口覆盖的小残差，阈值取 8；双写会带来 ~sent_ok 量级的偏差）。
实测：修复前残差为 sent_ok（1000 量级），修复后 ≤3。 */
TEST_F(TinyCrossProcessRun, PathCountersHaveSingleWriter)
{
    const std::string counters = read_file(out_dir() + "/counters.json");
    ASSERT_FALSE(counters.empty()) << "counters.json 缺失";
    ASSERT_TRUE(contains(counters, "\"counter_scope\""))
        << "counters.json 缺 counter_scope（写入者/口径自证材料）";
    ASSERT_TRUE(contains(counters, "\"harness_writes\": false"))
        << "path_message_counts 未声明 harness 不写 *_messages";

    const auto rows = read_csv(out_dir() + "/samples/r0_tlv_full_blocking_64B_200Hz.pub_raw.csv.counters.json");
    (void)rows;
    /* 角色进程的 counters 是"计数器快照"JSON，不是 CSV；这里直接按文本取三个 family 值。 */
    const std::string pc = read_file(out_dir() + "/samples/r0_tlv_full_blocking_64B_200Hz.pub_raw.csv.counters.json");
    ASSERT_FALSE(pc.empty()) << "角色进程 counters.json 缺失";
    auto value_of = [&](const char* key) -> long long {
        const std::string needle = std::string("\"") + key + "\": ";
        const std::size_t p = pc.find(needle);
        if (p == std::string::npos) return -1;
        return std::strtoll(pc.c_str() + p + needle.size(), nullptr, 10);
    };
    const long long tlv = value_of("tlv_messages");
    const long long a = value_of("dzflat_a_messages");
    const long long b = value_of("dzflat_b_messages");
    ASSERT_GE(tlv, 0) << "counters.json 里取不到 tlv_messages";
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    /* summary.csv 取列：用表头定位列号后按行切分（不要手算逗号偏移，容易错位）。 */
    const auto summary_rows = read_csv(out_dir() + "/summary.csv");
    ASSERT_GT(summary_rows.size(), 1u) << "summary.csv 无数据行";
    const auto& shdr = summary_rows[0];
    const auto& srow = summary_rows[1];
    auto col_of = [&](const char* key) -> long long {
        for (std::size_t i = 0; i < shdr.size(); ++i)
            if (shdr[i] == key) return static_cast<long long>(i);
        return -1;
    };
    const long long i_dz = col_of("pub_dzflat");
    const long long i_fb = col_of("pub_fallback");
    ASSERT_GE(i_dz, 0) << "summary.csv 缺列 pub_dzflat";
    ASSERT_GE(i_fb, 0) << "summary.csv 缺列 pub_fallback";
    ASSERT_LT(static_cast<std::size_t>(i_dz), srow.size());
    ASSERT_LT(static_cast<std::size_t>(i_fb), srow.size());
    const long long dz = std::strtoll(srow[static_cast<std::size_t>(i_dz)].c_str(), nullptr, 10);
    const long long fb = std::strtoll(srow[static_cast<std::size_t>(i_fb)].c_str(), nullptr, 10);

    const long long family = tlv + a + b;
    EXPECT_LE(std::llabs(family - (dz + fb)), 8)
        << "family=" << family << " 与 pub_dzflat+pub_fallback=" << (dz + fb)
        << " 相差 " << (family - (dz + fb)) << " —— 疑似 *_messages 又被第二个写入者记账（D-17 回归）";
}

/* 验收 (f)：**失败量进判定 + 阈值入 manifest**（t47/W02-F3 回归闸）。

背景：修前 `late`/`backlog`/`send_blocked`/`abnormal` 只落列、`bad_header` 连列都没有
⇒ 17/75 case 的 71 次迟发可以全部不进判定而 `case_ok` 全 True（「用仅落列实现静默排除」）。
本闸把它做成语义判据：
  ① `summary.csv` 有 `bad_header` 与 5 个 gate 布尔列；
  ② 逐行「gate=0 ⇒ `failure_reasons` 非空」（判定逻辑唯一性）；
  ③ `manifest.failure_thresholds` 六个阈值 + `cli_overridden` 在位，且 `gates_in_judgement`
     明确列出 5 个 gate 的**触发计数**（不是"写了不判"）。 */
TEST_F(TinyCrossProcessRun, FailureCountersAreJudged)
{
    const auto rows = read_csv(out_dir() + "/summary.csv");
    ASSERT_GT(rows.size(), 1u);
    const auto& hdr = rows[0];
    auto idx_of = [&](const char* c) -> long long {
        for (std::size_t i = 0; i < hdr.size(); ++i) if (hdr[i] == c) return static_cast<long long>(i);
        return -1;
    };
    const char* gates[] = {"late_ok", "backlog_ok", "send_blocked_ok", "abnormal_ok", "bad_header_ok"};
    long long gi[5];
    for (int i = 0; i < 5; ++i)
    {
        gi[i] = idx_of(gates[i]);
        EXPECT_GE(gi[i], 0) << "summary.csv 缺 gate 列 " << gates[i];
    }
    EXPECT_GE(idx_of("bad_header"), 0) << "summary.csv 缺 bad_header 列（t47 补列）";
    const long long i_rs = idx_of("failure_reasons");
    ASSERT_GE(i_rs, 0);
    long long judged = 0;
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        if (rows[r].size() <= static_cast<std::size_t>(i_rs)) continue;
        for (int i = 0; i < 5; ++i)
        {
            if (gi[i] < 0 || rows[r].size() <= static_cast<std::size_t>(gi[i])) continue;
            if (rows[r][static_cast<std::size_t>(gi[i])] == "0")
            {
                EXPECT_FALSE(rows[r][static_cast<std::size_t>(i_rs)].empty())
                    << "第 " << r << " 行 " << gates[i] << "=0 但 failure_reasons 为空（判定逻辑不一致）";
                ++judged;
            }
        }
    }
    (void)judged;

    const std::string man = read_file(out_dir() + "/manifest.json");
    ASSERT_FALSE(man.empty());
    for (const char* k : {"late_abs_max", "late_rate_max", "backlog_max_periods", "send_blocked_max",
                          "abnormal_max", "bad_header_max", "cli_overridden", "gates_in_judgement"})
        EXPECT_TRUE(contains(man, k)) << "manifest.failure_thresholds 缺 " << k;
    EXPECT_TRUE(contains(man, "\"current_run_authority\"")) << "manifest 缺现行 run 纪律（t47/F0）";
    EXPECT_TRUE(contains(man, "\"single_judgement_logic\"")) << "manifest 缺判定逻辑唯一性纪律";
    EXPECT_TRUE(contains(man, "\"zero_value_three_states\"")) << "manifest 缺零值三态纪律";
}

/* 验收 (e)：**未测项写空字段，不写 0**（D-19 / W03 §2.1 回归闸）。

背景：DDS 档不采集 wire 字节，早先 `samples.csv` 的 `wire_bytes` 对这些行写 `0`
（27,000 行），而 summary/results 写 null —— 同一次运行里两种表示并存；按 samples
聚合的下游会把"未采集"读成"wire=0 = 零拷贝"，正是方案 §10.7/§13.3 要防的方向性误读。
本用例断言三层表示一致：CSV 空字段 / results.json `null` / summary 空字段 + 来源串。
自检运行只跑 TLV 档，故这里只能断言"已采集档写数值"，同时用 `wire_bytes_source`
的存在性守住"来源必须显式"这条更一般的要求。 */
TEST_F(TinyCrossProcessRun, UnmeasuredValuesAreBlankNotZero)
{
    const auto rows = read_csv(out_dir() + "/samples/r0_tlv_full_blocking_64B_200Hz.samples.csv");
    ASSERT_GT(rows.size(), 1u);
    const auto& hdr = rows[0];
    std::size_t c_wire = hdr.size(), c_have = hdr.size();
    for (std::size_t i = 0; i < hdr.size(); ++i)
    {
        if (hdr[i] == "wire_bytes") c_wire = i;
    }
    ASSERT_LT(c_wire, hdr.size()) << "样本表缺 wire_bytes 列";

    std::size_t numeric = 0;
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        if (rows[r].size() <= c_wire) continue;
        if (!rows[r][c_wire].empty()) ++numeric;
    }
    /* TLV 档 wire 字节是实测值 ⇒ 必须**非空**；若实现退化成"一律写 0"，这里仍会通过，
     * 所以再加一道：数值不得为 0（本仓 TLV wire 恒 > 0）。 */
    EXPECT_EQ(numeric, rows.size() - 1) << "TLV 档 wire_bytes 出现空字段（实测值不应为空）";
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        if (rows[r].size() <= c_wire || rows[r][c_wire].empty()) continue;
        EXPECT_NE(rows[r][c_wire], "0") << "TLV 档 wire_bytes 为 0（疑似写占位值而非实测）";
    }

    /* summary 的 wire 字节与来源必须成对出现：有数值就得有来源串。 */
    const auto srows = read_csv(out_dir() + "/summary.csv");
    ASSERT_GT(srows.size(), 1u);
    const auto& sh = srows[0];
    std::size_t i_wire = sh.size(), i_src = sh.size();
    for (std::size_t i = 0; i < sh.size(); ++i)
    {
        if (sh[i] == "wire_bytes_per_msg") i_wire = i;
        if (sh[i] == "wire_bytes_source") i_src = i;
    }
    ASSERT_LT(i_wire, sh.size());
    ASSERT_LT(i_src, sh.size());
    for (std::size_t r = 1; r < srows.size(); ++r)
    {
        if (srows[r].size() <= i_src) continue;
        if (!srows[r][i_wire].empty())
            EXPECT_FALSE(srows[r][i_src].empty())
                << "summary 第 " << r << " 行有 wire 数值但无来源说明";
    }
    (void)c_have;
}

/* 验收 (e2)：**DDS 档的"未采集"必须是空字段/None，不能是 0**（W02-F2 的回归闸）。

与上一条的区别：上一条只守"已采集档写数值"，这条真起一次 CycloneDDS 运行
（`--path=cyc-udp`，SharedMemory=off ⇒ **不需要 RouDi**）来守"未采集档写空"。
三层必须一致：`samples.csv:wire_bytes` 空、`results.json` 为 `null`、
`summary.csv` 空 + `wire_bytes_source` 说明来源。

若本机构建未启用 CycloneDDS（`-DW02_WITH_DDS` 缺省时找不到 libddsc），基准会拒绝
该档 ⇒ 本用例 GTEST_SKIP，不制造假红（跳过原因写进日志）。 */
TEST(W02DdsNullSemantics, UncollectedWireBytesAreNullNotZero)
{
    const std::string dir = selftest_dir("_dds");
    std::system(("rm -rf " + dir).c_str());
    const std::string cmd = std::string(W02_BENCH_BIN)
        + " --path=cyc-udp --payload=64 --workload=full --duration=0.3 --warmup=0.1"
          " --rate=200 --sample-limit=500 --out-dir=" + dir
        + " --run-id=w02-selftest-dds > " + dir + ".log 2>&1";
    if (std::system(cmd.c_str()) != 0)
    {
        GTEST_SKIP() << "CycloneDDS 档不可用（本机构建未启用或 DDS 初始化失败）；见 " << dir << ".log";
    }

    const auto rows = read_csv(dir + "/samples/r0_cyclonedds-udp_full_blocking_64B_200Hz.samples.csv");
    ASSERT_GT(rows.size(), 1u) << "DDS 档样本表为空";
    const auto& hdr = rows[0];
    std::size_t c_wire = hdr.size();
    for (std::size_t i = 0; i < hdr.size(); ++i) if (hdr[i] == "wire_bytes") c_wire = i;
    ASSERT_LT(c_wire, hdr.size()) << "样本表缺 wire_bytes 列";
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        if (rows[r].size() <= c_wire) continue;
        EXPECT_TRUE(rows[r][c_wire].empty())
            << "DDS 档 wire_bytes 写成了 '" << rows[r][c_wire]
            << "'（第 " << r << " 行）—— 未采集必须写空字段，不可写 0 冒充（W03 §2.1）";
    }

    const std::string res = read_file(dir + "/results.json");
    ASSERT_FALSE(res.empty());
    EXPECT_TRUE(contains(res, "\"wire_bytes_per_msg\": null"))
        << "results.json 的 DDS 档 wire_bytes_per_msg 不是 null";
    EXPECT_FALSE(contains(res, "\"wire_bytes_per_msg\": 0"))
        << "results.json 出现 wire_bytes_per_msg=0（0 = 实测 0 字节，会被误读为零拷贝）";

    const auto srows = read_csv(dir + "/summary.csv");
    ASSERT_GT(srows.size(), 1u);
    const auto& sh = srows[0];
    std::size_t i_wire = sh.size(), i_src = sh.size();
    for (std::size_t i = 0; i < sh.size(); ++i)
    {
        if (sh[i] == "wire_bytes_per_msg") i_wire = i;
        if (sh[i] == "wire_bytes_source") i_src = i;
    }
    ASSERT_LT(i_wire, sh.size());
    ASSERT_LT(i_src, sh.size());
    EXPECT_TRUE(srows[1][i_wire].empty())
        << "summary 的 DDS 档 wire_bytes_per_msg 应为空字段，实际为 '" << srows[1][i_wire] << "'";
    EXPECT_TRUE(contains(srows[1][i_src], "not_collected"))
        << "wire_bytes_source 未标明未采集，取值为: " << srows[1][i_src];
}

/* 验收 (c)：异常消息与发送失败不被静默排除。
 *   · 计数必须在产物里（send_failed / missing / duplicate / out_of_order 列存在）；
 *   · 样本表条数必须与"传输成功且订阅侧合并"的条数自洽（不能偷偷丢行）;
 *   · 载荷校验结果必须是显式 true/false，不能留空。 */
TEST_F(TinyCrossProcessRun, FailuresAndLossesAreAccounted)
{
    const std::string json = read_file(out_dir() + "/results.json");
    ASSERT_FALSE(json.empty());
    EXPECT_TRUE(contains(json, "\"send_failed\"")) << "发送失败未计数";
    EXPECT_TRUE(contains(json, "\"missing\"")) << "丢失未计数";
    EXPECT_TRUE(contains(json, "\"duplicate\"")) << "重复未计数";
    EXPECT_TRUE(contains(json, "\"out_of_order\"")) << "乱序未计数";
    EXPECT_TRUE(contains(json, "\"checksum_bad\"")) << "校验失败未计数";

    const auto rows = read_csv(out_dir() + "/samples/r0_tlv_full_blocking_64B_200Hz.samples.csv");
    ASSERT_GT(rows.size(), 1u);
    std::size_t ok_rows = 0;
    for (std::size_t r = 1; r < rows.size(); ++r)
    {
        const auto& row = rows[r];
        ASSERT_GT(row.size(), 18u) << "样本行列数不足";
        // payload_checksum_ok 必须显式为 1 或 0（null 会在 CSV 里表现为空字段）
        EXPECT_FALSE(row[16].empty()) << "第 " << r << " 行 payload_checksum_ok 为空（未测项应写明原因）";
        if (!row[16].empty()) { EXPECT_EQ(row[16], "1") << "第 " << r << " 行载荷校验未通过"; ++ok_rows; }
        // dropped 列必须显式
        EXPECT_FALSE(row[18].empty()) << "第 " << r << " 行 dropped 未标";
    }
    EXPECT_GT(ok_rows, 0u);
    EXPECT_TRUE(contains(json, "\"missing\": 0")) << "丢失不为 0（自检运行应当无误）";
    EXPECT_TRUE(contains(json, "\"checksum_bad\": 0")) << "存在载荷校验失败";
}
