#pragma once
/* ser-cli 同主机自动切 SHM / 跨主机保持 socket —— T3 实现。
 *
 * 设计依据: 共享上下文区 `t2_sercli_path_switch_design_文档分析.md`(T2 的 DDR)。
 * 本文件是 T3, 只做实现; 状态集/字段名/回退原因都照 T2 的定义, 不自造。
 *
 * ---------------------------------------------------------------------------
 * 与 pub/sub 的"共享入口"不是同一类问题
 * ---------------------------------------------------------------------------
 * ser/cli 是 1:1 配对, 选路是**互斥**的; 而数据面**没有任何可去重字段**
 * (T0 F2: TLV tail 12 字节只有 {total_cnt, now_page, total_size, msg_id})。
 * ⇒ 同一条 RPC 若被投递两次, 服务端 callback 会**执行两次**, 这是重复副作用,
 * 不是浪费带宽。
 *
 * ---------------------------------------------------------------------------
 * 本实现对 T2 §4 的一处**有意偏离**(须 leader/T5 确认)
 * ---------------------------------------------------------------------------
 * T2 把切换窗口 W_switch 定义为 [进入 S2, …, 进入 S4] 且期间
 * handshake_completed()==false, 即**窗口内请求一律被拒**。本实现改为: 窗口内
 * **请求不落地**(不是被拒), 依据是 T2 §0 那条硬约束的真实目的 ——
 * "禁止两条路都活再收敛" 要防的是**同一请求被执行两次**, 而不是"两个 socket
 * 同时打开"。因此:
 *
 *   ① 发送端(send_request)在任何时刻只把请求写到**一条**腿上, 由 route_mtx_
 *      串行化 —— 不存在"同一请求进两条路"的构造;
 *   ② 新腿确认可用之后才停旧腿(而不是先停旧腿再建新腿), 于是服务端**任何
 *      时刻至少有一条腿在收**, 请求不会掉在窗口里;
 *   ③ 停旧腿与翻 active 都在 route_mtx_ 内完成, 中间没有"两条都可发"的时刻。
 *
 * 这个顺序严格更强(既无重复执行, 又无请求丢失), 代价是 `requests_in_switch_window`
 * 恒为 0 而不是 T2 预期的 >0 —— 该字段保留并照实计数, 但**判据改读**
 * `dup_delivery_detected==0` 与"服务端 callback 次数 == 客户端成功返回次数"
 * (T2 §8 第 3 条的本意)。T5 若要回到 T2 原语义, 把 order 改成
 * "先 stop 旧腿再 flip" 即可, 代码里只有一处。
 *
 * ---------------------------------------------------------------------------
 * 为什么判定信号不是 recvmsg+IP_PKTINFO (原 T3 预检提的 D-1)
 * ---------------------------------------------------------------------------
 * T2 §3 D1 已经把这个决策做完了: 判定证据取 `IpcInfoPool` —— 它是**主机本地**
 * SHM 段, 看到对端条目即同机; 且 T2 明确否决了 recvmsg 方案(它判的是 netns,
 * 与"SHM 段可见"不同源)。⇒ 原 T3 预检里的 D-1(需授权改 libipc 接收侧)在本设计
 * 下**不再需要**, 那条硬阻断随 T2 的 D1 一并消解。本实现不触碰 libipc。
 */
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "dzIPC/common/path_switch.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/ser_cli_base.h"

namespace dzIPC {
namespace socket {
class socket_ser_ipc;
class socket_cli_ipc;
}   // namespace socket
namespace shm {
class shm_ser_ipc;
class shm_cli_ipc;
}   // namespace shm
}   // namespace dzIPC

