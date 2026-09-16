#pragma once
/* ser-cli 握手通道(端口 = base + kUdpPortOffsetHandshake, 即 base+2)的**只读**观测探针。
 *
 * ---- 为什么需要它 ----
 * 路径裁定(path_state)的全部信号都只走这一条通道: 双方在握手帧的第 4 个载荷字节里
 * 交换 ProposeShm/ConfirmShm/ConfirmSocket/WithdrawToSocket。切到 SHM 之后 socket
 * 数据面被双侧停掉, 于是"看数据面"在这段时间里必然一片空白 —— **空白不是故障,
 * 而是预期**; 唯一还活着的证据就在这条常驻通道上。此前没有任何工具监听它
 * (dzipc_topic_cat 只建 +0/+1), 于是"切没切成功、为什么回退"在工具侧完全不可见。
 *
 * ---- 为什么这条探针是安全的(全部结论都有实测, 不是推断) ----
 * ① **不偷包**: dzIPC 的 UDP 全是 IP 组播, libipc 把 socket bind 到**组地址**并
 *    IP_ADD_MEMBERSHIP(见 src/libipc/platform/posix/udp.h)。组播的投递语义是
 *    "每个匹配的 socket 各得一份副本", 不是单播那种按 SO_REUSEPORT 散列到某一个
 *    socket。实测: A 发 1 帧, 业务对端 B 与观察者 W 各收 1 帧; A 连发 20 帧,
 *    B 收 20 帧 —— 与"没有观察者"的对照组逐数字相同。
 * ② **不注入**: 本探针只调用 create/connect/receive_nowait/close, **从不调用 send**。
 *    对照组实测(有/无观察者各 3 轮): A 的自回绕帧数、B 的收帧数逐数字一致。
 *    所有节点用 NodeRole::RecvOnly 建 —— 该角色在 libipc 里就是"只收不发"的语义
 *    (joins_group()==true 故照常 bind+入组), 把意图写进类型而不是注释里。
 * ③ **不阻塞**: receive_nowait() 不阻塞; 每轮最多排空 kMaxDrainPerPoll 帧, 防止
 *    对端刷帧把**调用方的循环**拖住。
 * ④ **不参与判定**: 探针只读快照, 不写回任何字段 —— 判错的代价只是屏幕上显示错,
 *    不会改变产品行为。
 *
 * ---- 边界(诚实标注) ----
 * ① 只对 ser-cli 存在这条通道; pub/sub 上没人建过 +2, 开在那里只会白占一个端口。
 * ② 探针看到的是**电平**不是事件流: 每轮轮询取"最近一帧"。握手帧约 10 帧/秒,
 *    轮询约 20 次/秒, 稳态下不会漏状态, 但极短的中转态(存在时间 < 一个轮询周期)
 *    可能被跳过。它是"当前对端状态"的观测器, 不是精确的事件序列记录器。
 * ③ 帧的合法性判据复用产品自己的字节布局(pure_payload_size/has_path_state/
 *    peek_path_state 就是 IpcPubSubIdInitMsg 的 static 公共函数), 不做第二套解读 ——
 *    判据单一来源。 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include "dzIPC/common/hash.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "libipc/udp.h"

namespace dzIPC {

/* 一帧握手报文的解码结果。valid==false 表示"这不像本通道的一帧"。 */
struct handshake_frame
{
    bool valid{false};
    bool host_flag{false};   // 服务端发的帧置 1
    bool cli_flag{false};    // 客户端发的帧置 1
    bool run_status{false};
    bool has_path_state{false};   // 旧帧为 false(纯载荷 3 字节)
    uint8_t path_state{0};        // IpcPubSubIdInitMsg::PathState 的数值
    uint32_t payload_size{0};
};

/* 解码一帧。**不拷贝、不拥有** data, 调用方保证 data 在本次调用期间有效。
 * 最短的合法帧 = 3 字节载荷(旧版本) + 12 字节页尾 = 15 字节。 */
