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

/* 叫醒伪影判别: 这条 buffer 是不是「被 disconnect/quit_waiting 叫醒」的产物, 而不是
 * 一条真消息。收包循环必须用它挡在分流之前。
 *
 * ---- 机理 (libipc 侧, 本阶段不改) ----
 * recv 内部是 wait_for(rd_waiter_, pred, tm)。quit_waiting() 置的 quit_ 是**粘性**的
 * (waiter.h:66 只在 open() 复位), 而 wait_if 对「quit_ 短路」与「pred 满足」返回同一个
 * true (waiter.h:104-121) —— 两者在返回值上不可区分。于是:
 *   ipc.cpp:1044  for (unsigned k = 0; pred();)  ... wait_if(...) 返回 true
 *   ⇒ wait_for 返回 true, 而 pop() **从未执行**, 局部 msg{} 保持值初始化的全零
 *   ⇒ ipc.cpp:1689  r_size = data_length(64) + remain_(0) > 0
 *   ⇒ ipc.cpp:1757  return make_cache(msg.data_, 64)
 * 结果: size() == ipc::data_length、empty() == false、**整段全零**。
 * 超时路径不同: wait_if 返回 false ⇒ recv 返回 {} (size 0) ⇒ empty() == true。
 * 所以伪影**只**来自叫醒路径, 且恒为 64 字节全零。
 *
 * ---- 为什么判据是「整段全零」而不是「尾 12 字节全零」----
 * 只扫尾部更快, 但对 size == 64 的**真 DZFlat 段**会误伤: 段头 32 字节之后是 Root 区 /
 * 变长区, 全零字段与填充都会落进去, 尾 12 字节恰好全零是可能的
 * (test_wakeup_artifact.cpp 的阴性对照用例构造的就是这种段)。而伪影是**恒**整段全零
 * —— 值初始化的 64 字节没有任何非零来源。所以整段扫描既充分(抓得住)又必要(不误伤),
 * 代价是一次最多 64 字节比较, 且只在 size() == ipc::data_length 时才扫。
 *
 * ---- 为什么不会丢真消息 ----
 * wait_for 的循环体在进入 wait_if 之前先自旋 32 轮, **每轮都调 pred()**
 * (ipc.cpp:1044)。队列里有真消息时 pred() 立即满足 ⇒ 循环退出时 msg **已填充**
 * ⇒ 返回的是真 buffer, 与本判别无关。伪影只在「队列确实空 + quit_ 已置」时产生。
 * 所以丢伪影 == 丢「本来就没有的消息」, 不会吞掉任何已弹出的字节。
 *
 * ⛔ 判据**不得**换成「lease.generation 落后就丢」—— 那会违反 RouteSession 的 I5
 * (真消息可能在 disconnect 之前就已弹出, 见 shm_route_session.h 的 I5 与说明 §3)。
 *
 * ---- 调用约定: 判别与计数分开 ----
 * 本函数是**纯判别**, 无副作用 —— 单测大量调用它做断言, 不能因为断言就污染计数。
 * 收包循环命中后须显式调 NoteWakeupArtifact() 记一笔(见 shm_pub_sub_ipc.cc 的调用点)。
 */
IPC_EXPORT bool IsWakeupArtifact(const ipc::buffer& raw) noexcept;

/* 被守门挡下的叫醒伪影条数 —— 诊断面, **不是错误计数**。
 *
 * 每次 stop_and_wake / generation 重建都会 disconnect 一次旧 route, 若那时收包线程
 * 正卡在 recv 里, 就会产生一条伪影。所以这个数正常增长 ≈ 重建次数; 它涨本身是
 * 正常路径。判据是「它涨的同时, msg_queue_/view_queue_ 里没有多出任何伪消息, 且
 * DzFlatRxStats 的任一 TLV 项不动」—— 即伪影**停在守门处**, 没走到分流。
 *
 * 单独一面计数而不并入 DzFlatRxStats: 后者是「wire 拒收」的口径(计数器回答"这条
 * wire 为什么被丢"), 而伪影根本没到 wire 判别那一步 —— 混在一起会让 defects() 与
 * kTlvAccepted/kTlvIdSkipped 的语义失真(修复前 msg_id!=0 的话题正是被记成
 * kTlvIdSkipped, 一条"不是我的话题"的正常过滤, 把缺陷伪装成了正常过滤)。
 */
IPC_EXPORT std::uint64_t WakeupArtifactCount() noexcept;
IPC_EXPORT void ResetWakeupArtifactCount() noexcept;
IPC_EXPORT void NoteWakeupArtifact() noexcept;

}   // namespace dzIPC