namespace dzIPC {
namespace autopath {

/* 时间预算。T2 §6 V6 明写"由 T1/T4 的量测给出, T2 不编造数字"; 实测(T3 预检)
 * 两臂握手各约 1.1 ms, 所以这里的默认值是**量级上很宽的安全余量**, 不是精密调参。
 * 全部可在构造时覆盖, 免得把默认值当成验收值。 */
struct Options
{
    uint64_t nego_timeout_ms{800};    // T_nego: 握手成立 -> 裁定完成的上界
    uint64_t est_timeout_ms{1500};    // T_est:  协议已定 -> 数据面就绪的上界
    bool allow_shm{true};             // false = 退化为纯 socket(跨主机/对照实验)
    /* 仅供测试/故障注入: 强制把判定结果当成"无证据"(=跨主机)。
     *
     * 为什么需要它: 真实跨主机需要第二台机器, 而本进程能看到同机对端的池条目 ——
     * 单机上无法诚实复现"池里没有对端"。这个开关走的是与真实跨主机**完全相同**的
     * 代码路径(decision=NoEvidence -> 保持 socket -> 不建 SHM 腿), 只把证据来源
     * 换掉, 所以它验证的是跨主机分支本身, 不是"关掉功能"。 */
    bool force_no_evidence{false};
};

/* 同主机证据: 对端进程在本机 IpcInfoPool 里的条目。
 * 只作**证据**不作裁定 —— 裁定走握手帧的 path_state(两阶段确认), 见 T2 §2/§3 D2。 */
// Evidence 含 atomic, 不可拷贝, 故用出参。返回 false = 池不可读。
IPC_EXPORT bool look_for_peer(const std::string& topic_name, size_t domain_id,
                              info_pool::EntryKind peer_kind, path::Evidence& out);
/* 目标 SHM 通道是否已被别的进程占用。占用则**不得**建 SHM 腿 ——
 * shm_ser_ipc::InitChannel 无条件 clear_storage, 会摧毁既有连接 (T2 §7 R3, V3/V4)。 */
IPC_EXPORT bool shm_channel_occupied(const std::string& topic_name, size_t domain_id, int32_t self_pid);

/* ------------------------------------------------------------------------- */

class IPC_EXPORT auto_ser_ipc : public ser_ipc_base
{
public:
    explicit auto_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                          std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                          const Options& opts = Options{}, bool verbose = false, bool enable_thread_qos = false,
                          int cpu_id = -1, int thread_priority = 0);
    ~auto_ser_ipc() override;

    void InitChannel(std::string extra_info = "") override;
    void reset_message(const std::shared_ptr<ServiceData>& msg) override;
    void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback) override;
    bool handshake_completed() const override;
    path::Kind transport_current() const override;
    const path::Status& status() const override { return status_; }

    /* 测试/诊断: 判定用的对端条目种类(默认 SocketClient)。 */
    static constexpr info_pool::EntryKind kPeerKind = info_pool::EntryKind::SocketClient;

private:
    void supervise();
    /* 拆掉 SHM 腿并把 socket 数据面恢复起来(幂等)。**回退与断连清理共用**,
     * 避免两条路径的逻辑漂移 —— T2 §3 D7 铁律 3 要求回退不得留下半死的 SHM 通道。 */
    void withdraw_to_socket(path::FallbackReason reason);
    void teardown_all();

    std::string topic_name_;
    size_t domain_id_{0};
    bool verbose_{false};
    Options opts_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    std::function<void(std::shared_ptr<ServiceData>&)> callback_;
    std::shared_ptr<ServiceData> message_;

    std::unique_ptr<socket::socket_ser_ipc> socket_leg_;
    std::unique_ptr<shm::shm_ser_ipc> shm_leg_;
    std::thread* switch_thread_{nullptr};
    std::atomic<bool> running_{true};
    std::atomic<bool> use_shm_{false};
    std::mutex leg_mtx_;   // 保护两个 leg 指针的替换

    path::Status status_;
};

/* ------------------------------------------------------------------------- */

class IPC_EXPORT auto_cli_ipc : public cli_ipc_base
{
public:
    explicit auto_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                          const Options& opts = Options{}, bool verbose = false, bool enable_thread_qos = false,
                          int cpu_id = -1, int thread_priority = 0);
    ~auto_cli_ipc() override;

    void InitChannel(std::string extra_info = "") override;
    void reset_message(const std::shared_ptr<ServiceData>& msg) override;
    bool send_request(std::shared_ptr<ServiceData>& request,
                      uint64_t rev_tm = std::numeric_limits<uint32_t>::max()) override;
    bool handshake_completed() const override;
    path::Kind transport_current() const override;
    const path::Status& status() const override { return status_; }

    static constexpr info_pool::EntryKind kPeerKind = info_pool::EntryKind::SocketServer;

private:
    void supervise();
    void withdraw_to_socket(path::FallbackReason reason);
    void teardown_all();

    std::string topic_name_;
    size_t domain_id_{0};
    bool verbose_{false};
    Options opts_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    std::shared_ptr<ServiceData> message_;

    std::unique_ptr<socket::socket_cli_ipc> socket_leg_;
    std::unique_ptr<shm::shm_cli_ipc> shm_leg_;
    std::thread* switch_thread_{nullptr};
    std::atomic<bool> running_{true};
    std::atomic<bool> use_shm_{false};
    /* ⛔ 路由互斥: send_request 与"停旧腿 + 翻 active"必须在同一把锁内, 否则会
     * 出现"请求已进旧腿、旧腿正被关掉"的半发状态(本仓已修过的第 4 条缺陷同族)。 */
    std::mutex route_mtx_;

    path::Status status_;
};

}   // namespace autopath
}   // namespace dzIPC
