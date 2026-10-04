#pragma once

namespace dzIPC {

/* Parse the opt-in SHM MPMC switch without consulting process state.
 * Only the explicit value "1" enables the new layout; all other values
 * keep the legacy single-publisher route. */
bool shm_mpmc_enabled_value(const char* value) noexcept;

/* Process-wide mode selection. The environment is sampled once, matching the
 * existing SHM control-scheduler switch semantics. */
bool shm_mpmc_enabled() noexcept;

} // namespace dzIPC
