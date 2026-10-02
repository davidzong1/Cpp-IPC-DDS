#pragma once

#include <type_traits>  // std::aligned_storage_t
#include <cstring>      // std::memcmp
#include <cstdint>

#include "libipc/def.h"
#include "libipc/platform/detail.h"

namespace ipc {

/* storage_id_t 现由 libipc/def.h 提供(公共 loan API 需要指名它)。 */

template <std::size_t DataSize, std::size_t AlignSize>
struct id_type;

template <std::size_t AlignSize>
struct id_type<0, AlignSize> {
    uint_t<8> id_;

    id_type& operator=(storage_id_t val) {
        id_ = static_cast<uint_t<8>>(val);
        return (*this);
    }

    operator uint_t<8>() const {
        return id_;
    }
};

template <std::size_t DataSize, std::size_t AlignSize>
struct id_type : id_type<0, AlignSize> {
    std::aligned_storage_t<DataSize, AlignSize> data_;
};

template <std::size_t DataSize  = 0,
          std::size_t AlignSize = (ipc::detail::min)(DataSize, alignof(std::max_align_t))>
class id_pool {

    static constexpr std::size_t limited_max_count() {
        return ipc::detail::min<std::size_t>(topic_msg_cache, (std::numeric_limits<uint_t<8>>::max)());
    }

public:
    enum : std::size_t {
        /* eliminate error: taking address of temporary */
        max_count = limited_max_count()
    };

private:
    id_type<DataSize, AlignSize> next_[max_count];
    uint_t<8> cursor_ = 0;
    bool prepared_ = false;

public:
    void prepare() {
        if (!prepared_ && this->invalid()) this->init();
        prepared_ = true;
    }

    void init() {
        for (storage_id_t i = 0; i < max_count;) {
            i = next_[i] = (i + 1);
        }

    }

    bool invalid() const {
        static id_pool inv;
        return std::memcmp(this, &inv, sizeof(id_pool)) == 0;
    }

    bool empty() const {
        return cursor_ == max_count;
    }

    storage_id_t acquire() {
        if (empty()) return -1;
        storage_id_t id = cursor_;
        cursor_ = next_[id]; // point to next
        return id;
    }

    bool release(storage_id_t id) {
        if (id < 0) return false;
        next_[id] = cursor_;
        cursor_ = static_cast<uint_t<8>>(id); // put it back
        return true;
    }

    /**
     * \brief 把空闲链整表复位为「全空闲」。
     *
     * 用途唯一（见 docs/消息接收架构改造/团队改造交付/W09/死进程借样泄漏收口_D14.md）：
     * **池段是崩溃遗留段**时，段内空闲链只反映死进程留下的状态（它借走的 id 永远不会
     * 归还，而 published()==0 的块按 UF-003 §5 的窗口论证⛔不得被逐块清扫）。
     * 判据不是池内状态而是**段级**：该段除本进程外无任何活进程映射 ⇒ 段内不存在在飞
     * 持有者 ⇒ 整表复位安全。
     *
     * ⛔ 调用方必须：①确认段级无活映射者；②在**池锁内**调用（与借出/归还互斥）。
     * 详见 ipc.cpp 的 reclaim_orphan_segment 安全性论证。
     */
    void reset_free_chain() {
        cursor_ = 0;
        init();
        prepared_ = true;
    }

    void       * at(storage_id_t id)       { return &(next_[id].data_); }
    void const * at(storage_id_t id) const { return &(next_[id].data_); }
};

template <typename T>
class obj_pool : public id_pool<sizeof(T), alignof(T)> {
    using base_t = id_pool<sizeof(T), alignof(T)>;

public:
    T       * at(storage_id_t id)       { return reinterpret_cast<T       *>(base_t::at(id)); }
    T const * at(storage_id_t id) const { return reinterpret_cast<T const *>(base_t::at(id)); }
};

} // namespace ipc
