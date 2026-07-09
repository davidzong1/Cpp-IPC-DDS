#pragma once

namespace ipc {
namespace socket {

#define Debug true
#if Debug
#    define IPC_EXCEPTION_
#else
#    define IPC_EXCEPTION_ noexcept
#endif

#define TCP true
#define UDP false
}   // namespace socket
}   // namespace ipc