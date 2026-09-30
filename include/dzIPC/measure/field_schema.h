#pragma once
/* W03 测量口径 · 字段定义与 JSON/CSV schema（单一事实来源）
 * ============================================================================
 * 交付依据：§4 W03「交付：采集器、**字段定义**、路径证据、观测开销对照」；
 * §12 的样本字段与 `manifest.json` 字段清单。
 *
 * 本文件是字段定义的**唯一事实来源**：文档表格、CSV 表头、JSON schema 全部由
 * 这里的表生成，避免"文档写了一套、代码输出另一套"。任何新增字段只允许追加。
 *
 * 三类字段：
 *   A. 逐样本字段（samples.csv / samples.jsonl）—— 每条消息一行，见 sample_field_table()
 *   B. 汇总/计数与相位字段（counters.json / idle.json）—— 见 counters.h / idle_observer.h
 *   C. 运行元数据（manifest.json / environment.json / topology.json）—— 见 manifest_field_table()
 *
 * 缺失值口径：未测项一律输出 JSON `null` 并在 verdict.md 标注"未确认"；
 * CSV 用空字段表示 null（不允许写 0 冒充"没有延迟"）。
 *
 * ============================================================================
 * 【§2.1 登记门禁】本 schema 的**登记**与**测量所有权**是两件事（⛔ 勿混）：
 *
 *   1. **测量字段（measurable）**：由 W03 定义并拥有（时基、时间戳位点、计数、
 *      相位、空闲状态、路径证据、开销）。它们的 `owner = "W03"`。
 *   2. **运行编排字段（run_orchestration）**：由**各工作包**（W02/W06/W10…）定义与拥有，
 *      **不是**测量字段。W03 只在 schema 里**登记其结构**（名称/类型/单位/可空/语义来源），
 *      并**逐条标注 `owner`**，`registered_by = "W03"` 与之并列。
 *      ⇒ 登记 ≠ W03 拥有其语义；语义变更由 `owner` 负责，W03 只同步登记。
 *   3. **判定字段（judgement_artifact）**：`0/1` 或 `false/true` 的**判定结果**（如
 *      W02 的 gate 列）。它们是"**已采集的判定**"，**⛔ 不适用"未测写空字段"纪律**：
 *      未触发也照写 0/1，`0` 的含义由字段名确定（见 `zero_value_three_states`）。
 *
 *   机器可读声明见 `schema_document_json()` 的
 *   `structural_vs_measurement_registration` 段；逐字段归属见 `run_level_fields[]`
 *   的 `owner` / `registered_by` / `semantics_source`。
 * ============================================================================
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dzIPC/measure/counters.h"
#include "dzIPC/measure/idle_observer.h"
#include "dzIPC/measure/monotonic_clock.h"
#include "dzIPC/measure/path_evidence.h"

namespace dzIPC {
namespace measure {

struct FieldDef
{
    const char* name;
    const char* type;        ///< "u64" | "u32" | "f64" | "string" | "bool" | "enum" | "array" | "object"
    const char* unit;        ///< "ns" | "us" | "byte" | "count" | "core" | "-"
    const char* category;
    const char* description;
    bool        nullable;    ///< true = 允许 null（未测），CSV 表现为空字段
};

/* ---- A. 逐样本字段（§12 固定样本字段 + 本工作包补的派生量） ---- */
inline const std::vector<FieldDef>& sample_field_table()
{
    static const std::vector<FieldDef> kFields = {
        {"run_id",             "string", "-",     "meta",    "运行编号, 例如 20260928-r01-W02", false},
        {"seq",                "u64",    "count", "identity","生产序号, 逐 route 单调递增", false},
        {"route",              "string", "-",     "identity","route 标识 (topic/domain/endpoint)", false},
        {"generation",         "u32",    "count", "identity","route generation, 用于识别旧 generation 投递", false},
        {"path",               "enum",   "-",     "path",    "tlv | dzflat-a | dzflat-b | cyclonedds-iox | compat-fallback", false},
        {"produced_ns",        "u64",    "ns",    "ts",      "生产端: 生成本条消息数据之前 (CLOCK_MONOTONIC)", false},
        {"publish_enter_ns",   "u64",    "ns",    "ts",      "生产端: 发布 API 入口", false},
        {"transport_done_ns",  "u64",    "ns",    "ts",      "生产端: 发布 API 返回 (传输完成)", false},
        {"app_obtained_ns",    "u64",    "ns",    "ts",      "订阅端: 应用获得视图/对象", true},
        {"fully_consumed_ns",  "u64",    "ns",    "ts",      "订阅端: 完整载荷遍历与校验结束", true},
        {"transport_ns",       "u64",    "ns",    "derived", "= transport_done_ns - publish_enter_ns (发布路径)", true},
        {"delivery_ns",        "u64",    "ns",    "derived", "= app_obtained_ns - transport_done_ns (通知与交付)", true},
        {"app_read_ns",        "u64",    "ns",    "derived", "= fully_consumed_ns - app_obtained_ns (应用完整读取)", true},
        {"e2e_ns",             "u64",    "ns",    "derived", "= fully_consumed_ns - produced_ns (生产到消费)", true},
        {"payload_bytes",      "u64",    "byte",  "payload", "应用逻辑载荷字节 (与 wire 字节分开)", false},
        {"wire_bytes",         "u64",    "byte",  "payload", "实际传输字节 (含分片/头部)", true},
        {"payload_checksum_ok","bool",   "-",     "payload", "载荷校验结果 (序号相关模式, 防旧帧/损坏)", true},
        {"retry_count",        "u32",    "count", "flags",   "本消息重试次数 (重试不重复计 path)", false},
        {"evicted",            "bool",   "-",     "flags",   "本消息是否来自队列淘汰/回退样本", false},
        {"dropped",            "bool",   "-",     "flags",   "是否被判为丢失 (发送计划 vs 接收完成)", false},
    };
    return kFields;
}

