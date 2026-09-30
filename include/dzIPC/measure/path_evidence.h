#pragma once
/* W03 测量口径 · 传输路径证据登记（证据不足一律标「未确认」）
 * ============================================================================
 * 交付依据：§4 W03 最后一条「交付：采集器、字段定义、路径证据、观测开销对照。
 * 验收：已知线程活动可被正确计入，**路径证据不足的实验明确标注"未确认"**」，
 * 与 §11「实施状态分级与证据准入」的 S0–S5 分级、§11 末尾"以下事实必须分别登记，
 * 不能从一项事实推导另一项事实"。
 *
 * 关键规则（写进代码，不靠人记）：
 *   · 一条路径要标"已确认(confirmed)"，必须**同时**具备：真实调用点(call site)、
 *     运行日志中的路径标识(runtime evidence)、二进制/库指纹(binary fingerprint)，
 *     以及该路径自己的计数（TLV / DZFlat A / DZFlat B 三者的计数必须能分开）。
 *   · 只有"源码存在"或"模块已接入"但没有运行证据 ⇒ 只能到 S1/S2，且 `label()`
 *     输出「未确认」。**不允许**把 S2 写成"路径已确认"。
 *   · "使用 loan()"不等于应用就地构造(DZFlat B)；"内部路径标记为 A"不等于
 *     应用侧直接写 chunk。因此 DZFlat B 的证据必须包含 call site 指向
 *     `loan → 应用原地构造 → publish_loan`，仅内部 `loan()` 调用不足以判定 B。
 *   · 回退路径(fallback)单独登记：出现回退时，对应实验的路径结论必须写明
 *     "含回退 N 次"，不能把回退样本混入纯路径统计。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "dzIPC/measure/counters.h"

namespace dzIPC {
namespace measure {

/* 传输路径分类（§6.1 能力边界 / §10.7 三个实验组的路径口径）。 */
enum class PathKind
{
    unknown = 0,
    tlv,               ///< TLV 物化（含兼容路径）
    dzflat_a,          ///< DZFlat A：对象 → 共享段一次复制
    dzflat_b,          ///< DZFlat B：应用在借样内存中原地构造
    cyclonedds_iox,    ///< CycloneDDS + iceoryx 配置组合
    compat_fallback    ///< 显式兼容回退（保留兼容收包线程等）
};

inline const char* path_kind_name(PathKind k) noexcept
{
    switch (k) {
    case PathKind::tlv:             return "tlv";
    case PathKind::dzflat_a:        return "dzflat-a";
    case PathKind::dzflat_b:        return "dzflat-b";
    case PathKind::cyclonedds_iox:  return "cyclonedds-iox";
    case PathKind::compat_fallback: return "compat-fallback";
    case PathKind::unknown:         break;
    }
    return "unknown";
}

/* §11 的 S0–S5 分级。等级只能逐级提升。 */
enum class EvidenceLevel
{
    s0_unchecked = 0,        ///< 只有设计/口头结论
    s1_source_exists,        ///< 源码存在：文件、符号、配置入口、代码版本
    s2_module_wired,         ///< 模块已接入：真实调用点、运行日志路径标识、构建产物指纹
    s3_single_case_verified, ///< 单项验证通过：可复现命令、通过条件、原始日志、失败计数
    s4_independent_verified, ///< 独立验收通过：非实现者复核、完整矩阵、基线对照、未决项
    s5_default_enabled       ///< 默认启用：S4 + 回滚演练 + 兼容矩阵 + 发布配置签认
};

inline const char* evidence_level_name(EvidenceLevel l) noexcept
{
    switch (l) {
    case EvidenceLevel::s0_unchecked:            return "S0";
    case EvidenceLevel::s1_source_exists:        return "S1";
    case EvidenceLevel::s2_module_wired:         return "S2";
    case EvidenceLevel::s3_single_case_verified: return "S3";
    case EvidenceLevel::s4_independent_verified: return "S4";
    case EvidenceLevel::s5_default_enabled:      return "S5";
    }
    return "S0";
}

/* 一条路径的证据包。任何一项为空都进 missing[]，并由 confirmed()/label() 体现。 */
struct PathEvidence
{
    PathKind                  path{PathKind::unknown};
    EvidenceLevel             level{EvidenceLevel::s0_unchecked};
    std::string               experiment_id;   ///< run_id / 实验编号
    std::vector<std::string>  call_sites;      ///< 真实调用点（文件:行:符号）
    std::vector<std::string>  runtime_evidence;///< 运行日志/计数中的路径标识
    std::vector<std::string>  binary_fingerprints; ///< 二进制/库 sha256
    std::vector<std::string>  counters;        ///< 该路径的计数名（来自 counter_table）
    std::vector<std::string>  experiment_groups;///< 该路径参与的实验组(传输/完整读取/生产到消费)
    std::uint64_t             samples{0};      ///< 该路径有效样本数
    std::uint64_t             fallback_count{0};///< 其中回退样本数
    std::vector<std::string>  missing;         ///< 缺失项（中文，进 verdict）

