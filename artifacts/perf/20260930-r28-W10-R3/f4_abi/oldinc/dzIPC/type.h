#pragma once

namespace dzIPC
{
    enum IPCType
    {
        /* ---- 三个值, 三个互不重叠的含义 ---- */
        Shm = 0,        // 强制共享内存。同机最省, 跨不了主机。
        Socket = 1,     // ser-cli: 自动选路; pub/sub: 纯 UDP socket。
        /* ---- 值 2 是空洞, 故意**不留枚举名** ----
         *
         * 它曾是 Auto(自动选路的显式枚举值)。Auto 已删除, 理由: 它唯一的作用是当
         * "ser-cli 把 Socket 映射成什么"的中间值, 而那个映射经变异测试证明**没有任何
         * 可观测行为**(把它改成恒等, 49/49 仍全绿)。先测出"删了没影响"再删, 不是凭推理。
         *
         * 空洞必须留着: 值 2 可能已被旧二进制的调用方使用, 让 SocketOnly 从 3 挪到 2
         * 会**静默**改变它们对值的解读 —— 这与当初"Auto 追加到末尾而不插在中间"是同
         * 一条理由。
         *
         * 不留枚举名而不是写个 _Reserved2 = 2: 下划线后跟大写字母的标识符是保留给
         * 实现的, 用它本身就不合规; 而"没有名字"恰好就是这里要表达的意思。
         * 值 2 仍落在枚举的取值范围内(0..3), 所以 static_cast<IPCType>(2) 是良定义的
         * —— 见 topic_ipc.cc 的 kReservedAutoSlot。 */
        /* 强制**纯** UDP socket —— 不做任何自动选路(即使同主机也不切 SHM)。
         *
         * 存在的理由: ser-cli 的 Socket 就是自动选路, 于是"我要纯 socket"这个意图
         * **没有别的表达方式**。需要它的场合: 对照实验/基线测量、复现
         * "跨主机"行为、排查"到底是 SHM 腿还是 socket 腿出问题"。生产路径不该用它。
         *
         * pub/sub 上它与 Socket 等价 —— pub/sub 本来就没有自动选路。 */
        SocketOnly = 3
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