inline std::string sample_csv_header()
{
    std::string h;
    const auto& f = sample_field_table();
    for (std::size_t i = 0; i < f.size(); ++i) { if (i) h += ','; h += f[i].name; }
    return h;
}

/* 标准样本记录 + JSONL/CSV 输出。null 用 `has_*` 标记表示。 */
struct SampleRecord
{
    std::string run_id;
    std::uint64_t seq{0};
    std::string route;
    std::uint32_t generation{0};
    PathKind   path{PathKind::unknown};
    std::uint64_t produced_ns{0};
    std::uint64_t publish_enter_ns{0};
    std::uint64_t transport_done_ns{0};
    bool          has_app_obtained{false};
    std::uint64_t app_obtained_ns{0};
    bool          has_fully_consumed{false};
    std::uint64_t fully_consumed_ns{0};
    std::uint64_t payload_bytes{0};
    bool          has_wire_bytes{false};
    std::uint64_t wire_bytes{0};
    bool          has_payload_checksum_ok{false};
    bool          payload_checksum_ok{false};
    std::uint32_t retry_count{0};
    bool          evicted{false};
    bool          dropped{false};

    std::string to_csv_row() const
    {
        std::string r;
        r += run_id;                              r += ',';
        r += std::to_string(seq);                 r += ',';
        r += route;                               r += ',';
        r += std::to_string(generation);          r += ',';
        r += path_kind_name(path);                r += ',';
        r += std::to_string(produced_ns);         r += ',';
        r += std::to_string(publish_enter_ns);    r += ',';
        r += std::to_string(transport_done_ns);   r += ',';
        r += has_app_obtained ? std::to_string(app_obtained_ns) : std::string();
        r += ',';
        r += has_fully_consumed ? std::to_string(fully_consumed_ns) : std::string();
        r += ',';
        /* derived */
        r += (publish_enter_ns && transport_done_ns >= publish_enter_ns)
             ? std::to_string(transport_done_ns - publish_enter_ns) : std::string();
        r += ',';
        r += (has_app_obtained && app_obtained_ns >= transport_done_ns)
             ? std::to_string(app_obtained_ns - transport_done_ns) : std::string();
        r += ',';
        r += (has_app_obtained && has_fully_consumed && fully_consumed_ns >= app_obtained_ns)
             ? std::to_string(fully_consumed_ns - app_obtained_ns) : std::string();
        r += ',';
        r += (has_fully_consumed && fully_consumed_ns >= produced_ns)
             ? std::to_string(fully_consumed_ns - produced_ns) : std::string();
        r += ',';
        r += std::to_string(payload_bytes);        r += ',';
        r += has_wire_bytes ? std::to_string(wire_bytes) : std::string();
        r += ',';
        r += has_payload_checksum_ok ? (payload_checksum_ok ? "1" : "0") : std::string();
        r += ',';
        r += std::to_string(retry_count);          r += ',';
        r += evicted ? "1" : "0";                  r += ',';
        r += dropped ? "1" : "0";
        return r;
    }

