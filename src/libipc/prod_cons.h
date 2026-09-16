#pragma once

#include <atomic>
#include <utility>
#include <cstring>
#include <type_traits>
#include <cstdint>
#include <limits>

#include "libipc/def.h"

#include "libipc/platform/detail.h"
#include "libipc/circ/elem_def.h"
#include "libipc/utility/log.h"
#include "libipc/utility/utility.h"

namespace ipc {

////////////////////////////////////////////////////////////////
/// producer-consumer implementation
////////////////////////////////////////////////////////////////

/* 环的槽位数。由 circ::index_of() 的返回类型决定(它把 32 位计数器截成 u1_t), 所以这是
 * 结构性常量而非可配参数 —— 各 policy 共用。 */
constexpr circ::u2_t kRingSlots =
    static_cast<circ::u2_t>((std::numeric_limits<circ::u1_t>::max)()) + 1;

template <typename Flag>
struct prod_cons_impl;

template <>
struct prod_cons_impl<wr<relat::single, relat::single, trans::unicast>> {

    template <std::size_t DataSize, std::size_t AlignSize>
    struct elem_t {
        std::aligned_storage_t<DataSize, AlignSize> data_ {};
    };

    alignas(cache_line_size) std::atomic<circ::u2_t> rd_; // read index
    alignas(cache_line_size) std::atomic<circ::u2_t> wt_; // write index

    constexpr circ::u2_t cursor() const noexcept {
        return 0;
    }

    /// Writer's published position. Used by passive observers (e.g. ipc::sniffer)
    /// to know where the producer is, without touching rd_.
    circ::u2_t write_index() const noexcept {
        return wt_.load(std::memory_order_acquire);
    }
    circ::u2_t read_index() const noexcept {
        return rd_.load(std::memory_order_acquire);
    }

    template <typename W, typename F, typename E>
    bool push(W* /*wrapper*/, F&& f, E* elems) {
        auto cur_wt = circ::index_of(wt_.load(std::memory_order_relaxed));
        if (cur_wt == circ::index_of(rd_.load(std::memory_order_acquire) - 1)) {
            return false; // full
        }
        std::forward<F>(f)(&(elems[cur_wt].data_));
        wt_.fetch_add(1, std::memory_order_release);
        return true;
    }

    /**
     * In single-single-unicast, 'force_push' means 'no reader' or 'the only one reader is dead'.
     * So we could just disconnect all connections of receiver, and return false.
    */
    template <typename W, typename F, typename E>
    bool force_push(W* wrapper, F&&, E*) {
        wrapper->elems()->disconnect_receiver(~static_cast<circ::cc_t>(0u));
        return false;
    }

