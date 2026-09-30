/* F1/t35 要求 5：**未接线 ID 的 0 必须读作"未采集"** 的机械证据。
 * 对每个仍未接线的 ID，在**同一次真实负载**（真建 pub/sub + 收发 + 池耗尽）后读其值，
 * 证明"这些 0 不是实测为 0"——因为它们连写入点都没有（清点表已给出）。
 * 输出：逐 ID 一行 id=value，并给出"未接线 ID 集合"与"其值全 0"的机械判定。 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;
namespace {
constexpr int kMsgId = 98;
std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }
}  // namespace

int main()
{
    /* 一次"看起来什么都有"的负载：真建双子端、真收发、真把池打干。 */
    dzIPC::EnableDzFlat(true);
    auto pub_td = td();
    auto sub_td = td();
    dzIPC::shm::shm_pub_ipc pub{pub_td, "f1_undeclared", 0, false};
    dzIPC::shm::shm_sub_ipc sub{sub_td, "f1_undeclared", 0, 4, false};
    pub.InitChannel();
    sub.InitChannel();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    for (int i = 0; i < 60; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str.assign(64 * 1024, 'u');
        (void)pub.publish(m);
    }
    {
        std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
        for (int i = 0; i < 120; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdStringFlat>(64 * 1024);
            if (!lo.valid()) continue;
            held.push_back(std::move(lo));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        held.clear();
    }

    auto& reg = CounterRegistry::instance();
    const auto g = [&](CounterId id) { return static_cast<unsigned long long>(reg.get(id)); };
    /* 仍无写入点的 ID（清点表 11_*.tsv 里 verdict=table_only，本清单由脚本生成后贴入）。 */
    const std::vector<const char*> unwired = {
        "tlv_bytes", "dzflat_a_bytes", "dzflat_b_bytes",
        "fallback_type_incompatible", "fallback_oversized", "fallback_pool_exhausted",
        "registration_rejected", "fd_limit", "queue_backpressure", "generation_mismatch",
        "publish_blocked", "publish_failed", "rx_timeout",
        "seq_monotonic_ok", "seq_out_of_order", "seq_duplicate", "seq_lost",
        "payload_checksum_ok", "payload_checksum_bad",
        "path_selection_dzflat_disabled", "path_selection_type_unsupported",
        "fallback_reason_unknown",
        "borrow_failed_no_receiver", "borrow_failed_pool_exhausted", "borrow_failed_oversized",
        "borrow_failed_publish", "borrow_failed_reason_unknown",
    };
    int all_zero = 1;
    for (std::size_t i = 0; i < dzIPC::measure::kCounterCount; ++i)
    {
        const CounterId id = static_cast<CounterId>(i);
        const char* nm = dzIPC::measure::counter_name(id);
        bool is_unwired = false;
        for (const char* u : unwired) if (std::string(u) == nm) is_unwired = true;
        if (is_unwired)
        {
            const auto v = g(id);
            if (v != 0) all_zero = 0;
            std::printf("UNWIRED %-32s = %llu\n", nm, v);
        }
    }
    std::printf("wired_sample chunk_exhausted=%llu chunk_alloc_failed=%llu queue_evicted=%llu "
                "wait_set_full=%llu (对照: 这几类在本负载下确有真实写入点)\n",
                g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed), g(CounterId::queue_evicted),
                g(CounterId::wait_set_full));
    std::printf("UNDECLARED_PROBE_DONE all_unwired_zero=%d (期望 1: 未接线者恒 0 ⇒ 不可读作'未发生')\n",
                all_zero);
    ::fflush(nullptr);
    _exit(all_zero ? 0 : 1);
}