    std::string to_jsonl() const
    {
        std::string o = "{\"run_id\":\"" + json_escape(run_id) + "\"";
        o += ",\"seq\":" + std::to_string(seq);
        o += ",\"route\":\"" + json_escape(route) + "\"";
        o += ",\"generation\":" + std::to_string(generation);
        o += ",\"path\":\"" + std::string(path_kind_name(path)) + "\"";
        o += ",\"produced_ns\":" + std::to_string(produced_ns);
        o += ",\"publish_enter_ns\":" + std::to_string(publish_enter_ns);
        o += ",\"transport_done_ns\":" + std::to_string(transport_done_ns);
        o += ",\"app_obtained_ns\":" + (has_app_obtained ? std::to_string(app_obtained_ns) : std::string("null"));
        o += ",\"fully_consumed_ns\":" + (has_fully_consumed ? std::to_string(fully_consumed_ns) : std::string("null"));
        o += ",\"transport_ns\":" + ((publish_enter_ns && transport_done_ns >= publish_enter_ns)
             ? std::to_string(transport_done_ns - publish_enter_ns) : std::string("null"));
        o += ",\"delivery_ns\":" + ((has_app_obtained && app_obtained_ns >= transport_done_ns)
             ? std::to_string(app_obtained_ns - transport_done_ns) : std::string("null"));
        o += ",\"app_read_ns\":" + ((has_app_obtained && has_fully_consumed && fully_consumed_ns >= app_obtained_ns)
             ? std::to_string(fully_consumed_ns - app_obtained_ns) : std::string("null"));
        o += ",\"e2e_ns\":" + ((has_fully_consumed && fully_consumed_ns >= produced_ns)
             ? std::to_string(fully_consumed_ns - produced_ns) : std::string("null"));
        o += ",\"payload_bytes\":" + std::to_string(payload_bytes);
        o += ",\"wire_bytes\":" + (has_wire_bytes ? std::to_string(wire_bytes) : std::string("null"));
        o += ",\"payload_checksum_ok\":" + (has_payload_checksum_ok
             ? (payload_checksum_ok ? std::string("true") : std::string("false")) : std::string("null"));
        o += ",\"retry_count\":" + std::to_string(retry_count);
        o += ",\"evicted\":" + std::string(evicted ? "true" : "false");
        o += ",\"dropped\":" + std::string(dropped ? "true" : "false");
        o += "}";
        return o;
    }

    static std::string json_escape(const std::string& s)
    {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    }
};

