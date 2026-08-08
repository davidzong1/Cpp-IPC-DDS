#pragma once

#include "libipc/export.h"

namespace dzIPC {

// ---------------------------------------------------------------------------
// Unified nodelet fast-path enable switch (process-wide, default OFF).
//
// When enabled, each transport (SHM, UDP socket) MAY attempt an intra-process
// fast path.  The switch is a necessary but not sufficient condition —
// transports still verify local-only topology, K=3 stability, etc.
//
// Default is false (standard path).  Call EnableNodelet(true) once during
// initialization after confirming same-process deployment topology.
// ---------------------------------------------------------------------------
IPC_EXPORT void EnableNodelet(bool enabled);
IPC_EXPORT bool IsNodeletEnabled();

}   // namespace dzIPC
