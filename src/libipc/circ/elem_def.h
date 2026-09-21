#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "libipc/def.h"
#include "libipc/rw_lock.h"

#include "libipc/platform/detail.h"

namespace ipc {
namespace circ {

using u1_t = ipc::uint_t<8>;
using u2_t = ipc::uint_t<32>;

/** only supports max 32 connections in broadcast mode */
using cc_t = u2_t;

constexpr u1_t index_of(u2_t c) noexcept {
    return static_cast<u1_t>(c);
}

/* ── UF-003: owner 表(位→持有者身份侧车)─────────────────────────────────
 * 背景: 收方崩溃(kill -9)时, 它名下的 chunk 位图位与 elems 位图位都悬挂,
 * 池里的 chunk 永远等不到归还, 同档位发布方全部退化为 TLV 拷贝。
 * 方案(拍板见 docs/uf003_crash_reclaim_decision.md §2/§3):
 *   - owner 身份 = (pid, starttime), 8B 原子 u64: pid24 | start32 | gen8;
 *   - 表内嵌在 elems 尾部(槽数组之后), 位编号即槽编号;
 *   - 位分配器(conn_head::connect 的 CAS)是唯一权威, 本表只做"验尸";
 *   - 崩溃者不清槽(清不了) —— 清扫方据此回收它名下的悬挂 chunk。
 * 线序保证: connect 先发位、后写槽; disconnect 先清位、后清槽 —— 槽记录
 * 滞后于位状态, 于是"位悬挂而槽记录仍是死进程"是崩溃形态的稳定指纹;
 * 反向(槽先清、位还在)不会出现在本实现中。 */

/** owner 记录: [0:24) pid | [24:56) starttime | [56:64) gen; 0 = 槽空。 */
inline std::uint32_t current_pid() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

/** /proc/<pid>/stat 探测结果: parsed 才携带有意义 state/start; missing = 进程
 *  确定不存在(ENOENT); uncertain = 读了但拿不到(权限/隔离/格式异常)。 */
enum class proc_state : std::uint8_t { missing, uncertain, parsed };

/** 读 /proc/<pid>/stat 的 state(第 3 字段)与 starttime(第 22 字段, 节拍)。 */
inline proc_state proc_stat(std::uint32_t pid, char *state,
                            std::uint32_t *start) noexcept {
    *state = '?';
    *start = 0;
#if defined(_WIN32)
    return proc_state::uncertain;   // 无 /proc: 一律不确定(⇒ 永不回收)
#else
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    std::FILE *f = std::fopen(path, "r");
    if (f == nullptr)
        return (errno == ENOENT) ? proc_state::missing : proc_state::uncertain;
    char buf[512];
    char const *line = std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    if (line == nullptr) return proc_state::uncertain;
    /* comm 字段可含空格与括号(如 "(foo bar)"), 必须从**最后一个 ')' 之后**
     * 开始数字段: 其后第 1 个是 state(字段 3), 第 20 个是 starttime(字段 22)。 */
    char const *p = std::strrchr(buf, ')');
    if (p == nullptr) return proc_state::uncertain;
    ++p;
    char tmp[64];
    for (unsigned field = 3; field <= 22; ++field) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '\n') return proc_state::uncertain;
        char const *e = p;
        while (*e != '\0' && *e != ' ' && *e != '\t' && *e != '\n') ++e;
        std::size_t const n = static_cast<std::size_t>(e - p);
        if (field == 3) {
            *state = *p;
        } else if (field == 22) {
            if (n >= sizeof(tmp)) return proc_state::uncertain;
            std::memcpy(tmp, p, n);
            tmp[n] = '\0';
            *start = static_cast<std::uint32_t>(std::strtoull(tmp, nullptr, 10));
            return proc_state::parsed;
        }
        p = e;
    }
    return proc_state::uncertain;
#endif
}

/** 当前进程的 starttime(节拍)。带 pid 守卫的缓存: fork 后的子进程 pid 变化
 *  必须重读, 否则会继承父进程的 starttime。 */
inline std::uint32_t current_starttime() noexcept {
    static std::uint32_t cached_pid = 0;
    static std::uint32_t cached_start = 0;
    std::uint32_t const pid = current_pid();
    if (cached_pid == pid && pid != 0) return cached_start;
    char state = '?';
    std::uint32_t start = 0;
    proc_state const ps = proc_stat(pid, &state, &start);
    cached_pid = pid;
    cached_start = (ps == proc_state::parsed) ? start : 0;
    return cached_start;
}