/* ---- C. 运行元数据字段（§12 manifest.json 最低字段集） ---- */
inline const std::vector<FieldDef>& manifest_field_table()
{
    static const std::vector<FieldDef> kFields = {
        {"run_id",                "string", "-",     "manifest", "运行编号 (日期-编号-工作包)", false},
        {"work_package",          "string", "-",     "manifest", "工作包编号, 本工作包为 W03", false},
        {"source_revision",       "string", "-",     "manifest", "git commit (基线 e800ccc...)", false},
        {"working_tree_clean",    "bool",   "-",     "manifest", "工作区是否干净 (false 时须列出差异)", false},
        {"working_tree_diff",     "array",  "-",     "manifest", "未提交差异清单 (git status --short)", true},
        {"build_dir",             "string", "-",     "manifest", "构建目录绝对路径", false},
        {"build_type",            "string", "-",     "manifest", "CMake 构建类型", false},
        {"compiler",              "string", "-",     "manifest", "编译器与版本", false},
        {"binary_sha256",         "object", "-",     "manifest", "publisher/subscriber/library 的 sha256", false},
        {"loaded_library_paths",  "array",  "-",     "manifest", "运行进程实际加载的库路径", false},
        {"transport",             "enum",   "-",     "manifest", "shm|tlv|dzflat-a|dzflat-b|cyclonedds-iox", false},
        {"process_model",         "enum",   "-",     "manifest", "cross-process|same-process", false},
        {"measurement_mode",      "enum",   "-",     "manifest", "rusage|proc_task_sample (上下文切换采集模式)", false},
        {"diagnostics_enabled",   "bool",   "-",     "manifest", "扫描级诊断计数是否开启 (§10.2)", false},
        {"topic_count",           "u32",    "count", "manifest", "话题数", false},
        {"unique_topic_count",    "u32",    "count", "manifest", "独立话题数", false},
        {"worker_count",          "u32",    "count", "manifest", "固定 worker 数 (与池配置一致)", false},
        {"registered_count",      "u32",    "count", "manifest", "注册成功端点数", false},
        {"expected_count",        "u32",    "count", "manifest", "预期端点数", false},
        {"valid_rx_count",        "u32",    "count", "manifest", "完成有效收发的端点数", false},
        {"fallback_count",        "u32",    "count", "manifest", "兼容回退数 (目标规模要求 0)", false},
        {"sample_count",          "u64",    "count", "manifest", "有效样本数", false},
        {"window_s",              "f64",    "s",     "manifest", "观测窗口秒数 (空闲建议 >= 60)", false},
        {"config_hash",           "string", "-",     "manifest", "有效配置的 sha256", true},
        {"clock_source",          "string", "-",     "manifest", "时基, 固定 CLOCK_MONOTONIC", false},
        {"clock_cost_ns_per_call","f64",    "ns",    "manifest", "计时本身成本 (每次 clock_gettime)", false},
        {"collector_overhead_ns", "object", "ns",    "manifest", "采集器观测开销 (见 observation_overhead_schema)", false},
        {"result_files",          "array",  "-",     "manifest", "结果文件清单", false},
        {"path_evidence",         "object", "-",     "manifest", "路径证据登记册 (path_evidence.h)", false},
    };
    return kFields;
}

/* ---- D. 运行编排/判定字段登记（W03/t52：登记 ≠ 拥有；owner 逐条标注） ----
 *
 * 这些字段**不是** W03 的测量字段：它们由 W02 的基准产物（`summary.csv` /
 * `results.json` / `manifest.json`）定义并拥有，且 `bad_header` + 5 个 gate
 * **直接参与 `case_ok` 判定**（t47/W02-F3）。它们此前未进本 schema ⇒ 出现
 * 「判定与引用的关键字段没有登记」的形态（W02/R-5）。
 *
 * 本包只做**结构登记**：名称/类型/单位/可空/语义来源/归属。⛔ 不改 W02 任何产物与代码。
 *
 * `zero_semantics` 三种取值（对应 W02 `evidence_discipline.zero_value_three_states`
 * 与本文件 `zero_value_three_states` 段）：
 *   - `decided_zero`    = 已采集的**判定结果**，0/1 均有效（**不写空**）
 *   - `measured_zero`   = 已采集的实测量，0 = 实测为零
 *   - `uncollected_null` = 未采集/分母为零 ⇒ 写 `null`（CSV 空字段）
 */
struct RunFieldDef
{
    const char* name;          ///< 字段/列名
    const char* type;          ///< "u64" | "bool01" | "number" | "object" | "array<string>" | "string|null" | "enum"
    const char* unit;          ///< "count" | "-" | "period" | "ratio"
    const char* artifact;      ///< 所在产物: "summary.csv" / "results.json" / "manifest.json"
    bool        nullable;
    const char* zero_semantics;
    const char* owner;         ///< 语义所有者（工作包）
    const char* registered_by; ///< 登记者
    const char* semantics_source; ///< 语义来源（人可核对的单一出处）
    const char* description;
};

