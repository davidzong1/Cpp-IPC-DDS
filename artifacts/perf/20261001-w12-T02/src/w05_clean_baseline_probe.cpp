/* T02 判别探针：**不构造任何陈旧槽位**时，同样的「发布一条 + 等 200ms 收一条」数据面
 * 模式是否会偶发丢消息（rx < N）。
 *
 * 用途：门控用例 `test_w05_stale_slot_gate[_arm].cpp` 的判据②（活订阅者不被误断）
 * 在 T02 的一次复跑里出现过 99/100。本探针用来区分两种解释：
 *   · 若**无陈旧槽位**的干净路径也偶发 99/100 ⇒ 丢包来自数据面/用例时序，而非
 *     「陈旧 cc_id 位被复用后被 disconnect_receivers() 摘掉」；
 *   · 若干净路径 N/N 稳定，而带陈旧槽位的门控用例偶发丢包 ⇒ 指向回收时机路径。
 * ⛔ 本探针只读结论，不替任何一方下断言：它把两种情形都如实打印出来。
 *
 * 用法: ./w05_clean_baseline_probe <domain> <n>
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "libipc/ipc.h"
#include "libipc/shm.h"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kMsgId = 93;

std::shared_ptr<dzIPC::TopicData> make_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

}   // namespace

int main(int argc, char** argv)
{
    const long dom = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 9500;
    const int n = (argc > 2) ? std::atoi(argv[2]) : 100;

    static std::atomic<int> seq{0};
    const std::string topic = "w05clean_" + std::to_string(dom) + "_" + std::to_string(seq.fetch_add(1));
    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const bool fb = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    std::printf("arm=%s topic=%s n=%d\n", fb ? "compat" : "process-scheduler", topic.c_str(), n);

    int rc = 0;
    {
        auto td = make_td();
        dzIPC::shm::shm_pub_ipc pub{td, topic, (size_t)dom, /*verbose=*/false};
        pub.InitChannel();
        /* ⛔ 关键：**不** craft 陈旧槽位、不 add_peer/remove_peer，一切走产品正常路径。 */

        dzIPC::shm::shm_sub_ipc sub{td, topic, (size_t)dom, /*queue_size=*/8, /*verbose=*/false};
        sub.InitChannel();
        std::this_thread::sleep_for(300ms);   /* 等握手完成（与门控用例同） */

        int rx = 0;
        for (int i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "clean-" + std::to_string(i);
            if (!pub.publish(m)) { std::printf("publish_failed_at=%d\n", i); rc = 2; break; }
            const auto deadline = Clock::now() + 200ms;
            bool got = false;
            while (Clock::now() < deadline)
            {
                auto sink = make_td();
                if (sub.try_get_clone(sink)) { ++rx; got = true; break; }
                std::this_thread::sleep_for(1ms);
            }
            if (!got) { std::printf("miss_at=%d\n", i); }
        }
        std::printf("clean_baseline n=%d received=%d\n", n, rx);
        if (rx != n) { rc = 1; }
    }
    ipc::route::clear_storage(shm_topic_segment_name(topic, (size_t)dom).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, (size_t)dom).c_str());
    std::printf("T02_CLEAN_BASELINE_DONE rc=%d\n", rc);
    return rc;
}
