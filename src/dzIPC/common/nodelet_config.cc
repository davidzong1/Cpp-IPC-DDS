#include "dzIPC/common/nodelet_config.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dzIPC {

namespace {
std::atomic<bool> g_nodelet_enabled{false};
std::atomic<bool> g_dzflat_enabled{false};
std::atomic<std::uint64_t> g_dzflat_published{0};
std::atomic<std::uint64_t> g_dzflat_fallback{0};

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