    /* 判定：必须有调用点 + 运行证据 + 二进制指纹 + 该路径自己的计数 +
     * 至少一条有效样本，且等级 >= S3，且 `missing` 为空。
     * 关键：判定与 missing[] 必须来自**同一个函数**（compute_missing），否则会出现
     * "missing 里写了缺 publish_loan，label 却是已确认"的自相矛盾输出。 */
    bool confirmed() const noexcept { return compute_missing().empty(); }

    const char* label() const noexcept { return confirmed() ? "已确认" : "未确认"; }

    /* 缺失项计算（纯函数）。不修改对象状态，可随时调用。 */
    std::vector<std::string> compute_missing() const
    {
        std::vector<std::string> m;
        if (call_sites.empty())          m.push_back("缺真实调用点(call site)");
        if (runtime_evidence.empty())    m.push_back("缺运行日志/计数中的路径标识");
        if (binary_fingerprints.empty()) m.push_back("缺二进制/库指纹");
        if (counters.empty())            m.push_back("缺该路径独立计数");
        if (samples == 0)                m.push_back("无有效样本");
        if (level < EvidenceLevel::s3_single_case_verified)
            m.push_back(std::string("证据等级不足(当前 ") + evidence_level_name(level)
                        + ", 需 >= S3)");
        if (path == PathKind::dzflat_b) {
            bool has_inplace = false;
            for (const std::string& s : call_sites)
                if (s.find("publish_loan") != std::string::npos) has_inplace = true;
            if (!has_inplace)
                m.push_back("DZFlat B 缺 `loan -> 应用原地构造 -> publish_loan` 调用点(内部 loan() 不算)");
        }
        if (path == PathKind::unknown) m.push_back("路径未分类");
        return m;
    }

    void refresh_missing() { missing = compute_missing(); }

    std::string to_json() const
    {
        const std::string ind = "    ";
        std::string o;
        o += ind + "\"path\": \"" + path_kind_name(path) + "\",\n";
        o += ind + "\"level\": \"" + evidence_level_name(level) + "\",\n";
        o += ind + "\"status\": \"" + std::string(label()) + "\",\n";
        o += ind + "\"experiment_id\": \"" + json_escape_simple(experiment_id) + "\",\n";
        o += ind + "\"samples\": " + std::to_string(samples) + ",\n";
        o += ind + "\"fallback_count\": " + std::to_string(fallback_count) + ",\n";
        o += ind + "\"call_sites\": " + json_array(call_sites) + ",\n";
        o += ind + "\"runtime_evidence\": " + json_array(runtime_evidence) + ",\n";
        o += ind + "\"binary_fingerprints\": " + json_array(binary_fingerprints) + ",\n";
        o += ind + "\"counters\": " + json_array(counters) + ",\n";
        o += ind + "\"experiment_groups\": " + json_array(experiment_groups) + ",\n";
        o += ind + "\"missing\": " + json_array(compute_missing()) + "\n";
        return o;
    }

    static std::string json_escape_simple(const std::string& s)
    {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    }

    static std::string json_array(const std::vector<std::string>& v)
    {
        std::string o = "[";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) o += ", ";
            o += "\"" + json_escape_simple(v[i]) + "\"";
        }
        o += "]";
        return o;
    }
};

/* 路径证据登记册：一次运行内所有路径的证据包 + 未确认汇总。 */
class EvidenceRegister
{
public:
    void add(const PathEvidence& e)
    {
        PathEvidence copy = e;
        copy.refresh_missing();
        items_.push_back(copy);
    }

    std::size_t size() const noexcept { return items_.size(); }
    const std::vector<PathEvidence>& items() const noexcept { return items_; }

    /* 未确认的路径列表（供 verdict 直接引用）。 */
    std::vector<std::string> unconfirmed_paths() const
    {
        std::vector<std::string> v;
        for (const PathEvidence& e : items_)
            if (!e.confirmed()) v.push_back(path_kind_name(e.path));
        return v;
    }

    std::vector<std::string> confirmed_paths() const
    {
        std::vector<std::string> v;
        for (const PathEvidence& e : items_)
            if (e.confirmed()) v.push_back(path_kind_name(e.path));
        return v;
    }

    std::string to_json() const
    {
        std::string o = "{\n  \"kind\": \"path_evidence\",\n  \"paths\": [\n";
        for (std::size_t i = 0; i < items_.size(); ++i) {
            o += "  {\n";
            o += items_[i].to_json();
            o += "  }";
            if (i + 1 < items_.size()) o += ",";
            o += "\n";
        }
        o += "  ],\n";
        o += "  \"confirmed\": " + PathEvidence::json_array(confirmed_paths()) + ",\n";
        o += "  \"unconfirmed\": " + PathEvidence::json_array(unconfirmed_paths()) + "\n";
        o += "}\n";
        return o;
    }

private:
    std::vector<PathEvidence> items_{};
};

}   // namespace measure
}   // namespace dzIPC
