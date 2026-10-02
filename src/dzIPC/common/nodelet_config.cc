#include "dzIPC/common/nodelet_config.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dzIPC/measure/counters.h"   /* W08: 路径分流计数直接落到 W03 的注册表 */
#include "libipc/def.h"

namespace dzIPC {

namespace {
std::atomic<bool> g_nodelet_enabled{false};
std::atomic<bool> g_dzflat_enabled{false};
std::atomic<std::uint64_t> g_dzflat_published{0};
std::atomic<std::uint64_t> g_dzflat_fallback{0};
/* W08: 预构造段入口(publish_prebuilt_segment)的成功条数。W03 的三路径 ID 只定义了
 * tlv / dzflat-a / dzflat-b, 没有这一入口, 故记在本模块(⛔不并进 A/B —— 它既不是
 * "对象→共享段"也不是"应用原地构造")。 */
std::atomic<std::uint64_t> g_dzflat_prebuilt{0};

/* 步骤③ 的 view 队列容量钉。默认 ON —— 理由见 nodelet_config.h 的 ⚠️ 段:
 * 默认 OFF 等于"池会被队列吃干"这个已知缺陷仍然出厂。 */
std::atomic<bool> g_view_queue_pin{true};

/* 接收侧计数。索引即 detail::DzFlatRxEvent 的枚举值 —— 用数组而不是六个具名变量,
 * 是为了让埋点成为一次数组下标自增: 增删事件种类时不会漏改分派处。 */
constexpr std::size_t kRxEventCount =
    static_cast<std::size_t>(detail::DzFlatRxEvent::kCount);
std::atomic<std::uint64_t> g_dzflat_rx[kRxEventCount] = {};
}   // namespace

IPC_EXPORT void EnableDzFlat(bool enabled)
{
    g_dzflat_enabled.store(enabled, std::memory_order_release);
}

IPC_EXPORT bool IsDzFlatEnabled()
{
    return g_dzflat_enabled.load(std::memory_order_acquire);
}

IPC_EXPORT std::uint64_t DzFlatPublishCount()
{
    return g_dzflat_published.load(std::memory_order_relaxed);
}

IPC_EXPORT std::uint64_t DzFlatFallbackCount()
{
    return g_dzflat_fallback.load(std::memory_order_relaxed);
}

IPC_EXPORT void ResetDzFlatCounters()
{
    g_dzflat_published.store(0, std::memory_order_relaxed);
    g_dzflat_fallback.store(0, std::memory_order_relaxed);
    /* W08: 追加口一并复位(否则按配置逐轮采数时上一轮会把这一轮算歪)。 */
    g_dzflat_prebuilt.store(0, std::memory_order_relaxed);
}

/* ---------------------------------------------------------------------------
 * W08 路径分流(实现见头文件同名段的说明)。⛔本函数**只做计数**, 不读消息内容、
 * 不改任何发布/接收行为; 热路径上就是一次 relaxed fetch_add(无锁、无分配、无日志)。
 * ------------------------------------------------------------------------- */
namespace detail {
IPC_EXPORT void NoteDzFlatPathDelivered(DzFlatPath path, std::uint64_t wire_bytes) noexcept
{
    using measure::CounterId;
    switch (path)
    {
    case DzFlatPath::Tlv:
        DZIPC_MEASURE_INC(CounterId::tlv_messages);
        DZIPC_MEASURE_ADD(CounterId::tlv_wire_bytes, wire_bytes);
        break;
    case DzFlatPath::DzFlatA:
        DZIPC_MEASURE_INC(CounterId::dzflat_a_messages);
        DZIPC_MEASURE_ADD(CounterId::dzflat_wire_bytes, wire_bytes);
        break;
    case DzFlatPath::DzFlatB:
        DZIPC_MEASURE_INC(CounterId::dzflat_b_messages);
        DZIPC_MEASURE_ADD(CounterId::dzflat_wire_bytes, wire_bytes);
        break;
    case DzFlatPath::PrebuiltSegment:
        /* W03 的三路径 ID 未定义这一入口 ⇒ 条数记在本模块; 传输字节仍归入
         * dzflat_wire_bytes(它确实是 DZFlat 段)。交付里已登记该 ID 缺口。 */
        g_dzflat_prebuilt.fetch_add(1, std::memory_order_relaxed);
        DZIPC_MEASURE_ADD(CounterId::dzflat_wire_bytes, wire_bytes);
        break;
    case DzFlatPath::kCount:
        break;
    }
}
}   // namespace detail

IPC_EXPORT std::uint64_t DzFlatPrebuiltSegmentCount() noexcept
{
    return g_dzflat_prebuilt.load(std::memory_order_relaxed);
}

IPC_EXPORT void EnableViewQueuePin(bool enabled)
{
    g_view_queue_pin.store(enabled, std::memory_order_release);
}

IPC_EXPORT bool IsViewQueuePinEnabled()
{
    return g_view_queue_pin.load(std::memory_order_acquire);
}

IPC_EXPORT std::size_t ViewQueueCap()
{
    // 默认用户队列与每话题每尺寸档容量均为 10；应用取出的持样另占池。
    return static_cast<std::size_t>(ipc::topic_msg_cache);
}

/* 传输层内部使用: 每条发布记一次。计数只用于观测, 用 relaxed 即可。 */
namespace detail {
IPC_EXPORT void NoteDzFlatPublish(bool used_dzflat)
{
    if (used_dzflat)
        g_dzflat_published.fetch_add(1, std::memory_order_relaxed);
    else
        g_dzflat_fallback.fetch_add(1, std::memory_order_relaxed);
}
}   // namespace detail

IPC_EXPORT DzFlatRxStats DzFlatRxCounters()
{
    using E = detail::DzFlatRxEvent;
    auto at = [](E e) {
        return g_dzflat_rx[static_cast<std::size_t>(e)].load(std::memory_order_relaxed);
    };
    DzFlatRxStats st;
    st.dzflat_accepted = at(E::kDzFlatAccepted);
    st.dzflat_id_skipped = at(E::kDzFlatIdSkipped);
    st.dzflat_header_bad = at(E::kDzFlatHeaderBad);
    st.dzflat_schema_drop = at(E::kDzFlatSchemaDrop);
    st.tlv_accepted = at(E::kTlvAccepted);
    st.tlv_id_skipped = at(E::kTlvIdSkipped);
    st.tlv_corrupt_drop = at(E::kTlvCorruptDrop);
    st.dzflat_adopt_spilled = at(E::kDzFlatAdoptSpilled);
    return st;
}

IPC_EXPORT void ResetDzFlatRxCounters()
{
    for (auto& c : g_dzflat_rx)
    {
        c.store(0, std::memory_order_relaxed);
    }
}

namespace detail {
IPC_EXPORT void NoteDzFlatRx(DzFlatRxEvent ev)
{
    const auto i = static_cast<std::size_t>(ev);
    if (i < kRxEventCount)
    {
        g_dzflat_rx[i].fetch_add(1, std::memory_order_relaxed);
    }
}
}   // namespace detail

IPC_EXPORT void EnableNodelet(bool enabled)
{
    g_nodelet_enabled.store(enabled, std::memory_order_release);
}

IPC_EXPORT bool IsNodeletEnabled()
{
    return g_nodelet_enabled.load(std::memory_order_acquire);
}

}   // namespace dzIPC
