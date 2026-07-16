#pragma once

#include <cstddef>
#include <cstdint>
#include "libipc/export.h"

namespace dzIPC {
namespace common {

IPC_EXPORT uint32_t crc32c(const void* data, std::size_t size);

}   // namespace common
}   // namespace dzIPC
