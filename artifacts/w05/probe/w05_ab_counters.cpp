/* W05 验收探针（非交付物；证据用）—— DZFlat A/B/三路径分流计数在 W05 重构后仍可分。
 *
 * 判据：DZFlat 关闭 ⇒ tlv=20 且 a=b=0；dzflat-a ⇒ a=20 且 tlv=b=0；
 *       dzflat-b ⇒ b=20 且 tlv=a=0。两条驱动臂（调度器 / 兼容回退）都必须成立。
 *
 * 编译：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_ab_counters.cpp \
 *       -o artifacts/w05/scratch-probe/w05_ab_counters -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

constexpr std::uint32_t kMsgId = 93;

static std::size_t counter(dzIPC::measure::CounterId id)
{
    return dzIPC::measure::CounterRegistry::instance().get(id);
}

int main(int argc, char** argv)
{
    const std::string mode = (argc > 1) ? argv[1] : "tlv";
    const std::string topic = "w05ab_" + mode;
    const std::size_t domain = 71;
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId); };

    if (mode != "tlv")
    {
        dzIPC::EnableDzFlat(true);
    }
    dzIPC::ResetDzFlatCounters();

    dzIPC::shm::shm_sub_ipc sub{td(), topic, domain, 16, false};
    sub.InitChannel();
    dzIPC::shm::shm_pub_ipc pub{td(), topic, domain, false};
    pub.InitChannel();
    for (int i = 0; i < 300 && !pub.has_subscribed(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!pub.has_subscribed())
    {
        std::printf("%s: NO_HANDSHAKE\n", mode.c_str());
        return 2;
    }

    constexpr int kN = 20;
    int ok = 0;
    for (int i = 0; i < kN; ++i)
    {
        auto img = std::make_shared<dzIPC::Msg::StdImage>();
        img->set_msg_id(kMsgId);
        img->width = 8;
        img->height = 4;
        img->step = 24;
        img->encoding = "rgb8";
        img->data.assign(96, static_cast<std::uint8_t>(i + 1));
        if (mode == "dzflat-b")
        {
            auto lo = pub.loan<dzIPC::Msg::StdImageFlat>(96 + 256);
            if (!lo.valid())
            {
                std::printf("%s: LOAN_INVALID\n", mode.c_str());
                return 3;
            }
            lo->set_width(8);
            lo->set_height(4);
            lo->set_step(24);
            lo->set_encoding("rgb8");
            auto px = lo->alloc_data(96);
            if (px.size() != 96)
            {
                std::printf("%s: ALLOC_SHORT\n", mode.c_str());
                return 4;
            }
            for (std::size_t k = 0; k < px.size(); ++k)
            {
                px.data()[k] = static_cast<std::uint8_t>(i + 1);
            }
            if (pub.publish_loaned(std::move(lo), 0))
            {
                ++ok;
            }
        }
        else
        {
            if (pub.publish(img))
            {
                ++ok;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::printf("%s: publish_ok=%d tlv=%zu a=%zu b=%zu prebuilt=%zu dzflat_pub=%llu fallback=%llu\n", mode.c_str(),
                ok, counter(dzIPC::measure::CounterId::tlv_messages),
                counter(dzIPC::measure::CounterId::dzflat_a_messages),
                counter(dzIPC::measure::CounterId::dzflat_b_messages),
                counter(dzIPC::measure::CounterId::path_selection_dzflat_disabled),
                static_cast<unsigned long long>(dzIPC::DzFlatPublishCount()),
                static_cast<unsigned long long>(dzIPC::DzFlatFallbackCount()));
    return 0;
}
