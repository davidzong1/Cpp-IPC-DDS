/* t41 同臂复跑：证明三语义可分（(a) 路径选择 / (b) 真回退 / (c) B 借样失败）。
 * 三臂在同一二进制、同一库、同一进程模型下依次跑，逐臂 reset 计数后读同一组 ID。 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"

using namespace dzIPC::measure;

static constexpr std::uint32_t kMsgId = 61;
static constexpr std::uint64_t kDomain = 4242;
static unsigned g_serial = 0;
static std::string unique_topic(const char* tag)
{
    return std::string("t41_") + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(++g_serial);
}
static std::uint64_t c(CounterId id) { return CounterRegistry::instance().snapshot().get(id); }

struct Row
{
    std::uint64_t tlv, a, b, fb_total, sel_off, sel_unsup, fb_type, fb_unknown,
        bf_no_rx, bf_unknown, bf_publish, bf_oversized,
        chunk_ex, chunk_af, q_evict, reg_failed, reg_ok, reg_attempts;
};
static Row read_row()
{
    return Row{c(CounterId::tlv_messages), c(CounterId::dzflat_a_messages), c(CounterId::dzflat_b_messages),
               c(CounterId::fallback_total), c(CounterId::path_selection_dzflat_disabled),
               c(CounterId::path_selection_type_unsupported), c(CounterId::fallback_type_incompatible),
               c(CounterId::fallback_reason_unknown), c(CounterId::borrow_failed_no_receiver),
               c(CounterId::borrow_failed_reason_unknown), c(CounterId::borrow_failed_publish),
               c(CounterId::borrow_failed_oversized),
               c(CounterId::chunk_exhausted), c(CounterId::chunk_alloc_failed), c(CounterId::queue_evicted),
               c(CounterId::registration_failed), c(CounterId::registration_ok),
               c(CounterId::registration_attempts)};
}
static void dump(const char* arm, const Row& r)
{
    std::printf("%-22s tlv=%-4llu a=%-4llu b=%-4llu fb_total=%-3llu sel_off=%-4llu sel_unsup=%-3llu "
                "fb_type=%-3llu fb_unknown=%-3llu | bf_no_rx=%-3llu bf_unknown=%-3llu bf_pub=%-3llu bf_over=%-3llu\n",
                arm, (unsigned long long)r.tlv, (unsigned long long)r.a, (unsigned long long)r.b,
                (unsigned long long)r.fb_total, (unsigned long long)r.sel_off, (unsigned long long)r.sel_unsup,
                (unsigned long long)r.fb_type, (unsigned long long)r.fb_unknown, (unsigned long long)r.bf_no_rx,
                (unsigned long long)r.bf_unknown, (unsigned long long)r.bf_publish, (unsigned long long)r.bf_oversized);
    std::printf("%-22s [t35 家族] chunk_ex=%-3llu chunk_af=%-3llu q_evict=%-3llu | [注册家族] attempts=%-3llu ok=%-3llu failed=%-3llu\n",
                "", (unsigned long long)r.chunk_ex, (unsigned long long)r.chunk_af, (unsigned long long)r.q_evict,
                (unsigned long long)r.reg_attempts, (unsigned long long)r.reg_ok, (unsigned long long)r.reg_failed);
}
static void wait_recv(void* pub_opaque)
{
    for (int i = 0; i < 60; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); (void)pub_opaque; }
}

int main()
{
    dzIPC::EnableNodelet(false);            /* 关掉本进程快速路径，确保走 SHM/DZFlat 路径 */
    CounterRegistry::instance().set_diagnostics_enabled(false);

    /* ---------- 臂 1：(a) 路径选择 —— DZFlat 关闭（类型支持的标准消息） ---------- */
    {
        dzIPC::EnableDzFlat(false);
        dzIPC::ResetDzFlatCounters();
        CounterRegistry::instance().reset();
        const std::string topic = unique_topic("a_off");
        auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto std_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{ptd, topic, kDomain};
        dzIPC::shm::shm_sub_ipc sub{std_, topic, kDomain, 32};
        pub.InitChannel(); sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (int i = 0; i < 30; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdImage>();
            m->set_msg_id(kMsgId); m->width = 8; m->height = 8; m->step = 8; m->encoding = "rgb8";
            m->data.resize(64, (std::uint8_t)i);
            pub.publish(m);
        }
        dump("ARM1 OFF(TLV 30)", read_row());
    }

    /* ---------- 臂 2：(b) 真回退 —— DZFlat 开启 + 类型不支持(GenericMessage) ---------- */
    {
        dzIPC::EnableDzFlat(true);
        dzIPC::ResetDzFlatCounters();
        CounterRegistry::instance().reset();
        const std::string topic = unique_topic("b_unsup");
        auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), kMsgId);
        auto std_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{ptd, topic, kDomain};
        dzIPC::shm::shm_sub_ipc sub{std_, topic, kDomain, 32};
        pub.InitChannel(); sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (int i = 0; i < 20; ++i)
        {
            auto m = std::make_shared<dzIPC::GenericMessage>();
            m->set_uint32("v", static_cast<std::uint32_t>(i));
            pub.publish(m);
        }
        dump("ARM2 ON+unsupported 20", read_row());
    }

    /* ---------- 臂 3：(c) B 借样失败 —— 超变长预算 finalize 失败 ---------- */
    {
        dzIPC::EnableDzFlat(true);
        dzIPC::ResetDzFlatCounters();
        CounterRegistry::instance().reset();
        const std::string topic = unique_topic("c_bfail");
        auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto std_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{ptd, topic, kDomain};
        dzIPC::shm::shm_sub_ipc sub{std_, topic, kDomain, 32};
        pub.InitChannel(); sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (int i = 0; i < 10; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(256);
            if (!lo.valid()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            (void)lo->alloc_data(8192);          /* 超预算 ⇒ !ok */
            (void)pub.publish_loaned(std::move(lo));   /* 必失败 ⇒ (c) 组 */
        }
        dump("ARM3 B over-budget 10", read_row());
    }

    /* ---------- 臂 4：对照 —— B 借样成功不得进任何 fallback ---------- */
    {
        dzIPC::EnableDzFlat(true);
        dzIPC::ResetDzFlatCounters();
        CounterRegistry::instance().reset();
        const std::string topic = unique_topic("d_bok");
        auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto std_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{ptd, topic, kDomain};
        dzIPC::shm::shm_sub_ipc sub{std_, topic, kDomain, 32};
        pub.InitChannel(); sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (int i = 0; i < 10; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(4096);
            if (!lo.valid()) continue;
            (void)lo->alloc_data(64);
            (void)pub.publish_loaned(std::move(lo));
        }
        dump("ARM4 B ok 10", read_row());
    }
    /* ---------- 臂 5：B 借样池耗尽（借满不发布）⇒ 看 borrow_failed_* 与 t35 家族是否互不重复 ---------- */
    {
        dzIPC::EnableDzFlat(true);
        dzIPC::ResetDzFlatCounters();
        CounterRegistry::instance().reset();
        const std::string topic = unique_topic("e_pool");
        auto ptd = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto std_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{ptd, topic, kDomain};
        dzIPC::shm::shm_sub_ipc sub{std_, topic, kDomain, 32};
        pub.InitChannel(); sub.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdImageFlat>> held;
        for (int i = 0; i < 60; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(1024);
            if (!lo.valid()) break;
            (void)lo->alloc_data(64);
            held.push_back(std::move(lo));       /* 持有不发布 ⇒ 顶满该档池 */
        }
        /* 顶满后再借 5 次：loan() 返回无效。
         * ⚠️ 诚实标注：**产品路径不在这里调分类器** —— shm_pub_ipc::loan<Flat>() 只在
         * 返回 {} 前什么都没记（属已登记的 W19-F2：borrow_failed_pool_exhausted 无写入点）。*/
        int invalid = 0;
        for (int i = 0; i < 5; ++i)
        {
            auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(1024);
            if (!lo.valid()) ++invalid;
        }
        dump("ARM5 B pool exhausted", read_row());
        std::printf("ARM5 loan_invalid=%d (产品路径不记 borrow_failed_*：W19-F2 未闭环)\n", invalid);
        std::printf("ARM5 held=%zu\n", held.size());
    }
    (void)&wait_recv;
    std::printf("T41_ARMS_DONE\n");
    return 0;
}
