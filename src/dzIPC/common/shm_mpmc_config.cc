#include "dzIPC/common/shm_mpmc_config.h"

#include <cstdlib>

namespace dzIPC {

bool shm_mpmc_enabled_value(const char* value) noexcept
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool shm_mpmc_enabled() noexcept
{
    static const bool enabled = shm_mpmc_enabled_value(std::getenv("DZIPC_SHM_MPMC"));
    return enabled;
}

} // namespace dzIPC
