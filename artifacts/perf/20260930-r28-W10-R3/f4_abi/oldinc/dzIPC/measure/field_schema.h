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

    o += "  \"counter_fields\": [\n";
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const CounterMeta& m = counter_table()[i];
        o += "    {\"name\": \"" + std::string(m.name) + "\", \"unit\": \"" + m.unit
           + "\", \"category\": \"" + m.category + "\", \"diagnostics_only\": "
           + (m.diagnostics_only ? "true" : "false")
           + ", \"description\": \"" + m.description + "\"}";
        if (i + 1 < kCounterCount) o += ",";
        o += "\n";
    }
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

/* schema 的 CSV 形态：字段名,类型,单位,类别,可空,说明（供脚本/表格工具直接读）。 */
inline std::string schema_document_csv()
{
    std::string o = "section,field,type,unit,category,nullable,description\n";
    for (const FieldDef& f : sample_field_table()) {
        o += "sample," + std::string(f.name) + "," + f.type + "," + f.unit + "," + f.category
           + "," + (f.nullable ? "1" : "0") + "," + f.description + "\n";
    }
    for (const FieldDef& f : manifest_field_table()) {
        o += "manifest," + std::string(f.name) + "," + f.type + "," + f.unit + "," + f.category
           + "," + (f.nullable ? "1" : "0") + "," + f.description + "\n";
    }
    for (std::size_t i = 0; i < kCounterCount; ++i) {
        const CounterMeta& m = counter_table()[i];
        o += "counter," + std::string(m.name) + ",u64," + m.unit + "," + m.category
           + ",0," + m.description + "\n";
    }
    for (int i = 0; i <= static_cast<int>(IdleState::active_publishing); ++i) {
        const IdleState s = static_cast<IdleState>(i);
        o += "idle_state," + std::string(idle_state_name(s)) + ",enum,-,idle,0,"
           + idle_state_expected_behavior(s) + "\n";
    }
    for (std::size_t i = 0; i < kTimestampPointCount; ++i) {
        const TimestampPoint p = static_cast<TimestampPoint>(i);
        o += "timestamp_point," + std::string(timestamp_point_name(p)) + ",u64,ns,ts,0,"
           + timestamp_point_description(p) + "\n";
    }
    return o;
}

}   // namespace measure
}   // namespace dzIPC
