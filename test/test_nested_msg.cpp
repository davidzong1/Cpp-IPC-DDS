#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include "exec/dzipc_topic_cat/include/msg_type_identify.h"
#include "ipc_msg/test_nested/robot_state.hpp"

using dzIPC::Msg::Pose;
using dzIPC::Msg::RobotState;

namespace {
bool same_pose(const Pose& lhs, const Pose& rhs)
{
    return std::abs(lhs.x - rhs.x) < 1e-12 && std::abs(lhs.y - rhs.y) < 1e-12
           && std::abs(lhs.z - rhs.z) < 1e-12 && lhs.frame_id == rhs.frame_id;
}

Pose make_pose(double base, const std::string& frame_id)
{
    Pose pose;
    pose.x = base;
    pose.y = base + 1.0;
    pose.z = base + 2.0;
    pose.frame_id = frame_id;
    return pose;
}

RobotState make_state()
{
    RobotState state;
    state.name = "robot_alpha";
    state.current_pose = make_pose(1.0, "map");
    state.note = std::string(4096, 'n');
    for (int i = 0; i < 16; ++i)
    {
        state.pose_history.emplace_back(make_pose(10.0 + i, "history_frame_" + std::to_string(i)));
    }
    return state;
}
}   // namespace

int main()
{
    RobotState original = make_state();
    ipc::buffer serialized = original.serialize();

    RobotState restored;
    restored.deserialize(serialized);

    assert(restored.name == original.name);
    assert(restored.note == original.note);
    assert(same_pose(restored.current_pose, original.current_pose));
    assert(restored.pose_history.size() == original.pose_history.size());
    for (size_t i = 0; i < original.pose_history.size(); ++i)
    {
        assert(same_pose(restored.pose_history[i], original.pose_history[i]));
    }

    std::string printable = dzIPC::msg_to_string(serialized);
    assert(printable.find("current_pose:") != std::string::npos);
    assert(printable.find("pose_history: [") != std::string::npos);
    assert(printable.find("history_frame_15") != std::string::npos);

    std::cout << "nested message serialize/deserialize test passed" << std::endl;
    return 0;
}