inline handshake_frame decode_handshake_frame(const void* data, std::size_t n) noexcept
{
    handshake_frame f;
    if (data == nullptr || n < 15)
    {
        return f;
    }
    /* 非拥有视图: buffer(ptr, n) 走的是 destructor==nullptr 的那条构造,
     * 析构时直接返回、绝不释放(见 src/libipc/buffer.cpp:45-51)。这里只是为了让
     * 产品侧的判据函数能拿到 ipc::buffer 形参, 不是把数据接管过来。 */
    ipc::buffer view(const_cast<void*>(data), n);
    const uint32_t payload = IpcPubSubIdInitMsg::pure_payload_size(view);
    /* 只认握手帧这一种形状: 载荷 3(旧)或 4(新)字节。别的一律拒绝 —— 本通道上
     * 不该出现别的东西, 认错了不如不认。 */
    if (payload != 3 && payload != 4)
    {
        return f;
    }
    const auto* p = static_cast<const uint8_t*>(data);
    f.host_flag = (p[0] == 1);
    f.cli_flag = (p[1] == 1);
    f.run_status = (p[2] == 1);
    f.payload_size = payload;
    f.has_path_state = IpcPubSubIdInitMsg::has_path_state(view);
    f.path_state = f.has_path_state ? static_cast<uint8_t>(IpcPubSubIdInitMsg::peek_path_state(view)) : 0;
    /* 身份判据 = 产品自己的对端判据: 服务端认 check_cli, 客户端认 check_host。
     * 因此"host/cli 恰好一个为真"才是本通道的一帧。 */
    f.valid = (f.host_flag != f.cli_flag);
    return f;
}

/* 一侧(服务端或客户端)的观测快照。 */
struct handshake_observation
{
    bool seen{false};
    uint8_t path_state{0};
    bool has_path_state{false};
    bool run_status{false};
    uint64_t frames{0};
    int64_t last_ts_ns{0};
};

/* 探针的完整快照(一次加锁取全, 供 UI 线程读)。 */
struct handshake_snapshot
{
    bool opened{false};   // false = 没开观测 / 开失败 ⇒ 调用方什么都不显示(默认兼容)
    uint16_t port{0};
    uint64_t frames{0};        // 收到的帧总数(含解不出来的)
    uint64_t undecodable{0};
    handshake_observation server{};   // 服务端(host_flag)那一侧
    handshake_observation client{};   // 客户端(cli_flag)那一侧
};

/* path_state 的显示名。⛔ 这里只做**显示**, 判定用的数值永远来自
 * IpcPubSubIdInitMsg::peek_path_state。
 *
 * ⚠️ default 分支返回的是 "path_state(?)" —— **不含数值**。上面那句"会显示
 * path_state(<数值>)"是**错的**(与实现不符), 已就地更正为事实。
 * 这个差距今天有实际后果: F2 把 5..8 变成线上可见量之后, "将来再加一个值"或"监控读到
 * 比自己新的值"都会显示成**无法区分**的 "?" —— 分不清"未知的新原因"与"垃圾字节",
 * 归因链在那里断掉。**未实施**的最小修法: default 用 thread_local 缓冲返回
 * "path_state(<数值>)"(返回类型与 ABI 不变)。该项属 F2 复核清单, 待 leader 指派;
 * 本文件本轮只更正注释, **不改行为**。
 *
 * F2 新增 5..8(撤销原因细化)。显示成 "Withdraw(原因)" 而不是照抄枚举名, 是因为
 * 运维看的是**现象**: 这四行都是"对端撤销了", 区别只在原因; 名称前缀相同才一眼看得出
 * 它们同族。老端只看得到无原因的 WithdrawToSocket, 所以那一行的显示名不带后缀。 */
inline const char* path_state_name(uint8_t v) noexcept
{
    using PS = IpcPubSubIdInitMsg::PathState;
    switch (static_cast<PS>(v))
    {
    case PS::Unknown: return "Unknown";
    case PS::ProposeShm: return "ProposeShm";
    case PS::ConfirmShm: return "ConfirmShm";
    case PS::ConfirmSocket: return "ConfirmSocket";
    case PS::WithdrawToSocket: return "WithdrawToSocket";
    case PS::WithdrawChannelOccupied: return "Withdraw(ChannelOccupied)";
    case PS::WithdrawEstablishFailed: return "Withdraw(EstablishFailed)";
    case PS::WithdrawRendezvousTimeout: return "Withdraw(RendezvousTimeout)";
    case PS::WithdrawRuntimeDisconnect: return "Withdraw(RuntimeDisconnect)";
    default: return "path_state(?)";
    }
}

