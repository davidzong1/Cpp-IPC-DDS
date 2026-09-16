#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dzIPC {
namespace control_plane_shm {

enum class TopicState : uint32_t
{
    Empty = 0,
    Clearing = 1,
    Ready = 2,
    Stopping = 3
};

/* 单个订阅者的存活登记槽。
 *
 * 引入原因: libipc 的写路径原本在 force_push 里用"这一格还没读完"判定
 * invalid reader 并 disconnect_receiver(), 把只是慢了一格的活订阅者和真正
 * 死掉的进程一起踢下线(详见 src/libipc/prod_cons.h)。现在写路径改成只覆写
 * 不踢人, 死连接的回收就必须换一个真正反映存活性的信号 —— 即这里的心跳。
 *
 * 订阅端在 sub_handshake() 线程里周期刷新 heartbeat_ns; 发布端在
 * pub_handshake() 线程里扫描, 超时未刷新的才判死并回收其 cc_id。 */
struct PeerSlot
{
    std::atomic<uint32_t> in_use;        // 0=空闲 1=占用
    std::atomic<uint32_t> cc_id;         // 该订阅者在 libipc 接收方连接位图中的 bit
    std::atomic<int32_t> pid;            // 拥有者进程, 仅用于诊断
    std::atomic<uint32_t> generation;    // 登记时的 topic generation
    std::atomic<int64_t> heartbeat_ns;   // 最近一次心跳 (steady_clock)
};

/* 段头魔数(v2 布局: TopicControl 含 PeerSlot 表)。
 *
 * 只读探测者必须校验它: 段名规则是"topic+domain"派生的, 同名段可能是别人(别的
 * 版本/别的用途)建出来的 —— 魔数不符就读不懂里面的 state/owner_pid, 这时必须
 * 走保守方向, 不能把自己的语义套上去。 */
inline constexpr uint32_t kTopicControlMagic = 0x445A4351U;

/* 只读探测: 该控制面段是否被**别的活进程**占着(未建好/不确定时也按被占用算)。
 *
 * 引入原因: IpcInfoPool 是**登记**式的证据(T2 §3 D1), 只有主动 rebind 过的进程
 * 才看得见; 而控制面段是**建出来**的 —— 谁真把这条 SHM 服务通道建起来, 段就在。
 * 因此"段在 + 状态已建/正在重建 + owner 是活着的别的进程"是一条不依赖登记的
 * 占用证据, 用来兜住池启发式的漏判。
 *
 * 判错方向的代价不对称, 所以本函数一律往"被占用"偏:
 *   漏判占用 ⇒ 随后的 clear_storage 摧毁别人的活动连接(不可逆);
 *   误判占用 ⇒ 少切一次 SHM, 退化为 socket(功能完好)。
 *
 * ⛔ 绝不创建段、绝不 unlink 段: open-only 打开, 释放走 release_no_unlink()。
 *    (release() 在引用计数降到 0 时会 shm_unlink —— 那正是本函数要防的事。)
 *
 * 返回 false 的**唯一**含义是"段不存在": 这条通道从没被 SHM 建过, 不可能摧毁
 * 任何既有连接。其余一切(读不出、魔数不符、owner 存活状态不明)都返回 true。 */
bool occupied_by_other(const std::string& name, int32_t self_pid);

/* 单 topic 支持的最大并发订阅者数。libipc 的接收方连接位图是 32 位
 * (circ::cc_t = uint32_t), 因此实际可用连接数上限本来就是 32; 这里取 64
 * 留出余量, 避免订阅者频繁重连时槽位回收不及时导致占满。 */
constexpr uint32_t kMaxPeerSlots = 64;

struct TopicControl
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> generation;
    std::atomic<uint32_t> state;
    std::atomic<int32_t> owner_pid;
    std::atomic<int64_t> heartbeat_ns;
    std::atomic<uint32_t> peer_count;
    PeerSlot peers[kMaxPeerSlots];
};

class TopicControlPlane
{
public:
    TopicControlPlane();
    ~TopicControlPlane();

    TopicControlPlane(const TopicControlPlane&) = delete;
    TopicControlPlane& operator=(const TopicControlPlane&) = delete;

    bool open(const std::string& name);
    bool valid() const noexcept { return control_ != nullptr; }

    uint32_t begin_rebuild();
    void set_ready();
    void set_stopping();
    void heartbeat();

    uint32_t generation() const;
    TopicState state() const;
    uint32_t peer_count() const;

    bool add_peer(uint32_t generation);
    void remove_peer(uint32_t generation);

    /* ---- 订阅端: 存活登记 ---- */

    /* 占用一个槽位并登记自己的 libipc 连接 bit。
     * 返回槽位下标; cc_id 为 0 或槽位耗尽时返回 -1。 */
    int acquire_peer_slot(uint32_t generation, uint32_t cc_id);

    /* 刷新槽位心跳。slot < 0 时为空操作。 */
    void peer_heartbeat(int slot);

    /* 释放槽位。slot < 0 时为空操作。 */
    void release_peer_slot(int slot);

    /* ---- 发布端: 死连接回收 ---- */

    /* 扫描全部槽位, 把心跳距今超过 timeout_ns 的判定为死连接: 清空其槽位,
     * 并将这些订阅者的 cc_id 按位或后返回, 供调用方交给
     * ipc::route::disconnect_receivers() 从连接位图中摘除。
     * 返回 0 表示本轮没有需要回收的连接。 */
    uint32_t collect_stale_peers(int64_t timeout_ns);

private:
    void initialize_if_needed();
    void clear_all_peer_slots();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TopicControl* control_{nullptr};
};

}   // namespace control_plane_shm
}   // namespace dzIPC
