#pragma once

/* exec/dzipc_topic_cat 的控制面段名 —— 本工具的**唯一**转调点。
 *
 * ⛔ 这里以前是**复刻字符串**:
 *      "dz_ipc_" + sanitize_topic_name(topic) + "_ser_control"    // ser/cli
 *      "dz_ipc_" + sanitize_topic_name(topic) + "_topic_control"  // pub/sub
 *    与传输层的实际派生方式三处不符: ①缺 `d<domain>_` 前缀 ②ser 侧传输层**不做**
 *    sanitize(见 name_operator.cc 的 shm_service_prefix 注释) ③后缀少一个 "2"。
 *
 *    危害不是"打不开", 而是**静默失效**: TopicControlPlane::open() 内部是
 *    ipc::shm::handle::acquire(name, size, create|open)(control_plane.cc:89),
 *    段名错了它不报错, 只在 /dev/shm 新建一个谁都不映射的空段 —— generation() 恒 0、
 *    state() 恒 Empty, 于是 shm_sniffer::reopen_if_generation_changed() 永久提前返回,
 *    "发布端重建后重挂"这条能力**从未生效过**, 且每次运行都留一个垃圾段。
 *
 * 现在的规则: 段名一律从产品侧的导出点取, 本文件不保留任何字符串规则。
 *   - ser/cli : dzIPC::shm::ser_service_control_name(topic, domain)
 *               声明在 include/dzIPC/shm_ser_cli_ipc.h:27, 定义在 shm_ser_cli_ipc.cc:163
 *               —— 传输层为"占用判定"已经导出的唯一出处, 直接转调。
 *   - pub/sub : dzIPC::shm_topic_control_name(topic, domain)
 *               声明在 include/dzIPC/common/name_operator.h, 定义在
 *               src/dzIPC/common/name_operator.cc —— 原先这个后缀只存在于
 *               shm_pub_sub_ipc.cc 的匿名 namespace(control_name_for, static), 于是
 *               每个需要它的地方都只能复刻一遍; 已按 ser 侧同样的办法导出, 本工具与
 *               传输层现在转调同一个函数。
 *   test_shm_sniffer_control_name.cpp 仍对**真实发布端/服务端建出的段**做断言 ——
 *   两侧一旦改名/改后缀, 该用例立刻红。
 */

#include <cstddef>
#include <string>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "libipc/shm.h"

namespace dzipc_topic_cat {

/// topic(发布订阅) / ser(cli 服务) 两种模式的控制面段名。
inline std::string control_plane_name(const std::string& topic_name, std::size_t domain_id, bool ser_or_topic)
{
    if (ser_or_topic)
    {
        return dzIPC::shm::ser_service_control_name(topic_name, domain_id);
    }
    return shm_topic_control_name(topic_name, domain_id);
}

/* 控制面段是否已存在 —— **只读探测, 绝不建段**。
 *
 * 为什么不能拿 TopicControlPlane::open() 当探测: 它是 create|open, 段不存在时会
 * 建一个空壳(见文件头注释的"静默失效")。这里用 libipc 的 open 分支:
 * shm_posix.cpp 在 mode==open 时把 size 归零 ⇒ 既不 ftruncate 也不建段, 段不存在
 * 时 shm_open 返回 ENOENT ⇒ acquire 返回 nullptr。
 *
 * 手法与 control_plane_shm::occupied_by_other()(control_plane.cc:311)一致:
 * 必须配套调用 get_mem(它把引用计数 +1 并 mmap), 再用 release_no_unlink 配平 ——
 * 刻意**不**用 release(): 后者在引用计数归零时会 shm_unlink, 那正是这里要防的事。 */
inline bool control_plane_segment_exists(const std::string& name)
{
    ipc::shm::id_t id = ipc::shm::acquire(name.c_str(), 0, ipc::shm::open);
    if (id == nullptr)
    {
        return false;   // ENOENT: 段不存在(这是唯一返回 false 的情形)
    }
    std::size_t mapped = 0;
    (void)ipc::shm::get_mem(id, &mapped);
    ipc::shm::release_no_unlink(id);
    return true;
}

}   // namespace dzipc_topic_cat
