/* 控制面段名回归: exec/dzipc_topic_cat 推导的名字必须与**传输层实际建出的段**一致
 *
 * 背景(修复前的形态): shm_sniffer.cc 自己拼控制面段名, 与传输层三处不符 ——
 *   ① 缺 `d<domain>_` 前缀  ② ser 侧传输层不做 sanitize  ③ 后缀少一个 "2"
 * 危害不是"打不开"而是静默失效: TopicControlPlane::open() 内部是
 * ipc::shm::handle::acquire(name, size, create|open) —— 段名错了它**不报错**, 只在
 * /dev/shm 建一个谁都不映射的空壳, 于是 generation() 恒 0 / state() 恒 Empty,
 * shm_sniffer 的"发布端重建后重挂"永久不生效, 且每次运行留一个垃圾段。
 *
 * 判据取向(与 docs/shm_defect_fixes.md §1 的教训一致): **断言传输层实际建出的名字**,
 * 而不是把本工具拼的字符串与另一份字符串比。所以这个用例先起真实的 shm_pub_ipc /
 * shm_ser_ipc, 再检查:
 *   1. 本工具推导的名字在 /dev/shm 里**确实存在**(说明它指向真段);
 *   2. 修复前那份复刻名**不存在**(证明旧名从来是错的, 且工具没有再建它);
 *   3. 只读探测函数对不存在的段返回 false 且**不建段**(这是"避免 create|open 生成
 *      垃圾段"的直接判据)。
 * 变异对照: 若把 control_plane_name() 改回复刻写法, 断言 1 立刻失败 —— 见本文件末的
 * 注释(已用旧名常量 OldReplicaName 固定住该形态)。
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include <gtest/gtest.h>

/* 直接引用**被测工具自己的**命名实现(exec/ 不在 test 的 include 路径里, 用工程根相对路径),
 * 这样本用例咬住的是"工具推导出的名字", 而不是把另一份字符串再复刻一遍。 */
#include "exec/dzipc_topic_cat/include/control_plane_naming.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/topic_data.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {

using dzipc_topic_cat::control_plane_name;
using dzipc_topic_cat::control_plane_segment_exists;

std::string shm_path(const std::string& segment)
{
    return "/dev/shm/" + segment;
}

bool segment_on_disk(const std::string& segment)
{
    struct stat st;
    return ::stat(shm_path(segment).c_str(), &st) == 0;
}

/* 修复前的复刻字符串 —— 保留它作为**反面对照**: 任何时候它出现在 /dev/shm 里,
 * 都说明有人又照着旧写法建了段。 */
std::string old_replica_name(const std::string& topic, bool ser)
{
    /* 复刻原实现: "dz_ipc_" + sanitize(topic) + ("_ser_control" | "_topic_control") */
    std::string sanitized;
    for (char ch : topic)
    {
        const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        sanitized.push_back(alnum || ch == '_' || ch == '-' || ch == '.' ? ch : '_');
    }
    return "dz_ipc_" + sanitized + (ser ? "_ser_control" : "_topic_control");
}

void remove_segment(const std::string& segment)
{
    ::remove(shm_path(segment).c_str());
}

/* 带 domain 与**会被 sanitize 改写**的字符的 topic: 同时压住"缺 d<domain>_"与
 * "ser 侧不做 sanitize"两条偏差。
 *
 * ⛔ 刻意不用 '/': libipc 的 shm_open 名字里带 '/' 会直接 EINVAL(实测 errno=22),
 * 于是 ser 侧根本建不出段, 用例会死于无关原因而不是死于命名。'@' 合法且会被
 * sanitize 成 '_' —— pub/sub 真名用 sanitize 后的形态, ser 真名保留 '@', 两者
 * 与旧复刻名三者互不相同, 判别力足够。 */
constexpr const char* kTopic = "sniffer_naming@depth";
constexpr std::size_t kDomain = 3;
constexpr std::uint32_t kMsgId = 77;

}   // namespace

/* ------------------------------------------------------------------------- *
 * 1. pub/sub(topic) 模式: 工具推导名 == 真实发布端建出的控制面段
 * ------------------------------------------------------------------------- */
