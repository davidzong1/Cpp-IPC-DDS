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
