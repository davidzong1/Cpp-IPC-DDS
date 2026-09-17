#pragma once

#include <new>
#include <utility>

#include "libipc/export.h"
#include "libipc/def.h"

namespace ipc {
namespace mem {

class IPC_EXPORT pool_alloc {
public:
    static void* alloc(std::size_t size) noexcept;
    static void  free (void* p, std::size_t size) noexcept;
};

////////////////////////////////////////////////////////////////
/// construct/destruct an object
////////////////////////////////////////////////////////////////

namespace detail {

template <typename T>
struct impl {
    template <typename... P>
    static T* construct(T* p, P&&... params) {
        ::new (p) T(std::forward<P>(params)...);
        return p;
    }

    static void destruct(T* p) {
        reinterpret_cast<T*>(p)->~T();
    }
};

template <typename T, size_t N>
struct impl<T[N]> {
    using type = T[N];

    template <typename... P>
    static type* construct(type* p, P&&... params) {
        for (size_t i = 0; i < N; ++i) {
            impl<T>::construct(&((*p)[i]), std::forward<P>(params)...);
        }
        return p;
    }

    static void destruct(type* p) {
        for (size_t i = 0; i < N; ++i) {
            impl<T>::destruct(&((*p)[i]));
        }
    }
};

} // namespace detail

template <typename T, typename... P>
T* construct(T* p, P&&... params) {
    return detail::impl<T>::construct(p, std::forward<P>(params)...);
}

template <typename T, typename... P>
T* construct(void* p, P&&... params) {
    return construct(static_cast<T*>(p), std::forward<P>(params)...);
}

template <typename T>
void destruct(T* p) {
    return detail::impl<T>::destruct(p);
}

template <typename T>
void destruct(void* p) {
    destruct(static_cast<T*>(p));
}

////////////////////////////////////////////////////////////////
/// general alloc/free
////////////////////////////////////////////////////////////////

inline void* alloc(std::size_t size) {
    return pool_alloc::alloc(size);
}

template <typename T, typename... P>
T* alloc(P&&... params) {
    void* p = pool_alloc::alloc(sizeof(T));
    /* ⛔ 必须在**就地构造之前**判空。construct() 是在传进来的地址上 placement new,
     * 传 nullptr 进去就是让构造函数往地址 0 写 —— 现象是一个只有 ip 的 SIGSEGV
     * (写 NULL / 写 0x10 之类的低位地址), 现场完全看不出"这是分配失败"。
     * 实测: 把 malloc 定向注成失败(64 字节档)后, `ipc::shm::handle` 的构造函数
     * 就在地址 0 上崩掉了(见 docs/shm_defect_fixes.md 第 7 条)。
     * 返回 nullptr 与下面 `alloc(size_t)` 的语义一致, 既有调用方都按空指针处理。 */
    if (p == nullptr) {
        return nullptr;
    }
    return construct<T>(p, std::forward<P>(params)...);
}

inline void free(void* p, std::size_t size) {
    pool_alloc::free(p, size);
}

template <typename T>
void free(T* p) {
    if (p == nullptr) return;
    destruct(p);
    pool_alloc::free(p, sizeof(T));
}

} // namespace mem
} // namespace ipc