TEST(ShmSnifferControlName, TopicModeMatchesPublisherSegment)
{
    const std::string expected = control_plane_name(kTopic, kDomain, /*ser_or_topic=*/false);
    const std::string replica = old_replica_name(kTopic, /*ser=*/false);

    /* 先确认这是个**干净开始**: 两者都不应在盘上。 */
    ASSERT_FALSE(segment_on_disk(expected)) << "前置不干净: " << expected << " 已存在, 请清理 /dev/shm";
    ASSERT_FALSE(segment_on_disk(replica)) << "前置不干净: 旧复刻名 " << replica << " 已存在";

    {
        auto topic_data = std::make_shared<dzIPC::TopicData>(
            std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc publisher(topic_data, kTopic, kDomain, /*verbose=*/false);
        publisher.InitChannel();   // 建控制面段 + 数据段

        /* 判据 1: 工具推导的名字指向的是**真实建出来的段**。 */
        EXPECT_TRUE(segment_on_disk(expected))
            << "工具推导的控制面名 '" << expected << "' 在 /dev/shm 中不存在 —— "
               "说明它与传输层实际派生不符(这正是修复前的静默失效)";

        /* 判据 2: 旧复刻名不存在 —— 修复前那条路径从来没指对过, 也不该被重新建出来。 */
        EXPECT_FALSE(segment_on_disk(replica))
            << "旧复刻名 '" << replica << "' 出现在 /dev/shm —— 有人又照旧写法建了段";

        /* 判据 3: 只读探测对**存在**的段返回 true, 且不改动它(不 ftruncate/不删)。 */
        EXPECT_TRUE(control_plane_segment_exists(expected));
        EXPECT_TRUE(segment_on_disk(expected)) << "只读探测不应删除/改名段";

        /* 判据 4: 产品侧导出点(传输层与工具现在共同的出处)算出来的名字, 就是发布端
         * 真的建出来那个段 —— 后缀收口之后, 判据 1 的"工具名"与本条走的已是同一个
         * 函数, 这里把它显式写出来: 传输层自己也不许再拼后缀。 */
        EXPECT_EQ(shm_topic_control_name(kTopic, kDomain), expected);
        EXPECT_TRUE(segment_on_disk(shm_topic_control_name(kTopic, kDomain)))
            << "产品侧导出名在 /dev/shm 中不存在 —— 传输层没有转调它(说明又有人拼了后缀)";
    }

    /* 析构后段可能仍在(引用计数语义), 这里只负责把测试自己的痕迹清掉。 */
    remove_segment(expected);
    remove_segment(replica);
    remove_segment(shm_topic_segment_name(kTopic, kDomain));
}

/* ------------------------------------------------------------------------- *
 * 2. ser(cli 服务) 模式: 同样对齐, 且覆盖"ser 侧不做 sanitize"这条偏差
 * ------------------------------------------------------------------------- */
TEST(ShmSnifferControlName, SerModeMatchesServerSegment)
{
    const std::string expected = control_plane_name(kTopic, kDomain, /*ser_or_topic=*/true);
    const std::string replica = old_replica_name(kTopic, /*ser=*/true);

    /* 真名与旧复刻名必须**不同**(domain 前缀 + 不 sanitize + "_control2"), 否则本用例
     * 根本咬不住那个 bug。 */
    ASSERT_NE(expected, replica);
    ASSERT_NE(expected.find("d3_"), std::string::npos) << "ser 控制面名必须带 domain 前缀: " << expected;
    ASSERT_NE(expected.find("_ser_control2"), std::string::npos) << "后缀应是 _ser_control2: " << expected;

    remove_segment(expected);
    remove_segment(replica);

    {
        auto sd = std::make_shared<dzIPC::ServiceData>(
            std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
            std::make_shared<dzIPC::Srv::RequestResponseTestResponse>(), kMsgId);
        dzIPC::shm::shm_ser_ipc server(
            kTopic, sd,
            [](std::shared_ptr<dzIPC::ServiceData>&) {},
            kDomain, /*verbose=*/false);
        server.InitChannel();

        EXPECT_TRUE(segment_on_disk(expected))
            << "工具推导的服务控制面名 '" << expected << "' 不存在 —— 与传输层派生不符";
        EXPECT_FALSE(segment_on_disk(replica))
            << "旧复刻名 '" << replica << "' 出现在 /dev/shm —— 有人又照旧写法建了段";
        EXPECT_TRUE(control_plane_segment_exists(expected));
    }

    remove_segment(expected);
    remove_segment(replica);
    remove_segment(shm_service_prefix(kTopic, kDomain) + "_ser_r");
    remove_segment(shm_service_prefix(kTopic, kDomain) + "_ser_w");
}

/* ------------------------------------------------------------------------- *
 * 3. 只读探测绝不建段 —— 这是"避免 create|open 生成垃圾段"的直接判据
 * ------------------------------------------------------------------------- */
TEST(ShmSnifferControlName, ProbeNeverCreatesSegment)
{
    const std::string bogus = "dz_ipc_naming_probe_never_creates_control2";
    remove_segment(bogus);
    ASSERT_FALSE(segment_on_disk(bogus));

    EXPECT_FALSE(control_plane_segment_exists(bogus)) << "段不存在时应返回 false";
    EXPECT_FALSE(segment_on_disk(bogus))
        << "只读探测**建出了段** —— 说明它退回了 create|open, 正是垃圾段的来源";

    /* 旧复刻名同样不该被建出来(即使 topic 名合法, 走旧写法就会造出一个错段)。 */
    const std::string replica = old_replica_name("naming_probe_never_creates", /*ser=*/false);
    remove_segment(replica);
    ASSERT_FALSE(segment_on_disk(replica));
    (void)control_plane_segment_exists(replica);
    EXPECT_FALSE(segment_on_disk(replica)) << "探测旧复刻名时建出了段";
    remove_segment(replica);
    remove_segment(bogus);
}
