#pragma once
/* dzipc_topic_cat 的传输选型: 从 IpcInfoPool 快照里挑出"当前应该嗅探的那一条"。
 *
 * 为什么单独抽出来: 旧的 main.cc 取"池里第一条 topic 匹配的条目"就收工
 * (且一旦 CONNECTED 就再也不看后面的条目), 在 Auto ser-cli 场景下是确定性选错 ——
 * 切到 SHM 之后池里 socket/shm 四条并存(socket 腿 stop_data_plane() 时**不注销**
 * 池登记, 且注册在先), 首条恒为 socket_server ⇒ 建 socket_sniffer 空转。
 *
 * 这里的规则:
 *   1. 扫描**全部**条目, 只看 topic 名匹配、alive、in_use、且 kind 属于本次模式
 *      (service / topic) 的条目;
 *   2. `--transport` 显式指定时按传输过滤(滤空了就是"没找到", 由调用方继续等);
 *      默认 auto: **SHM 优先**, 没有 SHM 条目才回退 socket —— 这样"抓当前真在跑的
 *      那条腿", 而不是"抓先注册的那条腿";
 *   3. 同一传输内取 `register_ts_ns` 最新的一条(与 dzIPC::autopath 的 find_peer
 *      同口径), 时间相同取 slot 小者, 保证选型确定。
 *
 * 纯函数: 只依赖 EntrySnapshot 列表, 不碰 SHM/UDP, 因此可以直接单测
 * (test/test_topic_cat_select.cpp)。
 */

#include <cstdint>
#include <string>
#include <vector>

#include "dzIPC/ipc_info_pool.h"

namespace dzipc_topic_cat {

/* CLI `--transport` 的取值。 */
enum class TransportPreference : int
{
    Auto = 0,   // 优先 SHM, 无 SHM 条目时用 socket (旧行为 + 缺陷修复)
    Shm,        // 只挂 SHM 条目; 没有就一直等
    Socket,     // 只挂 socket 条目; 没有就一直等
};

inline const char* to_string(TransportPreference p) noexcept
{
    switch (p)
    {
    case TransportPreference::Shm:
        return "shm";
    case TransportPreference::Socket:
        return "socket";
    case TransportPreference::Auto:
    default:
        return "auto";
    }
}

/* 解析 CLI 取值; 非法值返回 false(调用方报错退出, 不做静默回退)。 */
inline bool parse_transport_preference(const std::string& s, TransportPreference& out) noexcept
{
    if (s == "auto")
    {
        out = TransportPreference::Auto;
        return true;
    }
    if (s == "shm")
    {
        out = TransportPreference::Shm;
        return true;
    }
    if (s == "socket")
    {
        out = TransportPreference::Socket;
        return true;
    }
    return false;
}

inline bool is_shm_kind(dzIPC::info_pool::EntryKind k) noexcept
{
    using K = dzIPC::info_pool::EntryKind;
    return k == K::ShmServer || k == K::ShmClient || k == K::ShmPub || k == K::ShmSub;
}

/* 该条目是否属于本次嗅探模式(service / topic)能挂的 kind。 */
inline bool is_sniffable_kind(dzIPC::info_pool::EntryKind k, bool ser_or_topic) noexcept
{
    using K = dzIPC::info_pool::EntryKind;
    if (ser_or_topic)
        return k == K::SocketServer || k == K::SocketClient || k == K::ShmServer || k == K::ShmClient;
    return k == K::SocketPub || k == K::SocketSub || k == K::ShmPub || k == K::ShmSub;
}

inline bool transport_allowed(TransportPreference pref, bool shm) noexcept
{
    switch (pref)
    {
    case TransportPreference::Shm:
        return shm;
    case TransportPreference::Socket:
        return !shm;
    case TransportPreference::Auto:
    default:
        return true;
    }
}

struct Selection
{
    bool found{false};
    int32_t slot{-1};
    dzIPC::info_pool::EntryKind kind{dzIPC::info_pool::EntryKind::Unknown};
    int32_t pid{0};
    int32_t domain_id{0};
    int64_t register_ts_ns{0};
    bool shm{false};

    /* sniffer 的入参只有 (topic, domain, ser_or_topic, shm); 两条腿的
     * server/client 区分不影响挂哪条通道, 所以"要不要重建 sniffer"只看这两项 ——
     * 同一传输内换了个进程(slot 变)不需要重建, 免得白白清掉显示缓存。 */
    bool same_channel(const Selection& other) const noexcept
    {
        return found && other.found && shm == other.shm && domain_id == other.domain_id;
    }
};

inline Selection select_sniffer_entry(const std::vector<dzIPC::info_pool::EntrySnapshot>& entries,
                                      const std::string& topic_name, bool ser_or_topic, TransportPreference pref)
{
    Selection best;
    auto take = [&best](const dzIPC::info_pool::EntrySnapshot& e, bool shm)
    {
        best.found = true;
        best.slot = e.slot;
        best.kind = e.kind;
        best.pid = e.pid;
        best.domain_id = e.domain_id;
        best.register_ts_ns = e.register_ts_ns;
        best.shm = shm;
    };

    for (const auto& e : entries)
    {
        if (e.topic_name != topic_name)
            continue;
        if (!e.alive || !e.in_use)
            continue;
        if (!is_sniffable_kind(e.kind, ser_or_topic))
            continue;
        const bool shm = is_shm_kind(e.kind);
        if (!transport_allowed(pref, shm))
            continue;
        if (!best.found)
        {
            take(e, shm);
            continue;
        }
        if (shm != best.shm)
        {
            /* 只有 auto 会同时看到两种传输。SHM 胜出 —— 这正是本函数存在的理由:
             * 切到 SHM 后 socket 腿仍然登记在池里, 但它已经不发数据了。 */
            if (shm)
                take(e, shm);
            continue;
        }
        if (e.register_ts_ns > best.register_ts_ns
            || (e.register_ts_ns == best.register_ts_ns && e.slot < best.slot))
            take(e, shm);
    }
    return best;
}

}   // namespace dzipc_topic_cat
