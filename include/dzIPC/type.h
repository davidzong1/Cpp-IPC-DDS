#pragma once

namespace dzIPC
{
    enum IPCType
    {
        Shm,
        Socket
    };
    enum CPU_CORE:int
    {
        None = -1,
        CORE0,
        CORE1,
        CORE2,
        CORE3,
        CORE4,
        CORE5,
        CORE6,
        CORE7
    };
    enum Qos:bool
    {
        UseQos = true,
        NotUseQos = false
    };
    enum DispatchPriority:int
    {
        LowPriority = 0,
        NormalPriority = 20,
        HighPriority = 40,
        ultraHighPriority = 60,
    };
}