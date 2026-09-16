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
        Auto,
        /* 强制**纯** UDP socket —— 不做任何自动选路(即使同主机也不切 SHM)。
         *
         * 存在的理由: ser-cli 的 Socket 已被归一化为 Auto(见 server_ipc.cc 的
         * normalize_sercli_type), 于是"我要纯 socket"这个意图**没有别的表达方式**。
         * 需要它的场合: 对照实验/基线测量、复现"跨主机"行为、排查"到底是 SHM 腿
         * 还是 socket 腿出问题"。生产路径不该用它。
         *
         * 同样加到末尾(SocketOnly = 3): 值 2 已被 Auto 占用, 插值会让既有的
         * IPC_AUTO 调用方静默拿到另一种语义。
         *
         * pub/sub 上它与 Socket 等价 —— pub/sub 本来就没有自动选路。 */
        SocketOnly
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