    template <typename W, typename F, typename R, typename E>
    bool pop(W* /*wrapper*/, circ::u2_t& /*cur*/, F&& f, R&& out, E* elems) {
        auto cur_rd = circ::index_of(rd_.load(std::memory_order_relaxed));
        if (cur_rd == circ::index_of(wt_.load(std::memory_order_acquire))) {
            return false; // empty
        }
        std::forward<F>(f)(&(elems[cur_rd].data_));
        std::forward<R>(out)(true);
        rd_.fetch_add(1, std::memory_order_release);
        return true;
    }
};

template <>
struct prod_cons_impl<wr<relat::single, relat::multi , trans::unicast>>
     : prod_cons_impl<wr<relat::single, relat::single, trans::unicast>> {

    template <typename W, typename F, typename E>
    bool force_push(W* wrapper, F&&, E*) {
        wrapper->elems()->disconnect_receiver(1);
        return false;
    }

    template <typename W, typename F, typename R, 
              template <std::size_t, std::size_t> class E, std::size_t DS, std::size_t AS>
    bool pop(W* /*wrapper*/, circ::u2_t& /*cur*/, F&& f, R&& out, E<DS, AS>* elems) {
        byte_t buff[DS];
        for (unsigned k = 0;;) {
            auto cur_rd = rd_.load(std::memory_order_relaxed);
            if (circ::index_of(cur_rd) ==
                circ::index_of(wt_.load(std::memory_order_acquire))) {
                return false; // empty
            }
            std::memcpy(buff, &(elems[circ::index_of(cur_rd)].data_), sizeof(buff));
            if (rd_.compare_exchange_weak(cur_rd, cur_rd + 1, std::memory_order_release)) {
                std::forward<F>(f)(buff);
                std::forward<R>(out)(true);
                return true;
            }
            ipc::yield(k);
        }
    }
};

template <>
struct prod_cons_impl<wr<relat::multi , relat::multi, trans::unicast>>
     : prod_cons_impl<wr<relat::single, relat::multi, trans::unicast>> {

    using flag_t = std::uint64_t;

    template <std::size_t DataSize, std::size_t AlignSize>
    struct elem_t {
        std::aligned_storage_t<DataSize, AlignSize> data_ {};
        std::atomic<flag_t> f_ct_ { 0 }; // commit flag
    };

    alignas(cache_line_size) std::atomic<circ::u2_t> ct_; // commit index

    template <typename W, typename F, typename E>
    bool push(W* /*wrapper*/, F&& f, E* elems) {
        circ::u2_t cur_ct, nxt_ct;
        for (unsigned k = 0;;) {
            cur_ct = ct_.load(std::memory_order_relaxed);
            if (circ::index_of(nxt_ct = cur_ct + 1) ==
                circ::index_of(rd_.load(std::memory_order_acquire))) {
                return false; // full
            }
            if (ct_.compare_exchange_weak(cur_ct, nxt_ct, std::memory_order_acq_rel)) {
                break;
            }
            ipc::yield(k);
        }
        auto* el = elems + circ::index_of(cur_ct);
        std::forward<F>(f)(&(el->data_));
        // set flag & try update wt
        el->f_ct_.store(~static_cast<flag_t>(cur_ct), std::memory_order_release);
        while (1) {
            auto cac_ct = el->f_ct_.load(std::memory_order_acquire);
            if (cur_ct != wt_.load(std::memory_order_relaxed)) {
                return true;
            }
            if ((~cac_ct) != cur_ct) {
                return true;
            }
            if (!el->f_ct_.compare_exchange_strong(cac_ct, 0, std::memory_order_relaxed)) {
                return true;
            }
            wt_.store(nxt_ct, std::memory_order_release);
            cur_ct = nxt_ct;
            nxt_ct = cur_ct + 1;
            el = elems + circ::index_of(cur_ct);
        }
        return true;
    }

    template <typename W, typename F, typename E>
    bool force_push(W* wrapper, F&&, E*) {
        wrapper->elems()->disconnect_receiver(1);
        return false;
    }

    template <typename W, typename F, typename R, 
              template <std::size_t, std::size_t> class E, std::size_t DS, std::size_t AS>
    bool pop(W* /*wrapper*/, circ::u2_t& /*cur*/, F&& f, R&& out, E<DS, AS>* elems) {
        byte_t buff[DS];
        for (unsigned k = 0;;) {
            auto cur_rd = rd_.load(std::memory_order_relaxed);
            auto cur_wt = wt_.load(std::memory_order_acquire);
            auto id_rd  = circ::index_of(cur_rd);
            auto id_wt  = circ::index_of(cur_wt);
            if (id_rd == id_wt) {
                auto* el = elems + id_wt;
                auto cac_ct = el->f_ct_.load(std::memory_order_acquire);
                if ((~cac_ct) != cur_wt) {
                    return false; // empty
                }
                if (el->f_ct_.compare_exchange_weak(cac_ct, 0, std::memory_order_relaxed)) {
                    wt_.store(cur_wt + 1, std::memory_order_release);
                }
                k = 0;
            }
            else {
                std::memcpy(buff, &(elems[circ::index_of(cur_rd)].data_), sizeof(buff));
                if (rd_.compare_exchange_weak(cur_rd, cur_rd + 1, std::memory_order_release)) {
                    std::forward<F>(f)(buff);
                    std::forward<R>(out)(true);
                    return true;
                }
                ipc::yield(k);
            }
        }
    }
};

template <>
struct prod_cons_impl<wr<relat::single, relat::multi, trans::broadcast>> {