class handshake_probe
{
public:
    static constexpr int kMaxDrainPerPoll = 64;

    handshake_probe() = default;
    ~handshake_probe() { close(); }

    handshake_probe(handshake_probe const&) = delete;
    handshake_probe& operator=(handshake_probe const&) = delete;

    /* 只读打开 base+2。失败(端口冲突/组地址非法/无网络)返回 false 并由调用方
     * 降级为"无观测" —— 观测不可用绝不能影响嗅探器本身能不能工作。 */
    bool open(const std::string& topic_name, int domain_id) noexcept
    {
        if (opened_)
        {
            return true;
        }
        try
        {
            const std::string group = common::udp_discovery_addr_calculate(topic_name);
            port_ = static_cast<uint16_t>(common::udp_discovery_port_calculate(topic_name, domain_id)
                                          + common::kUdpPortOffsetHandshake);
            node_ = std::make_unique<ipc::socket::UDPNode>(topic_name.c_str(), group.c_str(), port_,
                                                           ipc::socket::NodeRole::RecvOnly);
            /* connect() 在 libipc 里是"建 socket + bind 组地址 + 入组", **不发任何报文**
             * (全文件没有 ::connect 系统调用, send 才走 sendto)。 */
            if (!node_->connect())
            {
                node_.reset();
                return false;
            }
        }
        catch (...)
        {
            node_.reset();
            return false;
        }
        opened_ = true;
        return true;
    }

    void close() noexcept
    {
        if (!node_)
        {
            opened_ = false;
            return;
        }
        try
        {
            node_->close();   // 退出组播组; 同样不发报文
        }
        catch (...)
        {
        }
        node_.reset();
        opened_ = false;
    }

    bool opened() const noexcept { return opened_; }

    /* 非阻塞排空。可在任意线程调用(嗅探器的接收线程 / 测试线程)。 */
    void poll() noexcept
    {
        if (!opened_ || !node_)
        {
            return;
        }
        for (int i = 0; i < kMaxDrainPerPoll; ++i)
        {
            ipc::buffer raw;
            try
            {
                raw = node_->receive_nowait();
            }
            catch (...)
            {
                return;
            }
            if (raw.size() == 0)
            {
                return;
            }
            /* ⛔ receive_nowait 返回的是 node 内部 temp_buffer 的**非拥有视图**
             * (buffer(ptr, n, nullptr)), 下一次 receive 就会覆盖它 —— 必须在本轮
             * 解完, 不能把它留到下一轮。 */
            const handshake_frame f = decode_handshake_frame(raw.data(), raw.size());
            std::lock_guard<std::mutex> lock(m_);
            ++frames_;
            if (!f.valid)
            {
                ++undecodable_;
                continue;
            }
            const int64_t now = now_ns();
            handshake_observation& side = f.host_flag ? server_ : client_;
            side.seen = true;
            side.path_state = f.path_state;
            side.has_path_state = f.has_path_state;
            side.run_status = f.run_status;
            ++side.frames;
            side.last_ts_ns = now;
        }
    }

    handshake_snapshot snapshot() const noexcept
    {
        handshake_snapshot s{};
        try
        {
            std::lock_guard<std::mutex> lock(m_);
            s.opened = opened_;
            s.port = port_;
            s.frames = frames_;
            s.undecodable = undecodable_;
            s.server = server_;
            s.client = client_;
        }
        catch (...)
        {
        }
        return s;
    }

private:
    static int64_t now_ns() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::unique_ptr<ipc::socket::UDPNode> node_;
    bool opened_{false};
    uint16_t port_{0};
    mutable std::mutex m_;
    uint64_t frames_{0};
    uint64_t undecodable_{0};
    handshake_observation server_{};
    handshake_observation client_{};
};

}   // namespace dzIPC
