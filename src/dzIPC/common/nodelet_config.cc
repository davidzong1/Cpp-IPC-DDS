#include "dzIPC/common/nodelet_config.h"
#include <atomic>

namespace dzIPC {

namespace {
std::atomic<bool> g_nodelet_enabled{false};
}   // namespace

IPC_EXPORT void EnableNodelet(bool enabled)
{
    g_nodelet_enabled.store(enabled, std::memory_order_release);
}

IPC_EXPORT bool IsNodeletEnabled()
{
    return g_nodelet_enabled.load(std::memory_order_acquire);
}

}   // namespace dzIPC