    using rc_t = std::uint64_t;

    enum : rc_t {
        ep_mask = 0x00000000ffffffffull,
        ep_incr = 0x0000000100000000ull
    };

    template <std::size_t DataSize, std::size_t AlignSize>
    struct elem_t {
        std::aligned_storage_t<DataSize, AlignSize> data_ {};
        std::atomic<rc_t> rc_ { 0 }; // read-counter
    };

    alignas(cache_line_size) std::atomic<circ::u2_t> wt_;   // write index
    alignas(cache_line_size) rc_t epoch_ { 0 };             // only one writer

    circ::u2_t cursor() const noexcept {
        return wt_.load(std::memory_order_acquire);
    }

    /// Writer's published position; identical to cursor() for broadcast policies.
    circ::u2_t write_index() const noexcept {
        return wt_.load(std::memory_order_acquire);
    }

    template <typename W, typename F, typename E>
    bool push(W* wrapper, F&& f, E* elems) {
        E* el;
        circ::cc_t rem_cc = 0;
        for (unsigned k = 0;;) {
            circ::cc_t cc = wrapper->elems()->connections(std::memory_order_relaxed);
            if (cc == 0) return false; // no reader
            el = elems + circ::index_of(wt_.load(std::memory_order_relaxed));
            // check all consumers have finished reading this element
            auto cur_rc = el->rc_.load(std::memory_order_acquire);
            rem_cc = cur_rc & ep_mask;
            if ((cc & rem_cc) && ((cur_rc & ~ep_mask) == epoch_)) {
                return false; // has not finished yet
            }
            /* 注意这里**不是**"rem_cc 必然为 0"。
             *
             * 上面那个判据只在"仍有在连读方持有本格 **且** 该持有属于当前世代"时才判满。
             * 一旦此前有过 force_push 抬升 epoch_, 早于那次抬升写入的格子就带着**旧世代**,
             * 于是即便读方的位还置着(它被套圈了, 永远不会来取), 判据也不成立 —— push 会
             * 直接覆写。这是有意的"慢订阅者丢失被套圈的消息"语义。
             *
             * 但被覆写的那条消息如果是大消息, 它的 chunk 必须有人归还: 读方不会来取,
             * 所以只能由这里的覆写回调处理。旧实现给 push 传的是空回调(只有 force_push
             * 才带 clear_message), 于是**每次这样的覆写都漏一块 chunk** —— 实测洪泛 768
             * 条后 32 块全部漏光, 大消息随即永久退化成 64 字节分片。
             *
             * 这个泄漏此前被另一个缺陷掩盖着: 被套圈的读方会把同一格读两次, 同一
             * storage_id 因此二次入池, 恰好补上了漏掉的那些。两个缺陷一起才让池子
             * "看起来"是满的。详见 docs/dzflat_known_issues.md 第 4 条。 */
            if (el->rc_.compare_exchange_weak(
                        cur_rc, epoch_ | static_cast<rc_t>(cc), std::memory_order_release)) {
                break;
            }
            ipc::yield(k);
        }
        std::forward<F>(f)(&(el->data_), rem_cc);
        wt_.fetch_add(1, std::memory_order_release);
        return true;
    }

