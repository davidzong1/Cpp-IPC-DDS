#pragma once
#include "libipc/export.h"

namespace ipc::detail {
// 内部诊断，默认关闭；编号稳定。钩子不得阻塞、分配或抛出异常。
// 时间由采集器读取，未安装钩子时不读时钟，不改变消息布局。
enum class PublishPoint : unsigned {
    BeforeLoan, AfterLoan, BeforeCopy, AfterCopy,
    BeforeCommit, BeforeNotify, AfterNotify, AfterCommit, Count
};
using PublishHook = void (*)(PublishPoint) noexcept;
IPC_EXPORT void set_publish_hook(PublishHook hook) noexcept;
IPC_EXPORT PublishHook get_publish_hook() noexcept;
inline void trace_publish(PublishPoint point) noexcept {
    if (const auto hook = get_publish_hook()) hook(point);
}
}
