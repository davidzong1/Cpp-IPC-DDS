/* DZFlat 的 Python 侧验收用发布端 (docs/dzflat_shm.md Step 4)
 *
 * Python 侧无法自己发 DZFlat: GenericMessage 没有 schema, 写不出定长布局(读则由
 * python/dzipc/dzflat.py 按生成的 schema 完成)。所以验收需要一个 C++ 发布端。
 *
 * 用法: dzflat_py_publisher <topic> <msg_id> <dzflat:0|1> <kind:image|cloud> [秒数]
 * 持续发布固定内容的消息, 由 test/test_dzflat_python.py 订阅并逐字段比对。
 * 内容是**写死的**, 两侧共享同一份期望值 —— 见该 py 文件里的 EXPECT_*。
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kW = 8, kH = 4, kStep = kW * 3;
constexpr std::uint32_t kBytes = kStep * kH;   /* 96 */
constexpr std::uint32_t kPoints = 5;

dzIPC::Msg::StdImage make_image()
{
    dzIPC::Msg::StdImage m;
    m.header.frame_id = "camera_optical_frame";
    m.header.stamp = 1234.5;
    m.width = kW;
    m.height = kH;
    m.step = kStep;
    m.encoding = "rgb8";
    m.data.resize(kBytes);
    for (std::uint32_t i = 0; i < kBytes; ++i) m.data[i] = static_cast<std::uint8_t>(i * 3 % 251);
    return m;
}

dzIPC::Msg::StdPointCloud make_cloud()
{
    dzIPC::Msg::StdPointCloud m;
    m.header.frame_id = "lidar_top";
    m.header.stamp = 42.25;
    m.points.resize(kPoints);
    for (std::uint32_t i = 0; i < kPoints; ++i)
        m.points[i].data = {double(i), double(i) * 2.0, double(i) * 3.0};
    m.channel_names = {"intensity", "ring"};
    m.channels.resize(kPoints);
    for (std::uint32_t i = 0; i < kPoints; ++i) m.channels[i] = double(i) * 0.5;
    return m;
}

}   // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr,
                     "用法: %s <topic> <msg_id> <dzflat:0|1> <kind:image|cloud> [秒数]\n",
                     argv[0]);
        return 2;
    }
    const std::string topic = argv[1];
    const auto msg_id = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const bool use_dzflat = (argv[3][0] == '1');
    const std::string kind = argv[4];
    const int seconds = (argc > 5) ? std::atoi(argv[5]) : 8;

    dzIPC::EnableDzFlat(use_dzflat);

    std::shared_ptr<dzIPC::TopicData> td;
    if (kind == "cloud")
        td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdPointCloud>(), msg_id);
    else
        td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), msg_id);

    dzIPC::shm::shm_pub_ipc pub{td, topic, 0};
    pub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const auto img = make_image();
    const auto cloud = make_cloud();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (kind == "cloud")
        {
            auto m = std::make_shared<dzIPC::Msg::StdPointCloud>(cloud);
            m->set_msg_id(msg_id);
            pub.publish(m);
        }
        else
        {
            auto m = std::make_shared<dzIPC::Msg::StdImage>(img);
            m->set_msg_id(msg_id);
            pub.publish(m);
        }
        std::this_thread::sleep_for(20ms);
    }
    /* 让 Python 侧能确认发布端确实走了预期的 wire。 */
    std::printf("dzflat=%llu fallback=%llu\n",
                static_cast<unsigned long long>(dzIPC::DzFlatPublishCount()),
                static_cast<unsigned long long>(dzIPC::DzFlatFallbackCount()));
    return (use_dzflat && dzIPC::DzFlatPublishCount() == 0) ? 42 : 0;
}
