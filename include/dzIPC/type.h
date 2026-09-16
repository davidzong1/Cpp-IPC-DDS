#pragma once

namespace dzIPC
{
    enum IPCType
    {
        Shm,
        Socket,
        /* 自动选路(T3): 握手先走 UDP 引导通道; 双方通过握手帧两阶段确认后,
         * 同主机切 SHM、跨主机保持 socket。切换对调用方不可见(一个连接对象),
         * 当下实际走哪条路查 transport_current()。
         *
         * 加到末尾而不是插在中间: 前两个值已在 ABI 上被既有调用方/日志使用,
         * 插值会静默改变它们对 Shm/Socket 的解读。 */
        Auto
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