/** owner 记录打包: [0:24) pid | [24:56) starttime | [56:64) gen; 0 = 槽空。 */
inline std::uint64_t owner_pack(std::uint32_t pid, std::uint32_t start,
                                std::uint32_t gen = 0) noexcept {
    return (static_cast<std::uint64_t>(pid) & 0xFFFFFFull) |
           ((static_cast<std::uint64_t>(start) & 0xFFFFFFFFull) << 24) |
           ((static_cast<std::uint64_t>(gen) & 0xFFull) << 56);
}

inline std::uint32_t owner_pid_of(std::uint64_t rec) noexcept {
    return static_cast<std::uint32_t>(rec & 0xFFFFFFull);
}

inline std::uint32_t owner_start_of(std::uint64_t rec) noexcept {
    return static_cast<std::uint32_t>((rec >> 24) & 0xFFFFFFFFull);
}

inline std::uint32_t owner_gen_of(std::uint64_t rec) noexcept {
    return static_cast<std::uint32_t>((rec >> 56) & 0xFFull);
}

/** 该记录的持有者**是否仍须视为活着**(含一切不确定). 只有三种确定证据判死:
 *   ① stat 读不到且 errno == ENOENT(进程不存在);
 *   ② state 为 'Z'(zombie: 地址空间已销毁, 它的 buff_t 不可能再析构)或 'X';
 *   ③ stat 可读但 starttime 不吻合(pid 已被复用 ⇒ 原持有者已死)。
 *  其余(槽空/权限不足/解析失败)一律返回 true —— 保守方向是"判活 ⇒ 不回收",
 *  失败模式退化为现状行为(漏而不夺), 无回归风险。 */
inline bool owner_maybe_alive(std::uint64_t rec) noexcept {
    if (rec == 0) return true;                  // 槽空: 归属不明
    std::uint32_t const pid = owner_pid_of(rec);
    if (pid == 0) return true;
    char state = '?';
    std::uint32_t start = 0;
    proc_state const ps = proc_stat(pid, &state, &start);
    if (ps == proc_state::missing) return false;
    if (ps != proc_state::parsed) return true;
    if (state == 'Z' || state == 'X') return false;
    return start == owner_start_of(rec);
}

/** 单位位掩码(1<<n) → 位号 n; 非单位掩码返回 cap(非法)。 */
inline std::uint32_t bit_slot_of(ipc::circ::cc_t bit) noexcept {
    for (std::uint32_t i = 0; i < sizeof(ipc::circ::cc_t) * 8; ++i)
        if (bit == (static_cast<ipc::circ::cc_t>(1) << i)) return i;
    return static_cast<std::uint32_t>(sizeof(ipc::circ::cc_t) * 8);
}

/** 路由标签: conn 段名的 FNV-1a 散列。用于"共享池中只回收本路由的 chunk"
 *  —— chunk 位图的位号只在同路由的 owner 表里有意义, 跨路由解释即错。
 *  两个进程对同一路由名算出同一标签; 0 保留为"未标记"。 */
