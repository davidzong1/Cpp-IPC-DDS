/* t43：**运行时真相表**（把 t35 的静态清点与运行期实测对账）。
 *
 * 为什么需要：t35 的 27 项"未接线"清单来自**静态**审计（`audit_counter_wiring.py`）。
 * 静态审计看不到两类写点：
 *   ① **经载体变量**的间接写（`r.inc(c.detail)`，`detail` 是运行期决定的 `CounterId`）
 *      —— 例：`path_selection_dzflat_disabled` 静态判 `table_only`，但本次 SHM 千路
 *      实测 **4007**（= DZFlat 关闭态下的 TLV 条数）；
 *   ② 载体函数的调用方**新增**后（`shm_pub_sub_ipc.h` 现已调用 `note_dzflat_borrow_failed`），
 *      静态判定从 `table_only` 变 `potential_used`。
 * ⇒ 正确口径 = **静态清单 ∪ 运行期实测**；⛔ 不得只凭静态清单说"这一项永远不会变非零"。
 *
 * 本探针在两条腿下各 dump 一次全量 27 项 + 三个载体细分，产出运行期真相表。
 * 用法: w10_r4b_runtimetruth <leg: A|B|baseline> [--out <dir>]
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
static constexpr std::uint32_t kMsgId = 95;

struct Row { const char* name; CounterId id; };
/* t35 §5 清单的 27 项（逐字）+ 三个"载体细分"（其静态判定已变）。 */
static const std::vector<Row>& table()
{
    static const std::vector<Row> t = {
        {"tlv_bytes", CounterId::tlv_bytes},
        {"dzflat_a_bytes", CounterId::dzflat_a_bytes},
        {"dzflat_b_bytes", CounterId::dzflat_b_bytes},
        {"fallback_type_incompatible", CounterId::fallback_type_incompatible},
        {"fallback_oversized", CounterId::fallback_oversized},
        {"fallback_pool_exhausted", CounterId::fallback_pool_exhausted},
        {"fallback_reason_unknown", CounterId::fallback_reason_unknown},
        {"borrow_failed_no_receiver", CounterId::borrow_failed_no_receiver},
        {"borrow_failed_pool_exhausted", CounterId::borrow_failed_pool_exhausted},
        {"borrow_failed_oversized", CounterId::borrow_failed_oversized},
        {"borrow_failed_publish", CounterId::borrow_failed_publish},
        {"borrow_failed_reason_unknown", CounterId::borrow_failed_reason_unknown},
        {"path_selection_dzflat_disabled", CounterId::path_selection_dzflat_disabled},
        {"path_selection_type_unsupported", CounterId::path_selection_type_unsupported},
        {"registration_rejected", CounterId::registration_rejected},
        {"fd_limit", CounterId::fd_limit},
        {"queue_backpressure", CounterId::queue_backpressure},
        {"generation_mismatch", CounterId::generation_mismatch},
        {"publish_blocked", CounterId::publish_blocked},
        {"publish_failed", CounterId::publish_failed},
        {"rx_timeout", CounterId::rx_timeout},
        {"seq_monotonic_ok", CounterId::seq_monotonic_ok},
        {"seq_out_of_order", CounterId::seq_out_of_order},
        {"seq_duplicate", CounterId::seq_duplicate},
        {"seq_lost", CounterId::seq_lost},
        {"payload_checksum_ok", CounterId::payload_checksum_ok},
        {"payload_checksum_bad", CounterId::payload_checksum_bad},
    };
    return t;
}
/* 本 run **可判定**的类别（接线有效且运行期可触发）。 */
static const std::vector<Row>& covered()
{
    static const std::vector<Row> c = {
        {"wait_set_full", CounterId::wait_set_full},
        {"wait_token_invalid", CounterId::wait_token_invalid},
        {"chunk_exhausted", CounterId::chunk_exhausted},
        {"chunk_alloc_failed", CounterId::chunk_alloc_failed},
        {"queue_evicted", CounterId::queue_evicted},
        {"fallback_total", CounterId::fallback_total},
        {"path_selection_dzflat_disabled", CounterId::path_selection_dzflat_disabled},
    };
    return c;
}
static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string leg = argc > 1 ? argv[1] : "baseline";
    std::string out;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];

    auto& R = CounterRegistry::instance();
    R.set_diagnostics_enabled(false);   /* 与 w10 正式窗口同档 */

    if (leg == "A")
    {
        /* A 腿：DZFlat 关 + 订阅者不消费 ⇒ send 腿池空（chunk_exhausted） */
        dzIPC::EnableDzFlat(false);
        const std::string t = "rtt_legA";
        dzIPC::shm::shm_pub_ipc pub{td(), t, 0, false};
        dzIPC::shm::shm_sub_ipc sub{td(), t, 0, 64, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        for (int i = 0; i < 200; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str.assign(64 * 1024, 'A');
            (void)pub.publish(m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    else if (leg == "B")
    {
        /* B 腿：DZFlat 开 + 借样池空 ⇒ loan 被拒（chunk_alloc_failed） */
        dzIPC::EnableDzFlat(true);
        const std::string t = "rtt_legB";
        dzIPC::shm::shm_pub_ipc pub{td(), t, 0, false};
        dzIPC::shm::shm_sub_ipc sub{td(), t, 0, 4, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
        for (int i = 0; i < 120; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdStringFlat>(64 * 1024);
            if (lo.valid()) held.push_back(std::move(lo));
        }
        held.clear();
    }
    else
    {
        /* baseline：DZFlat 关、有接收方、正常收发 ⇒ 触发 path_selection_dzflat_disabled */
        dzIPC::EnableDzFlat(false);
        const std::string t = "rtt_baseline";
        dzIPC::shm::shm_pub_ipc pub{td(), t, 0, false};
        dzIPC::shm::shm_sub_ipc sub{td(), t, 0, 64, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        for (int i = 0; i < 50; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "b";
            (void)pub.publish(m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::printf("RUNTIME_TRUTH leg=%s diagnostics_enabled=%d\n", leg.c_str(), R.diagnostics_enabled() ? 1 : 0);
    std::vector<std::pair<std::string, unsigned long long>> nonzero, zero;
    for (const auto& r : table())
    {
        const unsigned long long v = R.get(r.id);
        std::printf("  %-34s %llu\n", r.name, v);
        (v ? nonzero : zero).push_back({r.name, v});
    }
    std::printf("UNWIRED_SET_NONZERO=%zu (", nonzero.size());
    for (size_t i = 0; i < nonzero.size(); ++i) std::printf("%s%s=%llu", i ? "," : "", nonzero[i].first.c_str(), nonzero[i].second);
    std::printf(")\n");
    std::printf("COVERED_LEG_READINGS ");
    for (const auto& r : covered()) std::printf("%s=%llu ", r.name, R.get(r.id));
    std::printf("\n");

    if (!out.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec))
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s\n", out.c_str());
            return 2;
        }
        std::filesystem::create_directories(out, ec);
        std::ofstream f(out + "/runtime_truth_" + leg + ".json");
        f << "{\n  \"leg\": \"" << leg << "\",\n  \"diagnostics_enabled\": "
          << (R.diagnostics_enabled() ? "true" : "false") << ",\n  \"t35_unwired_set_readings\": {\n";
        const auto& t = table();
        for (size_t i = 0; i < t.size(); ++i)
            f << "    \"" << t[i].name << "\": " << R.get(t[i].id) << (i + 1 < t.size() ? "," : "") << "\n";
        f << "  },\n  \"nonzero_in_unwired_set\": {";
        for (size_t i = 0; i < nonzero.size(); ++i)
            f << (i ? ", " : "") << "\"" << nonzero[i].first << "\": " << nonzero[i].second;
        f << "},\n  \"covered_readings\": {";
        const auto& c = covered();
        for (size_t i = 0; i < c.size(); ++i)
            f << (i ? ", " : "") << "\"" << c[i].name << "\": " << R.get(c[i].id);
        f << "}\n}\n";
    }
    return 0;
}
