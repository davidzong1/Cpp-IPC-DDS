#pragma once

#include <cstddef>
#include <cstdint>

#include "libipc/export.h"

namespace dzIPC {

// ---------------------------------------------------------------------------
// DZFlat 平坦布局开关(进程级, 默认 OFF)。设计见 docs/dzflat_shm.md。
//
// 开启后, **SHM 发布路径**在满足下列条件时改用 DZFlat wire:
//   - 消息类型由 generator 生成(dzflat_supported() 为真; 手写类型自动留在 TLV);
//   - 该通道当前有接收方(无接收方时 chunk 借不到, 见 ipc::chan_wrapper::loan);
//   - chunk 池未耗尽(每尺寸档位 32 块; 借不到就回退整包序列化)。
// 任一条件不满足都会静默回退到既有 TLV 路径, 行为不变。
//
// 订阅侧**始终**同时认两种 wire(按段首 magic 判别, 见 dzflat::looks_like_dzflat),
// 与本开关无关 —— 这样先升级订阅方、再升级发布方即可安全灰度。
//
// 为什么默认 OFF: DZFlat 段对**未升级**的订阅方是不可解析的(它会按 TLV 读段尾的
// msg_id, 几乎必然失配而丢弃 —— 不会错解, 但会静默丢消息)。因此必须在确认链路
// 两端都是新版本之后, 由部署方显式打开。
//
// socket/UDP 路径不受影响: 其分片格式(1460+12 页尾)由 data_rev.cc 消费, 见
// docs/dzflat_shm.md §1.4。
// ---------------------------------------------------------------------------
IPC_EXPORT void EnableDzFlat(bool enabled);
IPC_EXPORT bool IsDzFlatEnabled();

// DZFlat 发布计数(进程级, 单调累加)。
//
// 存在的理由: DZFlat 的回退是**静默**的 —— 开关开着但类型不支持、无接收方、chunk
// 池耗尽时都会回落 TLV 且照常送达。没有计数就无法区分"收益生效了"和"一直在回退",
// 无论是线上排查还是测试断言都会变成猜测。
//
//   dzflat = 走借样平坦布局发出的条数
//   fallback = 尝试过但回落整包序列化的条数(含开关关闭时的直接回落)
//
// 生产用途: fallback/(dzflat+fallback) 持续偏高说明 chunk 持有预算被突破
// (见 docs/dzflat_shm.md §5.3 的 32 槽天花板), 或链路上混着未升级的消息类型。
IPC_EXPORT std::uint64_t DzFlatPublishCount();
IPC_EXPORT std::uint64_t DzFlatFallbackCount();
IPC_EXPORT void ResetDzFlatCounters();

// ---------------------------------------------------------------------------
// view 队列容量钉(进程级, 默认 ON)。设计见 docs/shm_chunk_pool_occupancy_plan.md §3 步骤③。
//
// 要解决的问题: SHM 订阅侧的 view 队列持有的是**借样** Sample —— 每个 Sample 里的
// buff_t 让一块 chunk 的引用保持非零, 应用读完字段前该 chunk 不回池。而 chunk 池
// 每尺寸档只有 ipc::large_msg_cache(=32) 块, 且 dzIPC 层建 route 不带 prefix ⇒
// **同尺寸档全机一池**。调用方若把 queue_size 配得比池大(常见值 1024), 队列就能把
// 整池吃干: 此后发布侧 loan 拿不到块 ⇒ 回退整包 TLV(DzFlatFallbackCount 上升),
// 零拷贝收益归零。步骤② 实测坐实: 池占用 L == min(queue_size, 32), 队列是唯一
// 主导者(通道 A 外部直读与通道 B 插桩直读逐点精确相等)。
//
// 钉法: view_queue_ 容量 = min(queue_size, ViewQueueCap()); 默认
// ViewQueueCap() = ipc::large_msg_cache / 4 = 8, 留 24 块头寸给环内在飞、
// 同进程其他话题、同机其他进程。
//
// 射程(为什么只钉 SHM 的 view 队列):
//   - view_queue_ 只在"DZFlat + typed"的借样路径被 push; TLV 与 schema-less 走
//     **物化**的 msg 队列 ⇒ 钉它不影响非 DZFlat 话题, 也不影响 msg 队列深度,
//     所以 msg 队列刻意**不**钉(缩它只是白减应用缓冲)。
//   - socket/UDP 侧也有 view 队列, 但它的 Sample 持有的是接收层去帧出来的**独立
//     堆块**(见 socket_pub_sub_ipc.cc 的借样注释), 不占 chunk 池 ⇒ 不需要钉。
//
// ⚠️ 这是对调用方**显式传入**的 queue_size 的覆盖(该参数在 SubscriberIPCPtrMake
// 里是必填、无默认值), 所以覆盖必须可观测: 生效时 shm_sub_ipc 构造会打一条一次性
// stderr 诊断(按被请求的 queue_size 去重), 不静默。
//
// ⚠️ 单订阅者钉住 ≠ 全机不耗尽: 4 个同尺寸档订阅者各钉 8 块仍会用满 32。跨进程
// 隔离要靠 prefix(见 docs/unfixed_defects.md UF-003), 不在本开关射程内。
//
// 生效时机: 与 EnableDzFlat 一样是进程级, 但容量在**订阅者构造时**读取 ——
// 因此只影响之后新建的订阅者, 已建的不变。
// ---------------------------------------------------------------------------
IPC_EXPORT void EnableViewQueuePin(bool enabled);
IPC_EXPORT bool IsViewQueuePinEnabled();
/// 钉生效时 view 队列容量的上限(与具体 queue_size 无关, 取二者较小者)。
IPC_EXPORT std::size_t ViewQueueCap();

namespace detail {
/// 传输层内部埋点: 每条 SHM 发布记一次(true = 走了 DZFlat, false = 回退整包)。
IPC_EXPORT void NoteDzFlatPublish(bool used_dzflat);
}   // namespace detail

// ---------------------------------------------------------------------------
// DZFlat 接收计数(进程级, 单调累加)。
//
// 存在的理由比发布侧更要紧: 接收侧的拒收是**完全静默**的 —— 订阅循环只是 continue,
// 既不记日志也不抛错。于是"版本错配"这种一定会发生的部署事故, 现场表现是"消息量对
// 不上, 但两端都不报错", 属于最难查的形态。
//
// 六个量分成两组 wire, 每组一个"收下"两个"没收下"; 而"没收下"必须再分成**正常过滤**
// 与**真实缺陷** —— 前者是这里唯一的高频量, 不单独拿出来就会把后两个真信号淹没:
//
//   dzflat_accepted / tlv_accepted      收下的条数。拒收数没有分母无法解读, 故必须有
//   dzflat_id_skipped / tlv_id_skipped  msg_id 不是本话题 —— **正常**。同一通道上多种
//                                       消息混跑时本就该跳过, 量可能远大于其余各项
//   dzflat_header_bad                   段首 magic 对上但段头自相矛盾(total_size 超出
//                                       缓冲 / root_off 不对), 或 layout_ver 不是本
//                                       构建认识的版本 —— 前者是段被**截断或覆写**,
//                                       后者是对端用了更新的 wire 格式。两者都不是
//                                       "正常过滤", 所以不能混进 tlv_id_skipped:
//                                       坏掉的段会落进 TLV 分支, 尾部校验必然失配,
//                                       于是看起来和"这条不是我的话题"一模一样。
//                                       要分辨是哪一种, 看段里的 layout_ver 字段
//   dzflat_schema_drop                  msg_id 对上但结构不符(schema_hash / root_size
//                                       / 段长任一) —— **一定是版本错配**, 见下
//   tlv_corrupt_drop                    TLV 反序列化读越界 —— 缓冲被截断或错乱
//                                       (docs/dzflat_known_issues.md 第 3 条)
//
// 判读:
//   dzflat_schema_drop > 0  ⇒ 链路两端的 msg 定义不是同一份。DZFlat 是定长布局, 结构
//                             一变指纹就变, 老读端只能拒收(否则按错误偏移解出垃圾)。
//                             正确的升级顺序是先关 DZFlat 借道 TLV 滚 schema, 再开回来
//                             —— 见 docs/dzflat_shm.md §3.5。
//   tlv_corrupt_drop > 0    ⇒ 有缓冲损坏。TLV 的偏移全由缓冲自身内容算出, 一个错乱
//                             缓冲能把偏移推到任意远处; 这一项是那道拦截的命中次数。
//
// 注意 GenericMessage(Python 侧的载体)是**直通体**: 它没有 schema, 于是对任何格式合法
// 的段都返回成功, 永远不会计入 dzflat_schema_drop。Python 进程的版本错配发生在更上层
// (按指纹查不到 schema), 计数在 python/dzipc/dzflat.py 的 rx_stats()。
// ---------------------------------------------------------------------------
struct DzFlatRxStats
{
    std::uint64_t dzflat_accepted = 0;
    std::uint64_t dzflat_id_skipped = 0;
    std::uint64_t dzflat_header_bad = 0;
    std::uint64_t dzflat_schema_drop = 0;
    std::uint64_t tlv_accepted = 0;
    std::uint64_t tlv_id_skipped = 0;
    std::uint64_t tlv_corrupt_drop = 0;

