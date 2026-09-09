#pragma once

#include <type_traits>
#include <new>
#include <utility>  // [[since C++14]]: std::exchange
#include <algorithm>
#include <atomic>
#include <tuple>
#include <thread>
#include <chrono>
#include <string>
#include <cassert>  // assert

#include "libipc/def.h"
#include "libipc/shm.h"
#include "libipc/rw_lock.h"

#include "libipc/utility/log.h"
#include "libipc/platform/detail.h"
#include "libipc/circ/elem_def.h"
#include "libipc/memory/resource.h"

namespace ipc {
namespace detail {

class queue_conn {
protected:
    circ::cc_t connected_ = 0;
    shm::handle elems_h_;

    template <typename Elems>
    Elems* open(char const * name) {
        if (!is_valid_string(name)) {
            ipc::error("fail open waiter: name is empty!\n");
            return nullptr;
        }
        if (!elems_h_.acquire(name, sizeof(Elems))) {
            return nullptr;
        }
        auto elems = static_cast<Elems*>(elems_h_.get());
        if (elems == nullptr) {
            ipc::error("fail acquire elems: %s\n", name);
            return nullptr;
        }
        elems->init();
        return elems;
    }

    void close() {
        elems_h_.release();
    }

public:
    queue_conn() = default;
    queue_conn(const queue_conn&) = delete;
    queue_conn& operator=(const queue_conn&) = delete;

    void clear() noexcept {
        elems_h_.clear();
    }

    static void clear_storage(char const *name) noexcept {
        shm::handle::clear_storage(name);
    }

    template <typename Elems>
    bool connected(Elems* elems) const noexcept {
        return elems->connected(connected_);
    }

    circ::cc_t connected_id() const noexcept {
        return connected_;
    }

    template <typename Elems>
    auto connect(Elems* elems) noexcept
                         /*needs 'optional' here*/
     -> std::tuple<bool, bool, decltype(std::declval<Elems>().cursor())> {
        if (elems == nullptr) return {};
        // if it's already connected, just return
        if (connected(elems)) return {connected(elems), false, 0};
        connected_ = elems->connect_receiver();
        return {connected(elems), true, elems->cursor()};
    }

    template <typename Elems>
    bool disconnect(Elems* elems) noexcept {
        if (elems == nullptr) return false;
        // if it's already disconnected, just return false
        if (!connected(elems)) return false;
        elems->disconnect_receiver(std::exchange(connected_, 0));
        return true;
    }
};

template <typename Elems>
class queue_base : public queue_conn {
    using base_t = queue_conn;

public:
    using elems_t  = Elems;
    using policy_t = typename elems_t::policy_t;

protected:
    elems_t * elems_ = nullptr;
    decltype(std::declval<elems_t>().cursor()) cursor_ = 0;
    bool sender_flag_ = false;

public:
    using base_t::base_t;

    queue_base() = default;

    explicit queue_base(char const * name)
        : queue_base{} {
        elems_ = queue_conn::template open<elems_t>(name);
    }

    explicit queue_base(elems_t * elems) noexcept
        : queue_base{} {
        assert(elems != nullptr);
        elems_ = elems;
    }

    /* not virtual */ ~queue_base() {
        base_t::close();
    }

    bool open(char const * name) noexcept {
        base_t::close();
        elems_ = queue_conn::template open<elems_t>(name);
        return elems_ != nullptr;
    }

    void clear() noexcept {
        base_t::clear();
        elems_ = nullptr;
    }

    elems_t       * elems()       noexcept { return elems_; }
    elems_t const * elems() const noexcept { return elems_; }

    bool ready_sending() noexcept {
        if (elems_ == nullptr) return false;
        return sender_flag_ || (sender_flag_ = elems_->connect_sender());
    }

    void shut_sending() noexcept {
        if (elems_ == nullptr) return;
        if (!sender_flag_) return;
        elems_->disconnect_sender();
    }

    bool connected() const noexcept {
        return base_t::connected(elems_);
    }

    bool connect() noexcept {
        auto tp = base_t::connect(elems_);
        if (std::get<0>(tp) && std::get<1>(tp)) {
            cursor_ = std::get<2>(tp);
            return true;
        }
        return std::get<0>(tp);
    }