    template <typename W, typename F, typename E>
    bool force_push(W* wrapper, F&& f, E* elems) {
        E* el;
        circ::cc_t rem_cc = 0;
        epoch_ += ep_incr;
        for (unsigned k = 0;;) {
            circ::cc_t cc = wrapper->elems()->connections(std::memory_order_relaxed);
            if (cc == 0) return false; // no reader
            el = elems + circ::index_of(wt_.load(std::memory_order_relaxed));
            auto cur_rc = el->rc_.load(std::memory_order_acquire);
            rem_cc = cur_rc & ep_mask;
            // 覆写一个仍被订阅者持有的槽位。
            //
            // 这里曾经调用 disconnect_receiver(rem_cc) 把这些订阅者强制踢下线,
            // 注释称其为 "invalid readers" —— 但 rem_cc 的真实语义只是"尚未释放
            // 这一格", 慢了一格的活订阅者和已死的进程在这个判据下无法区分。被踢
            // 掉的一方会被永久摘出连接位图且收不到任何通知, 表现为链路直接断掉。
            // 丢一帧可恢复, 断连不可恢复, 与 force_push 想要的 best-effort 语义
            // 也不符。故不再断开: 上面已自增 epoch_, 本格残留的读计数必然属于更老
            // 的世代, 直接 CAS 覆盖即可, 慢订阅者保持连接、只丢失被套圈的消息。
            // 真正死掉的连接交由上层心跳清理, 不在写路径上判定。
            //
            // 注意: 覆写与 pop() 的拷贝之间仍无互斥, 对方可能正在读本格 → 数据
            // 撕裂风险尚未消除(需在 pop() 侧增加覆写检测), 故此处限量告警。
            if (cc & rem_cc) {
                static std::atomic<unsigned> warned{0};
                const unsigned n = warned.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    ipc::log("force_push: overwriting slot still held by reader(s); "
                             "k = %u, cc = %u, rem_cc = %u\n", k, cc, rem_cc);
                } else if (n == 8) {
                    ipc::log("force_push: further overwrite warnings suppressed\n");
                }
            }
            // just compare & exchange
            if (el->rc_.compare_exchange_weak(
                        cur_rc, epoch_ | static_cast<rc_t>(cc), std::memory_order_release)) {
                break; // rem_cc 取自 CAS 成功的那一轮, 与被覆写的世代一致
            }
            ipc::yield(k);
        }
        // rem_cc = 尚未 pop 本格的接收方, 即"永远看不到被覆写这条消息"的那一批。
        // 覆写回调(clear_message)据此判定被丢弃消息的 chunk 能否立即归还:
        // 这些位必须从 chunk 的 conns 里清掉(否则位图永不归零 → chunk 泄漏),
        // 而已经 pop 过、可能仍持有 buff_t 的接收方的位必须保留(否则 chunk id
        // 会在持有者手里被回池复用)。详见 ipc.cpp: discard_storage。
        std::forward<F>(f)(&(el->data_), rem_cc);
        wt_.fetch_add(1, std::memory_order_release);
        return true;
    }

    template <typename W, typename F, typename E>
    bool push_sniffer(W* /*wrapper*/, F&& f, E* elems) {
        E* el = elems + circ::index_of(wt_.load(std::memory_order_relaxed));
        epoch_ += ep_incr;
        el->rc_.store(epoch_, std::memory_order_release);
        std::forward<F>(f)(&(el->data_));
        wt_.fetch_add(1, std::memory_order_release);
        return true;
    }

