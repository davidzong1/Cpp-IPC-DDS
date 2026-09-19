#include "dzIPC/common/nodelet_config.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "libipc/def.h"

namespace dzIPC {

namespace {
std::atomic<bool> g_nodelet_enabled{false};
std::atomic<bool> g_dzflat_enabled{false};
std::atomic<std::uint64_t> g_dzflat_published{0};
std::atomic<std::uint64_t> g_dzflat_fallback{0};

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
    /* 池容量 / 4。池容量取 ipc::large_msg_cache —— 它同时是 ipc::id_pool<>::max_count
     * 的取值来源(id_pool.h 取二者较小), 也就是池空报告里打印的那个 "pool capacity";
     * 这里不直接引 id_pool.h 是因为那是 libipc 的内部头, 而 dzIPC 侧只依赖 def.h。
     * max(1, ...) 是防御: 若将来把 large_msg_cache 调到 <4, 整数除法会给出容量 0 的
     * 队列 —— 那会让 view 路径彻底失效, 远比"钉得不够紧"更糟。 */
    constexpr std::size_t kDivisor = 4;
    return (std::max)(std::size_t{1},
                      static_cast<std::size_t>(ipc::large_msg_cache) / kDivisor);
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