    bool disconnect() noexcept {
        return base_t::disconnect(elems_);
    }

    std::size_t conn_count() const noexcept {
        return (elems_ == nullptr) ? static_cast<std::size_t>(invalid_value) : elems_->conn_count();
    }

    bool valid() const noexcept {
        return elems_ != nullptr;
    }

    bool empty() const noexcept {
        return !valid() || (cursor_ == elems_->cursor());
    }

    template <typename T, typename F, typename... P>
    bool push(F&& prep, P&&... params) {
        if (elems_ == nullptr) return false;
        /* rem_cc 与 force_push 同义: 被覆写消息的"永远不会来取它"的读方位图。
         * push 也会覆写被套圈的格子(见 prod_cons.h 里 <single,multi,broadcast>::push
         * 的注释), 所以它同样需要把被丢弃消息的 chunk 交给 prep 处理。
         * 默认实参是为了那些不传第二个参数的 policy(如 unicast 与 multi-multi), 它们
         * 按 rem_cc = 0 处理 —— 只在 conns 已归零时归还, 最保守。 */
        return elems_->push(this, [&](void* p, ipc::circ::cc_t rem_cc = 0) {
            if (prep(p, rem_cc)) ::new (p) T(std::forward<P>(params)...);
        });
    }

    template <typename T, typename F, typename... P>
    bool force_push(F&& prep, P&&... params) {
        if (elems_ == nullptr) return false;
        // rem_cc: 被覆写消息的"永远看不到它"的接收方位图, 由 broadcast 策略的
        // force_push 提供, 交给 prep 判定被丢弃消息的 chunk 能否立即归还。
        // 默认实参是为了 <multi,multi,broadcast>::force_push 退化调用 push() 的
        // 那一条路径 —— push() 只用 1 个实参调用回调, 此时 rem_cc 按 0 处理最保守。
        return elems_->force_push(this, [&](void* p, ipc::circ::cc_t rem_cc = 0) {
            if (prep(p, rem_cc)) ::new (p) T(std::forward<P>(params)...);
        });
    }

    template <typename T, typename F, typename... P>
    bool push_sniffer(F&& prep, P&&... params) {
        if (elems_ == nullptr) return false;
        if constexpr (!relat_trait<policy_t>::is_broadcast || relat_trait<policy_t>::is_multi_producer) {
            return this->push<T>(std::forward<F>(prep), std::forward<P>(params)...);
        }
        else {
            return elems_->push_sniffer(this, [&](void* p) {
                /* push_sniffer 只在无接收方时使用, 不存在"被覆写消息的读方"这回事,
                 * 故 rem_cc 恒为 0。 */
                if (prep(p, ipc::circ::cc_t{0})) ::new (p) T(std::forward<P>(params)...);
            });
        }
    }

    template <typename T, typename F>
    bool pop(T& item, F&& out) {
        if (elems_ == nullptr) {
            return false;
        }
        return elems_->pop(this, &(this->cursor_), [&item](void* p) {
            ::new (&item) T(std::move(*static_cast<T*>(p)));
        }, std::forward<F>(out));
    }
};

} // namespace detail

template <typename T, typename Policy>
class queue final : public detail::queue_base<typename Policy::template elems_t<sizeof(T), alignof(T)>> {
    using base_t = detail::queue_base<typename Policy::template elems_t<sizeof(T), alignof(T)>>;

public:
    using value_t = T;

    using base_t::base_t;

    template <typename... P>
    bool push(P&&... params) {
        return base_t::template push<T>(std::forward<P>(params)...);
    }

    template <typename... P>
    bool force_push(P&&... params) {
        return base_t::template force_push<T>(std::forward<P>(params)...);
    }

    template <typename... P>
    bool push_sniffer(P&&... params) {
        return base_t::template push_sniffer<T>(std::forward<P>(params)...);
    }

    bool pop(T& item) {
        return base_t::pop(item, [](bool) {});
    }

    template <typename F>
    bool pop(T& item, F&& out) {
        return base_t::pop(item, std::forward<F>(out));
    }
};

} // namespace ipc