    template <typename W, typename F, typename R, typename E>
    bool pop(W* wrapper, circ::u2_t& cur, F&& f, R&& out, E* elems) {
        /* 套圈(lapped)与撕裂检测。见 docs/dzflat_known_issues.md 第 3/4/5 条。
         *
         * 背景: cur 是 32 位计数器, 而 index_of() 只取低 8 位 —— 环长 256。一旦读方落后
         * 写指针超过 256, cur 与 cur+256 就映射到**同一个环槽位**。旧实现对此毫无察觉,
         * 于是同一格内容被投递两次:
         *   - 大消息(msg.storage_)会因此产生两个 buff_t → recycle_storage 走两次 →
         *     同一 storage_id 被重复放回 id_pool, 空闲链表接成自环并丢掉其后全部 id;
         *   - 分片消息会让按 msg.id_ 索引的重组缓存混进不同世代的分片, 拼出错乱缓冲,
         *     进而让 TLV deserialize 按缓冲内容算出的偏移越界读(实测 SIGSEGV)。
         *
         * 两道判据, 都只用已有状态, 不加字段:
         *
         *   ① **本格我是否已消费过** —— el->rc_ 的低 32 位是"尚未放行本格的读方位图",
         *      写方落格时把全部在连位都置上, 读方在 pop 末尾清掉自己那一位。所以
         *      "我的位已清" ⟺ 这一格的当前内容我已经取走过 ⟹ 套圈重读, 跳过。
         *      (新接入的读方 cursor_ 取自当时的写指针, 见 queue.h connect(), 所以不会
         *       因为"入连前写入的格子"而误跳。)
         *
         *   ② **拷贝期间是否被覆写** —— 只有 force_push 能覆写我还持位的格子(普通 push
         *      在 cc & rem_cc != 0 时直接返回 false), 而 force_push 必然先自增 epoch_。
         *      于是拷贝前后比一次 epoch 位(rc_ 的高 32 位)就够: 变了说明本格在我拷贝
         *      途中被覆写, 数据可能撕裂 → 丢弃。这就是 §5.1 里登记的那个残余窗口。
         *
         * 跳过的格子只是 continue, 不返回 false —— pop 的语义仍是"没有可投递的消息才
         * 返回 false", 否则被套圈的读方每跳一格都要惊动一次 wait_for。
         */
        const rc_t my = static_cast<rc_t>(wrapper->connected_id());
        /* ③ 被套圈则重同步游标。
         *
         * 读方落后写指针超过一整圈时, cur 指向的那些格子早已被写方甩过多轮 —— 它们的
         * 当前内容属于**更新的世代**, 与 cur 无关。继续按 cur 逐格读有两类恶果:
         *   - 同一份内容被读多次(靠下面的 ① 位检查能挡, 但那只是止损);
         *   - 更糟: 从被甩过的格子里读出的 storage_id 可能已归还并被复用, 于是读方的
         *     buff_t 析构时会回收**别人正在用的** chunk(ABA), 池子记账随即错乱 —— 实测
         *     表现为 chunk 池缩水到 2/20, 大消息全部退化成分片。
         *
         * 正确做法就是 force_push 注释里早已声明的语义: 慢订阅者保持连接、只丢失被套圈
         * 的消息。所以把游标跳到"环里还留着的最旧一格", 中间的全部放弃。 */
        if (static_cast<circ::u2_t>(cursor() - cur) > kRingSlots) {
            cur = static_cast<circ::u2_t>(cursor() - kRingSlots);
        }
        for (;;) {
            if (cur == cursor()) return false; // acquire
            auto* el = elems + circ::index_of(cur++);
            const rc_t rc0 = el->rc_.load(std::memory_order_acquire);
            if ((my != 0) && ((rc0 & my) == 0)) {
                continue;   // ① 本格内容已消费过 → 套圈重读
            }
            std::forward<F>(f)(&(el->data_));
            const rc_t rc1 = el->rc_.load(std::memory_order_acquire);
            if ((rc1 & ~static_cast<rc_t>(ep_mask)) != (rc0 & ~static_cast<rc_t>(ep_mask))) {
                continue;   // ② 拷贝期间被 force_push 覆写 → 可能撕裂
            }
            for (unsigned k = 0;;) {
                auto cur_rc = el->rc_.load(std::memory_order_acquire);
                /* ③ ② 之后还有一个窗口: force_push 落在 rc1 读完之后、下面 clear CAS
                 *    成功之前。
                 *
                 * f() 已把这格内容(对 storage 消息就是 storage_id)拷进调用方局部, 而
                 * force_push 的 discard_storage 按 rem_cc 清 chunk conns 位 —— rem_cc
                 * 捕获时本读方的 rc 位还没清, 于是被当成"永远看不到这格"的读方; 若它
                 * 恰好是最后持有者, chunk 当场归还池中。若不拦, 我们随后照样 clear 自己
                 * 的 rc 位、return true, recv 就会为一个已归还/可能已复用的 storage_id
                 * 建 buff_t → ABA / double-free。
                 *
                 * 这正是 ipc.cpp discard_storage 注释预告的场景: 一旦接收方把 chunk 拿
                 * 出去长期持有(DZFlat Sample), 该窗口不再只有几条指令宽, 必然可复现。
                 *
                 * 拦法: 进 clear 循环先重检 epoch。变了 = 本格被 force_push 覆写、旧内容
                 * 已被 discard_storage 收回 —— 绝不能让 recv 拿到 f() 拷出的旧值, 于是
                 * 跳过本格(break 出内层, 落到外层 continue, cur 已 advance)。与 ② 同
                 * 语义: force_push 只在环满时发生, 读方本就落后, 丢这一格符合"慢订阅者
                 * 丢消息"的契约; 且我们的 rc 位已随新格重新置上, 套圈回来仍能读到它,
                 * 不会双投。 */
                if ((cur_rc & ~static_cast<rc_t>(ep_mask)) != (rc0 & ~static_cast<rc_t>(ep_mask))) {
                    break;   // → 外层 for 继续读下一格(不经 out, 不 return true)
                }
                if ((cur_rc & ep_mask) == 0) {
                    std::forward<R>(out)(true);
                    return true;
                }
                auto nxt_rc = cur_rc & ~static_cast<rc_t>(wrapper->connected_id());
                if (el->rc_.compare_exchange_weak(cur_rc, nxt_rc, std::memory_order_release)) {
                    std::forward<R>(out)((nxt_rc & ep_mask) == 0);
                    return true;
                }
                ipc::yield(k);
            }
        }
    }
};

