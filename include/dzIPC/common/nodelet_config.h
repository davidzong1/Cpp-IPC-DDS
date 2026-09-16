#pragma once

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
