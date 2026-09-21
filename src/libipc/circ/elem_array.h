#pragma once

#include <atomic>   // std::atomic<?>
#include <limits>
#include <utility>
#include <type_traits>

#include "libipc/def.h"
#include "libipc/rw_lock.h"

#include "libipc/circ/elem_def.h"
#include "libipc/platform/detail.h"

namespace ipc {
namespace circ {

template <typename Policy,
          std::size_t DataSize,
          std::size_t AlignSize = (ipc::detail::min)(DataSize, alignof(std::max_align_t))>
class elem_array : public ipc::circ::conn_head<Policy> {
public:
    using base_t   = ipc::circ::conn_head<Policy>;
    using policy_t = Policy;
    using cursor_t = decltype(std::declval<policy_t>().cursor());
    using elem_t   = typename policy_t::template elem_t<DataSize, AlignSize>;

    enum : std::size_t {
        head_size  = sizeof(base_t) + sizeof(policy_t),
        data_size  = DataSize,
        elem_max   = (std::numeric_limits<uint_t<8>>::max)() + 1, // default is 255 + 1
        elem_size  = sizeof(elem_t),
        block_size = elem_size * elem_max
    };

    /// UF-003: 广播策略的 cc 是收方位图(chunk 位图与之同构); 单播是计数。
    /// owner 表的位号语义只对广播成立, 故单播不参与登记/清扫。
    constexpr static bool broadcast_policy = relat_trait<policy_t>::is_broadcast;

private:
    policy_t head_;
    elem_t   block_[elem_max] {};

    /**
     * \remarks 'warning C4348: redefinition of default parameter' with MSVC.
     * \see
     *  - https://stackoverflow.com/questions/12656239/redefinition-of-default-template-parameter
     *  - https://developercommunity.visualstudio.com/content/problem/425978/incorrect-c4348-warning-in-nested-template-declara.html
    */
    template <typename P, bool/* = relat_trait<P>::is_multi_producer*/>
    struct sender_checker;

    template <typename P>
    struct sender_checker<P, true> {
        constexpr static bool connect() noexcept {
            // always return true
            return true;
        }
        constexpr static void disconnect() noexcept {}
    };

    template <typename P>
    struct sender_checker<P, false> {
        bool connect() noexcept {
            return !flag_.test_and_set(std::memory_order_acq_rel);
        }
        void disconnect() noexcept {
            flag_.clear();
        }

    private:
        // in shm, it should be 0 whether it's initialized or not.
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

    template <typename P, bool/* = relat_trait<P>::is_multi_consumer*/>
    struct receiver_checker;

    template <typename P>
    struct receiver_checker<P, true> {
        constexpr static cc_t connect(base_t &conn) noexcept {
            return conn.connect();
        }
        constexpr static cc_t disconnect(base_t &conn, cc_t cc_id) noexcept {
            return conn.disconnect(cc_id);
        }
    };

    template <typename P>
    struct receiver_checker<P, false> : protected sender_checker<P, false> {
        cc_t connect(base_t &conn) noexcept {
            return sender_checker<P, false>::connect() ? conn.connect() : 0;
        }
        cc_t disconnect(base_t &conn, cc_t cc_id) noexcept {
            sender_checker<P, false>::disconnect();
            return conn.disconnect(cc_id);
        }
    };

    sender_checker  <policy_t, relat_trait<policy_t>::is_multi_producer> s_ckr_;
    receiver_checker<policy_t, relat_trait<policy_t>::is_multi_consumer> r_ckr_;

    /* UF-003: 位→持有者身份侧车表。追加在既有成员之后 —— 不改动任何既有
     * 偏移(head_/block_/checkers); 但会让 elems 段变长, 故段名带 __V2 版本
     * 分量隔离新旧二进制(见 ipc.cpp::queue_generator::conn_info_t 与
     * sniffer.cpp 的 QU_CONN__ 名同步点)。 */
    owner_table owners_;

    // make these be private
    using base_t::connect;
    using base_t::disconnect;

public:
    bool connect_sender() noexcept {
        return s_ckr_.connect();
    }

    void disconnect_sender() noexcept {
        return s_ckr_.disconnect();
    }

    cc_t connect_receiver() noexcept {
        cc_t const id = r_ckr_.connect(*this);
        if (!broadcast_policy || id == 0) return id;
        /* UF-003: 位分配器发位后立刻声明 owner 槽("先位后槽", 见 elem_def.h
         * 的线序说明)。声明失败 = 槽被"活"持有者占着(位分配器已发同一位的
         * 畸形态) ⇒ 回滚位, 不允许两个活持有者共用一位。 */
        if (!owners_.claim(bit_slot_of(id))) {
            r_ckr_.disconnect(*this, id);
            return 0;
        }
        return id;
    }

    cc_t disconnect_receiver(cc_t cc_id) noexcept {
        /* UF-003: 先清位、后清槽 —— 于是"位悬挂 + 槽记录属于死进程"只在
         * 崩溃时出现(稳定指纹); 干净断连不会留下可被误判为死的记录。 */
        cc_t const left = r_ckr_.disconnect(*this, cc_id);
        if (broadcast_policy && cc_id != 0) {
            if (cc_id == static_cast<cc_t>(~static_cast<cc_t>(0u))) {
                owners_.release_all();
            } else {
                owners_.release(bit_slot_of(cc_id));
            }
        }
        return left;
    }

    cursor_t cursor() const noexcept {
        return head_.cursor();
    }

    /// Writer's published index. For broadcast policies this equals cursor();
    /// for unicast it is wt_, which cursor() does not expose.
    /// Used by ipc::sniffer for passive (non-registering) observation.
    cursor_t write_index() const noexcept {
        return head_.write_index();
    }

    /// Direct read-only access to the slot array. Used by ipc::sniffer.
    /// Writers/readers should keep going through push/pop instead.
    elem_t const* block() const noexcept { return block_; }
    elem_t      * block()       noexcept { return block_; }

    /// UF-003: owner 表(位→持有者身份)。清扫方在池穷尽时用它做"验尸"。
    /// 只有广播策略在连接/断连时维护它; 单播的 cc 是计数语义, 不参与。
    owner_table       & owners()       noexcept { return owners_; }
    owner_table const & owners() const noexcept { return owners_; }

    template <typename Q, typename F>
    bool push(Q* que, F&& f) {
        return head_.push(que, std::forward<F>(f), block_);
    }

    template <typename Q, typename F>
    bool force_push(Q* que, F&& f) {
        return head_.force_push(que, std::forward<F>(f), block_);
    }

    template <typename Q, typename F>
    bool push_sniffer(Q* que, F&& f) {
        return head_.push_sniffer(que, std::forward<F>(f), block_);
    }

    template <typename Q, typename F, typename R>
    bool pop(Q* que, cursor_t* cur, F&& f, R&& out) {
        if (cur == nullptr) return false;
        return head_.pop(que, *cur, std::forward<F>(f), std::forward<R>(out), block_);
    }
};

} // namespace circ
} // namespace ipc
