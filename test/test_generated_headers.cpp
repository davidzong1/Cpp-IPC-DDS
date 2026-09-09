/* 编译全部生成头文件 —— 让 generator 的缺陷无处潜伏。自动生成, 勿手改。
 *
 * 生成的消息头没有任何产品 TU 会编译, 只有测试用到的那几个类型才被实例化。于是
 * generator 对某个字段形态的 bug 可以长期潜伏: DZFlat 落地时就撞上一个 ——
 * std_matrix_3d 的 StdVector3d[3](定长嵌套数组)对应 std::array<>, 而生成的
 * copy_to 调了 resize(), 编译不过; 因为没人编译它, 前三个 Step 全绿。
 *
 * 本文件把所有生成头文件拉进来编译一遍, 于是每个类型的这些东西都被覆盖:
 *   - Root 布局 static_assert(generator 的字面量偏移 vs 真实 ABI, 也即 Python
 *     侧 DZFlat 解码器所用偏移的正确性);
 *   - 每个字段形态的 View / Builder / copy_to 能否编译;
 *   - kSchemaHash / kRootTight 能否在编译期求值。
 *
 * 见 docs/dzflat_shm.md §9.6。
 */
#include <cstdint>
#include <vector>

#include "ipc_msg/std_msgs/std_color.hpp"
#include "ipc_msg/std_msgs/std_double.hpp"
#include "ipc_msg/std_msgs/std_float.hpp"
#include "ipc_msg/std_msgs/std_header.hpp"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/std_msgs/std_marker.hpp"
#include "ipc_msg/std_msgs/std_matrix.hpp"
#include "ipc_msg/std_msgs/std_matrix_3d.hpp"
#include "ipc_msg/std_msgs/std_path.hpp"
#include "ipc_msg/std_msgs/std_point_cloud.hpp"
#include "ipc_msg/std_msgs/std_pose.hpp"
#include "ipc_msg/std_msgs/std_quaternion.hpp"
#include "ipc_msg/std_msgs/std_raw_message.hpp"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "ipc_msg/std_msgs/std_tf.hpp"
#include "ipc_msg/std_msgs/std_vector.hpp"
#include "ipc_msg/std_msgs/std_vector_3d.hpp"
#include "ipc_msg/test_messages/complex_message.hpp"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "ipc_msg/test_nested/pose.hpp"
#include "ipc_msg/test_nested/robot_state.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"
#include "ipc_srv/std_srv/std_bool.hpp"
#include "ipc_srv/std_srv/std_double.hpp"
#include "ipc_srv/std_srv/std_float.hpp"
#include "ipc_srv/std_srv/std_int.hpp"
#include "ipc_srv/std_srv/std_string.hpp"

#include "gtest/gtest.h"

/* 编译本身就是断言。下面再抽查两条关键不变式, 让用例有可观测输出。 */
TEST(GeneratedHeaders, SchemaHashesAreNonZeroAndStructureSensitive)
{
    EXPECT_NE(dzIPC::Msg::StdImageFlat::kSchemaHash, 0u);
    EXPECT_NE(dzIPC::Msg::StdPointCloudFlat::kSchemaHash, 0u);
    EXPECT_NE(dzIPC::Msg::StdMatrix3dFlat::kSchemaHash, 0u);
    EXPECT_NE(dzIPC::Msg::StdImageFlat::kSchemaHash,
              dzIPC::Msg::StdPointCloudFlat::kSchemaHash);
    EXPECT_NE(dzIPC::Msg::StdMatrix3dFlat::kSchemaHash,
              dzIPC::Msg::StdVector3dFlat::kSchemaHash);
}

/* 定长嵌套数组(std::array<Msg,N>) —— 正是上面那个潜伏 bug 的形态。 */
TEST(GeneratedHeaders, FixedNestedArrayRoundTrips)
{
    dzIPC::Msg::StdMatrix3d m;
    for (std::size_t i = 0; i < m.rot.size(); ++i)
        m.rot[i].data = {double(i), double(i) + 0.5, double(i) + 0.25};

    const std::uint32_t cap = dzIPC::Msg::StdMatrix3dFlat::size(m);
    std::vector<std::uint8_t> seg(cap);
    ASSERT_TRUE(dzIPC::Msg::StdMatrix3dFlat::write(m, seg.data(), cap));
    const auto* h = reinterpret_cast<const dzflat::SegHeader*>(seg.data());

    auto v = dzIPC::Msg::StdMatrix3dView::bind(seg.data(), h->total_size);
    ASSERT_TRUE(v.valid());
    ASSERT_EQ(v.rot_count(), m.rot.size());

    dzIPC::Msg::StdMatrix3d back;
    v.copy_to(back);
    for (std::size_t i = 0; i < m.rot.size(); ++i)
    {
        EXPECT_EQ(back.rot[i].data, m.rot[i].data) << "rot[" << i << "]";
    }
}
