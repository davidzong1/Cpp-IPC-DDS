#pragma once
/* SHM 接收侧的双 wire 判别 —— pub/sub 与 ser/cli 共用一份。
 *
 * 为什么要共用: 两条路径原本各自内联了同样的判别逻辑。判别本身不难, 难的是它有四种
 * 拒收出口且每一种都必须记到**同一组**计数器上; 两份实现意味着两份埋点, 迟早漂移成
 * "一条路径的丢弃看得见, 另一条看不见"。而看不见的那条恰好就是这套计数要解决的问题。
 */
#include <cstdint>

#include "dzIPC/common/nodelet_config.h"
#include "ipc_msg/ipc_msg_base/ipc_msg_base.hpp"
#include "libipc/buffer.h"
#include "libipc/export.h"

namespace dzIPC {

/* 判别 raw 是 DZFlat 段还是 TLV, 校验 msg_id 与结构, 解码进 sink。
 *
 * 返回 false 表示这条应当**整条丢弃**。四种拒收各自计数(见 DzFlatRxStats):
 *   msg_id 不符      → 正常过滤(同一通道上多种消息混跑时本就该跳过)
 *   DZFlat 结构不符  → 版本错配
 *   TLV 读越界       → 缓冲损坏
 *
 * 判别必须**在 msg_id 校验之前**做, 因为两种 wire 的 msg_id 在不同位置: DZFlat 在段头
 * 字段, TLV 在页尾最后 4 字节。DZFlat 段的尾部是借来的 chunk 里没写到的部分, 拿去做
 * TLV 校验必然是垃圾。
 *
 * expected_msg_id 单独传而不是取 sink 自己的: pub/sub 侧的 sink 是会被 swap() 移走的
 * 那个对象, 只有 TopicData 始终持有本话题的 msg_id。
 */
IPC_EXPORT bool AcceptWire(const ipc::buffer& raw, std::uint32_t expected_msg_id, IpcMsgBase& sink);

}   // namespace dzIPC