inline const std::vector<RunFieldDef>& run_level_field_table()
{
    static const std::vector<RunFieldDef> kFields = {
        /* --- summary.csv 新增列 6 个（W02/t47 F3；逐 case） --- */
        {"bad_header", "u64", "count", "summary.csv", false, "measured_zero", "W02", "W03",
         "xproc_benchmark.cpp 订阅循环 -> 控制块 sub_bad_header -> CaseResult::bad_header",
         "订阅侧段头/对象结构自相矛盾次数(CheckOutcome.header_ok==false 或长度不符; 视图路径 !view.valid()/d2.size()<kHdrSlots)"},
        {"late_ok", "bool01", "-", "summary.csv", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp run_one_case() 单处判定",
         "迟发 gate 判定: 1=未超预算; 仅在 late>late_abs_max 且 late/attempts>late_rate_max 时为 0。"
         "【判定结果, 一律不写空: 未触发也写 1, 0 表示判定失败, ⛔ 不得读作未采集】"},
        {"backlog_ok", "bool01", "-", "summary.csv", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp run_one_case() 单处判定",
         "积压 gate: backlog_max_ns/周期 > backlog_max_periods 时为 0。未触发写 1, 不写空"},
        {"send_blocked_ok", "bool01", "-", "summary.csv", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp run_one_case() 单处判定",
         "send_blocked > send_blocked_max(默认 0) 时为 0。未触发写 1, 不写空"},
        {"abnormal_ok", "bool01", "-", "summary.csv", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp run_one_case() 单处判定",
         "abnormal > abnormal_max(默认 0) 时为 0。未触发写 1, 不写空"},
        {"bad_header_ok", "bool01", "-", "summary.csv", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp run_one_case() 单处判定",
         "bad_header > bad_header_max(默认 0) 时为 0。未触发写 1, 不写空"},

        /* --- results.json 新增键 --- */
        {"cases[].bad_header", "number", "count", "results.json", false, "measured_zero", "W02", "W03",
         "xproc_benchmark.cpp results 写出", "同 summary.csv:bad_header"},
        {"cases[].gates", "object", "-", "results.json", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp results 写出",
         "{late_ok,backlog_ok,send_blocked_ok,abnormal_ok,bad_header_ok} 五个 bool, 与 summary.csv 的 0/1 一一对应"},

        /* --- manifest.json 新增键（运行元数据；结构登记，语义归 W02） --- */
        {"failure_thresholds", "object", "-", "manifest.json", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp manifest 写出; 判定预算定义见同文件 failure_thresholds 注释",
         "失败量判定预算: late_abs_max(u64,默认20)/late_rate_max(number,默认0.05)/"
         "backlog_max_periods(number,单位=发送周期数,0=不判仅信息)/send_blocked_max(u64,默认0)/"
         "abnormal_max(u64,默认0)/bad_header_max(u64,默认0); 另有 cli_overridden(bool)、"
         "gates(array<string>)、gates_in_judgement(object,逐 gate 本 run 触发计数)、"
         "informational_only(object,显式声明只作信息性保留的量及理由)"},
        {"currently_citable", "string|null", "-", "manifest.json", true, "uncollected_null", "W02", "W03",
         "xproc_benchmark.cpp manifest 写出; 纪律见 evidence_discipline.authority_claim_rule",
         "现行 run 的唯一权威指针; null=本轮未认领权威(缺 --purpose 或复核轮未给 --citation-authority)"},
        {"run_identity", "object", "-", "manifest.json", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp manifest 写出",
         "this_run_id/purpose(evidence|verification|experiment,缺省 verification)/purpose_explicit(bool)/"
         "this_run_is_evidence(bool)/authority_undeclared(bool)/claims_authority(bool)/"
         "citation_authority(string|null)"},
        {"evidence_discipline", "object", "-", "manifest.json", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp manifest 写出",
         "current_run_authority/authority_claim_rule(认领权威是显式动作: 仅 --purpose=evidence)/"
         "single_judgement_logic/zero_value_three_states(0|null|unwired)"},
        {"field_aliases", "object", "-", "manifest.json", false, "decided_zero", "W02", "W03",
         "xproc_benchmark.cpp manifest 写出",
         "同一事实在不同产物里的列名映射(summary.csv:late <-> results.json:late_sends; "
         "late_ok 等 5 个 <-> gates.*); ⛔ 表示同一来源, 不是两个独立来源"},
    };
    return kFields;
}