template <>
struct prod_cons_impl<wr<relat::multi, relat::multi, trans::broadcast>> {

    using rc_t   = std::uint64_t;
    using flag_t = std::uint64_t;

    enum : rc_t {
        rc_mask = 0x00000000ffffffffull,
        ep_mask = 0x00ffffffffffffffull,
        ep_incr = 0x0100000000000000ull,
        ic_mask = 0xff000000ffffffffull,
        ic_incr = 0x0000000100000000ull
    };

    template <std::size_t DataSize, std::size_t AlignSize>
    struct elem_t {
        std::aligned_storage_t<DataSize, AlignSize> data_ {};
        std::atomic<rc_t  > rc_   { 0 }; // read-counter
        std::atomic<flag_t> f_ct_ { 0 }; // commit flag
    };

    alignas(cache_line_size) std::atomic<circ::u2_t> ct_;   // commit index
    alignas(cache_line_size) std::atomic<rc_t> epoch_ { 0 };

    circ::u2_t cursor() const noexcept {
        return ct_.load(std::memory_order_acquire);
    }

    /// Writer's published position; identical to cursor() for broadcast policies.
    circ::u2_t write_index() const noexcept {
        return ct_.load(std::memory_order_acquire);
    }

    constexpr static rc_t inc_rc(rc_t rc) noexcept {
        return (rc & ic_mask) | ((rc + ic_incr) & ~ic_mask);
    }

    constexpr static rc_t inc_mask(rc_t rc) noexcept {
        return inc_rc(rc) & ~rc_mask;
    }

    template <typename W, typename F, typename E>
    bool push(W* wrapper, F&& f, E* elems) {
        E* el;
        circ::u2_t cur_ct;
        rc_t epoch = epoch_.load(std::memory_order_acquire);
        for (unsigned k = 0;;) {
            circ::cc_t cc = wrapper->elems()->connections(std::memory_order_relaxed);
            if (cc == 0) return false; // no reader
            el = elems + circ::index_of(cur_ct = ct_.load(std::memory_order_relaxed));
            // check all consumers have finished reading this element
            auto cur_rc = el->rc_.load(std::memory_order_relaxed);
            circ::cc_t rem_cc = cur_rc & rc_mask;
            if ((cc & rem_cc) && ((cur_rc & ~ep_mask) == epoch)) {
                return false; // has not finished yet
            }
            else if (!rem_cc) {
                auto cur_fl = el->f_ct_.load(std::memory_order_acquire);
                if ((cur_fl != cur_ct) && cur_fl) {
                    return false; // full
                }
            }
            // consider rem_cc to be 0 here
            if (el->rc_.compare_exchange_weak(
                        cur_rc, inc_mask(epoch | (cur_rc & ep_mask)) | static_cast<rc_t>(cc), std::memory_order_relaxed) &&
                epoch_.compare_exchange_weak(epoch, epoch, std::memory_order_acq_rel)) {
                break;
            }
            ipc::yield(k);
        }
        // only one thread/process would touch here at one time
        ct_.store(cur_ct + 1, std::memory_order_release);
        std::forward<F>(f)(&(el->data_));
        // set flag & try update wt
        el->f_ct_.store(~static_cast<flag_t>(cur_ct), std::memory_order_release);
        return true;
    }

