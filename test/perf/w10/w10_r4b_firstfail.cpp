/* t43 要求 2：`first_failed_resource` **可定位性**的独立复验。
 *
 * 为什么另写一个探针而不是复用 w10_matrix：`w10_matrix` 的规模拓扑**不会**主动打干
 * chunk 池（池耗尽不是规模验收的一部分），因此它的 `first_failed_resource` 恒为 `none`。
 * 要证明"能从 none 变成具体资源名"，必须在**同一张表、同一顺序**下构造一次真实耗尽。
 *
 * 本探针做**两阶段对照**（同一进程、同一库）：
 *   阶段 1（不构造）：读 first_failed_resource ⇒ 期望 `none`
 *   阶段 2（构造 send 腿池耗尽，DZFlat 关 + 订阅者不消费）⇒ 期望 `chunk_exhausted`
 * ⛔ 表的顺序与 §13.3 逐字一致（与 w10_matrix.cpp 的同一枚举顺序），不另立口径。
 *
 * 用法: w10_r4b_firstfail [--out <dir>] [--run-id <id>]
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}
static unsigned long long g(CounterId id) { return CounterRegistry::instance().get(id); }

/* §13.3 十类，**与 w10_matrix.cpp 同一张表、同一顺序**（取第一个非零）。 */
static const char* first_failed_resource()
{
    const std::vector<std::pair<const char*, unsigned long long>> classes = {
        {"registration_rejected", g(CounterId::registration_rejected)},
        {"wait_token_invalid", g(CounterId::wait_token_invalid)},
        {"wait_set_full", g(CounterId::wait_set_full)},
        {"fd_limit", g(CounterId::fd_limit)},
        {"chunk_exhausted", g(CounterId::chunk_exhausted)},
        {"queue_backpressure", g(CounterId::queue_backpressure)},
        {"generation_mismatch", g(CounterId::generation_mismatch)},
        {"fallback_activated", g(CounterId::fallback_total)},
        {"publish_blocked", g(CounterId::publish_blocked)},
        {"rx_timeout", g(CounterId::rx_timeout)},
    };
    for (const auto& kv : classes)
        if (kv.second != 0) return kv.first;
    return "none";
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string out, run_id = "r4b-firstfail";
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
        if (std::strcmp(argv[i], "--run-id") == 0) run_id = argv[i + 1];
    }

    /* ---- 阶段 1：不构造任何耗尽 ⇒ 期望 none ---- */
    dzIPC::EnableDzFlat(false);
    const std::string t1 = "r4b_ff_baseline";
    {
        auto p = std::make_shared<dzIPC::Msg::StdString>();
        p->set_msg_id(kMsgId);
        p->str = "x";
        dzIPC::shm::shm_pub_ipc pub{td(), t1, 0, false};
        dzIPC::shm::shm_sub_ipc sub{td(), t1, 0, 64, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        (void)pub.publish(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const std::string stage1 = first_failed_resource();
    std::printf("stage1_no_construction first_failed_resource=%s\n", stage1.c_str());

    /* ---- 阶段 2：send 腿池耗尽（订阅者建了但**从不 pop**）---- */
    const std::string t2 = "r4b_ff_exhaust";
    long publish_ok = 0;
    {
        dzIPC::shm::shm_pub_ipc pub{td(), t2, 0, false};
        dzIPC::shm::shm_sub_ipc sub{td(), t2, 0, 64, false};
        pub.InitChannel();
        sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const std::size_t big = 64 * 1024;
        for (int i = 0; i < 200; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str.assign(big, 'F');
            if (pub.publish(m)) ++publish_ok;
        }
    }
    const std::string stage2 = first_failed_resource();
    std::printf("stage2_send_leg_exhausted first_failed_resource=%s publish_ok=%ld chunk_exhausted=%llu "
                "chunk_alloc_failed=%llu\n",
                stage2.c_str(), publish_ok, g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed));

    const bool ok = (stage1 == "none") && (stage2 != "none") && (stage2 == "chunk_exhausted");
    std::printf("FIRSTFAIL_LOCATABLE=%d (none → %s)\n", ok ? 1 : 0, stage2.c_str());

    if (!out.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec))
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s\n", out.c_str());
            return 2;
        }
        std::filesystem::create_directories(out, ec);
        std::ofstream f(out + "/first_failed_resource.json");
        f << "{\n  \"run_id\": \"" << run_id << "\",\n";
        f << "  \"criteria_version\": \"t43/§13.3/v1\",\n";
        f << "  \"stage1_no_construction\": \"" << stage1 << "\",\n";
        f << "  \"stage2_send_leg_exhausted\": \"" << stage2 << "\",\n";
        f << "  \"publish_ok_stage2\": " << publish_ok << ",\n";
        f << "  \"chunk_exhausted\": " << g(CounterId::chunk_exhausted) << ",\n";
        f << "  \"chunk_alloc_failed\": " << g(CounterId::chunk_alloc_failed) << ",\n";
        f << "  \"locatable\": " << (ok ? "true" : "false") << ",\n";
        f << "  \"table_order\": \"§13.3 十类，与 w10_matrix.cpp 同一枚举顺序（取第一个非零）\",\n";
        f << "  \"note\": \"stage1 用真实 pub/sub 收发但不打干池；stage2 用 DZFlat=off + 订阅者不消费 ⇒ send 腿池空\"\n}\n";
    }
    return ok ? 0 : 1;
}