    /// 真实缺陷的合计(不含正常的 msg_id 过滤)。非 0 就该去查。
    std::uint64_t defects() const noexcept
    {
        return dzflat_header_bad + dzflat_schema_drop + tlv_corrupt_drop;
    }
};

IPC_EXPORT DzFlatRxStats DzFlatRxCounters();
IPC_EXPORT void ResetDzFlatRxCounters();

namespace detail {
/// 接收侧埋点的事件种类。与 DzFlatRxStats 的字段一一对应。
enum class DzFlatRxEvent
{
    kDzFlatAccepted,
    kDzFlatIdSkipped,
    kDzFlatHeaderBad,
    kDzFlatSchemaDrop,
    kTlvAccepted,
    kTlvIdSkipped,
    kTlvCorruptDrop,
    kCount,   /* 哨兵: 计数数组的长度由它推出, 增删事件不会漏改 */
};

/// 传输层内部埋点: 每条收到的 SHM 消息记一次(含被丢弃的)。
IPC_EXPORT void NoteDzFlatRx(DzFlatRxEvent ev);
}   // namespace detail

// ---------------------------------------------------------------------------
// 统一 nodelet 快速路径启用开关（进程级，默认关闭）。
//
// 启用后，每种传输方式（SHM、UDP 套接字）均可尝试使用进程内
// 快速路径。该开关是必要条件而非充分条件——各传输方式仍需验证
// 本地拓扑、K=3 稳定性等条件。
//
// 默认为 false（标准路径）。在确认同进程部署拓扑后，于初始化阶段
// 调用一次 EnableNodelet(true) 以启用。
// ---------------------------------------------------------------------------
IPC_EXPORT void EnableNodelet(bool enabled);
IPC_EXPORT bool IsNodeletEnabled();

}   // namespace dzIPC