    template <typename W, typename F, typename E>
    bool force_push(W* wrapper, F&& f, E* elems) {
        E* el;
        circ::u2_t cur_ct;
        circ::cc_t rem_cc = 0;
        rc_t epoch = epoch_.fetch_add(ep_incr, std::memory_order_release) + ep_incr;
        for (unsigned k = 0;;) {
            circ::cc_t cc = wrapper->elems()->connections(std::memory_order_relaxed);
            if (cc == 0) return false; // no reader
            el = elems + circ::index_of(cur_ct = ct_.load(std::memory_order_relaxed));
            // check all consumers have finished reading this element
            auto cur_rc = el->rc_.load(std::memory_order_acquire);
            rem_cc = cur_rc & rc_mask;
            // 同 <single,multi,broadcast>::force_push: 只覆写, 不再 disconnect_receiver。
            // 详细理由见该处注释。
            if (cc & rem_cc) {
                static std::atomic<unsigned> warned{0};
                const unsigned n = warned.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    ipc::log("force_push: overwriting slot still held by reader(s); "
                             "k = %u, cc = %u, rem_cc = %u\n", k, cc, rem_cc);
                } else if (n == 8) {
                    ipc::log("force_push: further overwrite warnings suppressed\n");
                }
            }
            // just compare & exchange
            if (el->rc_.compare_exchange_weak(
                        cur_rc, inc_mask(epoch | (cur_rc & ep_mask)) | static_cast<rc_t>(cc), std::memory_order_relaxed)) {
                if (epoch == epoch_.load(std::memory_order_acquire)) {
                    break; // rem_cc 取自 CAS 成功的那一轮
                }
                else if (push(wrapper, std::forward<F>(f), elems)) {
                    // 退化为 push(): 它只占用"读方都已放行"的槽位, 不存在"永远看不
                    // 到"的接收方, 故覆写回调按 rem_cc = 0 处理(见 queue.h 的默认实参)。
                    return true;
                }
                epoch = epoch_.fetch_add(ep_incr, std::memory_order_release) + ep_incr;
            }
            ipc::yield(k);
        }
        // only one thread/process would touch here at one time
        ct_.store(cur_ct + 1, std::memory_order_release);
        // rem_cc 语义与 <single,multi,broadcast>::force_push 一致, 见该处注释。
        std::forward<F>(f)(&(el->data_), rem_cc);
        // set flag & try update wt
        el->f_ct_.store(~static_cast<flag_t>(cur_ct), std::memory_order_release);
        return true;
    }

    template <typename W, typename F, typename R, typename E, std::size_t N>
    bool pop(W* wrapper, circ::u2_t& cur, F&& f, R&& out, E(& elems)[N]) {
        auto* el = elems + circ::index_of(cur);
        auto cur_fl = el->f_ct_.load(std::memory_order_acquire);
        if (cur_fl != ~static_cast<flag_t>(cur)) {
            return false; // empty
        }
        ++cur;
        std::forward<F>(f)(&(el->data_));
        for (unsigned k = 0;;) {
            auto cur_rc = el->rc_.load(std::memory_order_acquire);
            if ((cur_rc & rc_mask) == 0) {
                std::forward<R>(out)(true);
                el->f_ct_.store(cur + N - 1, std::memory_order_release);
                return true;
            }
            auto nxt_rc = inc_rc(cur_rc) & ~static_cast<rc_t>(wrapper->connected_id());
            bool last_one = false;
            if ((last_one = (nxt_rc & rc_mask) == 0)) {
                el->f_ct_.store(cur + N - 1, std::memory_order_release);
            }
            if (el->rc_.compare_exchange_weak(cur_rc, nxt_rc, std::memory_order_release)) {
                std::forward<R>(out)(last_one);
                return true;
            }
            ipc::yield(k);
        }
    }
};

} // namespace ipc