/* summary.csv 的**新增列**表头（按 W02 产物顺序；供读方机械拼接，⛔ 不改 W02 产物）。 */
inline const char* summary_csv_extra_header()
{
    return "bad_header,late_ok,backlog_ok,send_blocked_ok,abnormal_ok,bad_header_ok";
}

/* manifest.json 的新增顶层键（W02/t50 R-5）。 */
inline const std::vector<const char*>& manifest_extension_keys()
{
    static const std::vector<const char*> kKeys = {
        "failure_thresholds", "currently_citable", "run_identity",
        "evidence_discipline", "field_aliases",
    };
    return kKeys;
}

/* ---- JSON/CSV schema 文档 ---- */
inline std::string schema_document_json(const std::string& run_id)
{
    std::string o = "{\n";
    o += "  \"$schema\": \"w03-measurement/1.0\",\n";
    o += "  \"run_id\": \"" + SampleRecord::json_escape(run_id) + "\",\n";
    o += "  \"clock_source\": \"" + std::string(clock_source_name()) + "\",\n";
    o += "  \"timestamp_write_points\": [\n";
    for (std::size_t i = 0; i < kTimestampPointCount; ++i) {
        const TimestampPoint p = static_cast<TimestampPoint>(i);
        o += "    {\"name\": \"" + std::string(timestamp_point_name(p)) + "\", \"field\": \""
           + std::string(timestamp_point_name(p)) + "_ns\", \"description\": \""
           + std::string(timestamp_point_description(p)) + "\"}";
        if (i + 1 < kTimestampPointCount) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    o += "  \"sample_fields\": [\n";
    const auto& sf = sample_field_table();
    for (std::size_t i = 0; i < sf.size(); ++i) {
        o += "    {\"name\": \"" + std::string(sf[i].name) + "\", \"type\": \"" + sf[i].type
           + "\", \"unit\": \"" + sf[i].unit + "\", \"category\": \"" + sf[i].category
           + "\", \"nullable\": " + (sf[i].nullable ? "true" : "false")
           + ", \"description\": \"" + sf[i].description + "\"}";
        if (i + 1 < sf.size()) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    o += "  \"manifest_fields\": [\n";
    const auto& mf = manifest_field_table();
    for (std::size_t i = 0; i < mf.size(); ++i) {
        o += "    {\"name\": \"" + std::string(mf[i].name) + "\", \"type\": \"" + mf[i].type
           + "\", \"unit\": \"" + mf[i].unit + "\", \"nullable\": " + (mf[i].nullable ? "true" : "false")
           + ", \"description\": \"" + mf[i].description + "\"}";
        if (i + 1 < mf.size()) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    /* ---- 运行编排/判定字段登记（登记 ≠ 拥有；逐条 owner）---- */
    o += "  \"structural_vs_measurement_registration\": {\n";
    o += "    \"rule\": \"登记 != 拥有。measurable=W03 定义并拥有; run_orchestration=各工作包拥有, "
         "W03 只登记结构并逐条标注 owner; judgement_artifact=已采集的判定结果, 未触发也写 0/1, "
         "不适用\'未测写空字段\'纪律\",\n";
    o += "    \"measurable_owner\": \"W03\",\n";
    o += "    \"registered_by\": \"W03\",\n";
    o += "    \"judgement_artifact_note\": \"gate 列(0/1) 与 bad_header 均属已采集判定; "
         "0 的含义由字段名确定(见 zero_value_three_states), ⛔ 读方不得把 0 误解为未采集\"\n";
    o += "  },\n";

    o += "  \"zero_value_three_states\": {\n";
    o += "    \"0\": \"已采集且实测为零(可用 decidable 口径读作未发生); 或判定结果 0/1 中的 0\",\n";
    o += "    \"null\": \"未采集 或 分母为零(CSV 空字段, ⛔ 不写 0 冒充)\",\n";
    o += "    \"unwired\": \"字段有定义但无生产者; 其 0 不可读作未发生\",\n";
    o += "    \"decided_zero\": \"判定结果的 0/1: 两个值都是已采集的结论, ⛔ 不写空\"\n";
    o += "  },\n";

    o += "  \"run_level_fields\": [\n";
    const auto& rlf = run_level_field_table();
    for (std::size_t i = 0; i < rlf.size(); ++i) {
        o += "    {\"name\": \"" + std::string(rlf[i].name) + "\", \"type\": \"" + rlf[i].type
           + "\", \"unit\": \"" + rlf[i].unit + "\", \"artifact\": \"" + rlf[i].artifact
           + "\", \"nullable\": " + (rlf[i].nullable ? "true" : "false")
           + ", \"zero_semantics\": \"" + rlf[i].zero_semantics
           + "\", \"owner\": \"" + rlf[i].owner
           + "\", \"registered_by\": \"" + rlf[i].registered_by
           + "\", \"semantics_source\": \"" + rlf[i].semantics_source
           + "\", \"description\": \"" + rlf[i].description + "\"}";
        if (i + 1 < rlf.size()) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    o += "  \"run_level_csv_schemas\": {\"summary.csv.extra_columns\": \""
       + std::string(summary_csv_extra_header()) + "\"},\n";
    o += "  \"run_level_manifest_extension_keys\": [";
    {
        const auto& keys = manifest_extension_keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) o += ", ";
            o += "\"";
            o += keys[i];
            o += "\"";
        }
    }
    o += "],\n";

    o += "  \"counter_fields\": [\n";
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const CounterMeta& m = counter_table()[i];
        o += "    {\"name\": \"" + std::string(m.name) + "\", \"unit\": \"" + m.unit
           + "\", \"category\": \"" + m.category + "\", \"diagnostics_only\": "
           + (m.diagnostics_only ? "true" : "false")
           + ", \"value_when_diagnostics_off\": \""
           + (m.diagnostics_only ? "uncollected" : "collected")
           + "\", \"description\": \"" + m.description + "\"}";
        if (i + 1 < kCounterCount) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    /* 取值域声明：诊断关闭时 diagnostics_only 的 0 = **未采集**（⛔ 不是实测 0）。
     * R0-9/R0-10 的取证完备性判据依赖这条（脚本可按 diagnostics_only 机械判定可读性）。 */
    o += "  \"diagnostics_value_semantics\": {\"when_off\": \"uncollected\", "
         "\"when_on\": \"collected\", \"rule\": "
         "\"counter_is_diagnostics_only(id) && !diagnostics_enabled() => 该 0 是未采集, "
         "不得当读数; 判据见 counters.h counter_is_uncollected()\"},\n";

    /* 三个**不得混算**的显式声明（R0-8/R0-9/R0-10 + 规格 S4）。机器可读，避免报告混列。 */
    o += "  \"counter_semantics_guards\": [\n";
    o += "    {\"pair\": [\"ready_observed\", \"scan_ready_routes_total\"], "
         "\"rule\": \"不得相加\", \"why\": "
         "\"ready_observed 是*轮数*(本轮发现>=1条就绪的轮数), scan_ready_routes_total 是"
         "*route 数*(逐轮新入队 route 数之和); 量纲不同. 二者之比(后者/scan_rounds)才是"
         "平均每轮新入队 route 数\"},\n";
    o += "    {\"pair\": [\"deferred_depth_last\", \"deferred_depth_after_last\"], "
         "\"rule\": \"不得互相覆盖、不得混列一列\", \"why\": "
         "\"前者是扫描*前*(入队前)快照, 后者是扫描*后*快照; 语义不同(R0-10)\"},\n";
    o += "    {\"pair\": [\"deferred_depth_after_last\", \"pool_deferred_depth_total\"], "
         "\"rule\": \"gauge 不得当全池总量\", \"why\": "
         "\"*_last/_max 是全局 gauge(多 worker 时只保留最后写入者); 全池当前总深度 = "
         "Σ(每 worker 的 ScanRoundResult::deferred_depth_after), 唯一可安全求和的"
         "累计量是 deferred_depth_after_total\"}\n";
    o += "  ],\n";

    o += "  \"idle_states\": [\n";
    for (int i = 0; i <= static_cast<int>(IdleState::active_publishing); ++i) {
        const IdleState s = static_cast<IdleState>(i);
        o += "    {\"name\": \"" + std::string(idle_state_name(s)) + "\", \"expected\": \""
           + std::string(idle_state_expected_behavior(s)) + "\"}";
        if (i < static_cast<int>(IdleState::active_publishing)) o += ",";
        o += "\n";
    }
    o += "  ],\n";

    o += "  \"csv_schemas\": {\n";
    o += "    \"samples.csv\": {\"header\": \"" + sample_csv_header() + "\"},\n";
    o += "    \"counters.csv\": {\"header\": \"" + CounterRegistry::csv_header() + "\"}\n";
    o += "  },\n";
    o += "  \"evidence_levels\": [\"S0\", \"S1\", \"S2\", \"S3\", \"S4\", \"S5\"],\n";
    o += "  \"null_policy\": \"未测项输出 null (CSV 空字段) 并在 verdict.md 标 未确认; 不允许用 0 冒充\"\n";
    o += "}\n";
    return o;
}

/* ---------------------------------------------------------------------------
 * CSV 字段转义（RFC4180 最小集）：含 `,` / `"` / 换行 的字段必须整体加双引号，
 * 内部 `"` 写 `""`。⛔ 不转义会让描述里的逗号把一行拆成多列 —— 实测本文件在 t52
 * 之前已有 **8 行**列数不一致（含 `,` 的说明字段），t52 追加运行级字段后变 **19 行**。
 * 这类缺陷对"用 `csv` 模块读 schema.csv 的脚本"是静默的（多出的列被吞进最后一列），
 * 但会让**按列取值的**读方错位。已在 t52 一并修掉并加判据。
 * ------------------------------------------------------------------------- */
inline std::string csv_escape(const std::string& v)
{
    bool need = false;
    for (char c : v) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
    }
    if (!need) return v;
    std::string o = "\"";
    for (char c : v) {
        if (c == '"') o += "\"\"";
        else o += c;
    }
    o += "\"";
    return o;
}

/* schema 的 CSV 形态：字段名,类型,单位,类别,可空,说明（供脚本/表格工具直接读）。 */
inline std::string schema_document_csv()
{
    std::string o = "section,field,type,unit,category,nullable,description\n";
    for (const FieldDef& f : sample_field_table()) {
        o += "sample," + csv_escape(f.name) + "," + csv_escape(f.type) + "," + csv_escape(f.unit)
           + "," + csv_escape(f.category) + "," + (f.nullable ? "1" : "0")
           + "," + csv_escape(f.description) + "\n";
    }
    for (const FieldDef& f : manifest_field_table()) {
        o += "manifest," + csv_escape(f.name) + "," + csv_escape(f.type) + "," + csv_escape(f.unit)
           + "," + csv_escape(f.category) + "," + (f.nullable ? "1" : "0")
           + "," + csv_escape(f.description) + "\n";
    }
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const CounterMeta& m = counter_table()[i];
        o += "counter," + csv_escape(m.name) + ",u64," + csv_escape(m.unit) + ","
           + csv_escape(m.category) + ",0," + csv_escape(m.description) + "\n";
    }
    for (int i = 0; i <= static_cast<int>(IdleState::active_publishing); ++i) {
        const IdleState s = static_cast<IdleState>(i);
        o += "idle_state," + csv_escape(idle_state_name(s)) + ",enum,-,idle,0,"
           + csv_escape(idle_state_expected_behavior(s)) + "\n";
    }
    for (std::size_t i = 0; i < kTimestampPointCount; ++i) {
        const TimestampPoint p = static_cast<TimestampPoint>(i);
        o += "timestamp_point," + csv_escape(timestamp_point_name(p)) + ",u64,ns,ts,0,"
           + csv_escape(timestamp_point_description(p)) + "\n";
    }
    /* 运行编排/判定字段：section 前缀 "run_field:<artifact>"（登记 ≠ 拥有，owner 列随身携带）。 */
    for (const RunFieldDef& f : run_level_field_table()) {
        o += "run_field:" + csv_escape(f.artifact) + "," + csv_escape(f.name) + ","
           + csv_escape(f.type) + "," + csv_escape(f.unit)
           + ",zero=" + csv_escape(f.zero_semantics) + "/owner=" + csv_escape(f.owner)
           + "," + (f.nullable ? "1" : "0") + "," + csv_escape(f.description) + "\n";
    }
    return o;
}

}   // namespace measure
}   // namespace dzIPC