inline std::uint32_t route_tag_of(char const *name) noexcept {
    std::uint32_t h = 2166136261u;
    for (char const *p = name; p != nullptr && *p != '\0'; ++p) {
        h ^= static_cast<std::uint8_t>(*p);
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

/** owner 表: 位号 → 持有者身份 (pid, starttime, gen)。位宽与 cc_t 一致。
 *  - 内嵌在 elems 尾部(槽数组之后), shm 零填充即初始全空;
 *  - 写入者只有"刚拿到该位的收方进程"(见 elem_array::connect_receiver);
 *  - 读用者可并发(清扫方), 故字段都是 8B 单字原子 —— 无撕裂。 */
struct owner_table {
    enum : std::size_t { cap = sizeof(ipc::circ::cc_t) * 8 };

    std::atomic<std::uint64_t> slot[cap];

    /** 连接成功、拿到位 index 后声明槽。false ⇒ 该槽被"活"持有者占着
     *  (位分配器已发同一位的畸形态), 调用方必须回滚位。 */
    bool claim(std::uint32_t index) noexcept {
        if (index >= cap) return false;
        std::uint64_t const prev = slot[index].load(std::memory_order_acquire);
        if (prev != 0 && owner_maybe_alive(prev)) return false;
        std::uint32_t const gen =
            (prev == 0) ? 0u : (owner_gen_of(prev) + 1u);
        slot[index].store(
            owner_pack(current_pid(), current_starttime(), gen),
            std::memory_order_release);
        return true;
    }

    /** 干净断连: 清自己的槽(调用时位应已先清)。 */
    void release(std::uint32_t index) noexcept {
        if (index < cap) slot[index].store(0, std::memory_order_release);
    }

    /** 位掩码被整体清空(disconnect_receiver(~0))时清全部槽。 */
    void release_all() noexcept {
        for (std::size_t i = 0; i < cap; ++i)
            slot[i].store(0, std::memory_order_release);
    }

    std::uint64_t peek(std::uint32_t index) const noexcept {
        return index < cap ? slot[index].load(std::memory_order_acquire) : 0;
    }

    /** 位掩码中"持有者确定已死"的位集合(池穷尽清扫用)。槽空视为不确定,
     *  不进死集 —— 清扫方据此整块放弃(保守)。 */
    ipc::circ::cc_t proven_dead_bits(ipc::circ::cc_t bits) const noexcept {
        ipc::circ::cc_t dead = 0;
        for (std::uint32_t i = 0; i < cap; ++i) {
            ipc::circ::cc_t const bit = static_cast<ipc::circ::cc_t>(1) << i;
            if ((bits & bit) == 0) continue;
            if (!owner_maybe_alive(slot[i].load(std::memory_order_acquire)))
                dead = static_cast<ipc::circ::cc_t>(dead | bit);
        }
        return dead;
    }
};

class conn_head_base {
protected:
    std::atomic<cc_t> cc_{0}; // connections
    ipc::spin_lock lc_;
    std::atomic<bool> constructed_{false};

public:
    void init() {
        /* DCLP */
        if (!constructed_.load(std::memory_order_acquire)) {
            IPC_UNUSED_ auto guard = ipc::detail::unique_lock(lc_);
            if (!constructed_.load(std::memory_order_relaxed)) {
                ::new (this) conn_head_base;
                constructed_.store(true, std::memory_order_release);
            }
        }
    }

    conn_head_base() = default;
    conn_head_base(conn_head_base const &) = delete;
    conn_head_base &operator=(conn_head_base const &) = delete;

    cc_t connections(std::memory_order order = std::memory_order_acquire) const noexcept {
        return this->cc_.load(order);
    }
};

template <typename P, bool = relat_trait<P>::is_broadcast>
class conn_head;

template <typename P>
class conn_head<P, true> : public conn_head_base {
public:
    cc_t connect() noexcept {
        for (unsigned k = 0;; ipc::yield(k)) {
            cc_t curr = this->cc_.load(std::memory_order_acquire);
            cc_t next = curr | (curr + 1); // find the first 0, and set it to 1.
            if (next == curr) {
                // connection-slot is full.
                return 0;
            }
            if (this->cc_.compare_exchange_weak(curr, next, std::memory_order_release)) {
                return next ^ curr; // return connected id
            }
        }
    }

    cc_t disconnect(cc_t cc_id) noexcept {
        return this->cc_.fetch_and(~cc_id, std::memory_order_acq_rel) & ~cc_id;
    }

    bool connected(cc_t cc_id) const noexcept {
        return (this->connections() & cc_id) != 0;
    }

    std::size_t conn_count(std::memory_order order = std::memory_order_acquire) const noexcept {
        cc_t cur = this->cc_.load(order);
        cc_t cnt; // accumulates the total bits set in cc
        for (cnt = 0; cur; ++cnt) cur &= cur - 1;
        return cnt;
    }
};

template <typename P>
class conn_head<P, false> : public conn_head_base {
public:
    cc_t connect() noexcept {
        return this->cc_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    cc_t disconnect(cc_t cc_id) noexcept {
        if (cc_id == ~static_cast<circ::cc_t>(0u)) {
            // clear all connections
            this->cc_.store(0, std::memory_order_relaxed);
            return 0u;
        }
        else {
            return this->cc_.fetch_sub(1, std::memory_order_relaxed) - 1;
        }
    }

    bool connected(cc_t cc_id) const noexcept {
        // In non-broadcast mode, connection tags are only used for counting.
        return (this->connections() != 0) && (cc_id != 0);
    }

    std::size_t conn_count(std::memory_order order = std::memory_order_acquire) const noexcept {
        return this->connections(order);
    }
};

} // namespace circ
} // namespace ipc
