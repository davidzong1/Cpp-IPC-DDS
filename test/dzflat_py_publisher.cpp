/* DZFlat 的 Python 侧验收用 C++ 发布端 (docs/dzflat_shm.md §9.6)
 *
 * 为什么还要一个 C++ 发布端: Python 侧的读写两条路都归 python/dzipc(§9.7 的
 * publish_dzflat 写、dzflat.decode 读), 同类对同类时"两边一起错"是可能的 —— 比对基准
 * 必须来自**另一套实现**。这里写的段由 C++ 的 dzflat::Writer 产出, 正是 §9.4 的原始
 * 实现, 于是 test_dzflat_python.py 的 [9] 能把 Python 编码器与它逐字节对上。
 *
 * 用法: dzflat_py_publisher <topic> <msg_id> <dzflat:0|1> <kind:image|cloud|robot> [秒数]
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
#include "ipc_msg/test_nested/robot_state.hpp"

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

/* RobotState: 覆盖 string / 嵌套消息 / 嵌套消息数组 / note(JSON 字符串,
 * dzviz 的 _unwrap_joint_state 从这里解 joint_state)四种形态。
 * 内容写死, 与 tools/dzviz/test/test_dzflat_live_e2e.py 的 EXPECT_* 共享。 */
dzIPC::Msg::RobotState make_robot()
{
    dzIPC::Msg::RobotState m;
    m.name = "arm_left";
    m.current_pose.x = 1.5;
    m.current_pose.y = -2.25;
    m.current_pose.z = 0.125;
    m.current_pose.frame_id = "base_link";
    m.pose_history.resize(3);
    for (std::size_t i = 0; i < m.pose_history.size(); ++i)
    {
        m.pose_history[i].x = double(i);
        m.pose_history[i].y = double(i) * 10.0;
        m.pose_history[i].z = double(i) * 100.0;
        m.pose_history[i].frame_id = "wp";
    }
    m.note = "{\"joint_state\":{\"name\":[\"j1\",\"j2\"],\"position\":[0.25,-0.5],"
             "\"velocity\":[1.0,2.0],\"effort\":[0.0,0.0]}}";
    return m;
}

}   // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr,
                     "用法: %s <topic> <msg_id> <dzflat:0|1> <kind:image|cloud|robot> [秒数]\n",
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
    else if (kind == "robot")
        td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::RobotState>(), msg_id);
    else
        td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), msg_id);

    dzIPC::shm::shm_pub_ipc pub{td, topic, 0};
    pub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const auto img = make_image();
    const auto cloud = make_cloud();
    const auto robot = make_robot();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (kind == "cloud")
        {
            auto m = std::make_shared<dzIPC::Msg::StdPointCloud>(cloud);
            m->set_msg_id(msg_id);
            pub.publish(m);
        }
        else if (kind == "robot")
        {
            auto m = std::make_shared<dzIPC::Msg::RobotState>(robot);
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
