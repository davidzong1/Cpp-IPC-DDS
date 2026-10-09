#pragma once

#include <type_traits>
#include <limits>
#include <utility>
#include <functional>
#include <unordered_map>
#include <map>
#include <string>
#include <string_view>
#include <cstdio>
#include <memory>
#include <atomic>
#include <array>
#include "libipc/export.h"

#include "libipc/def.h"

#include "libipc/memory/alloc.h"
#include "libipc/memory/wrapper.h"
#include "libipc/platform/detail.h"

namespace ipc {
namespace mem {

//using async_pool_alloc = static_wrapper<variable_wrapper<async_wrapper<
//    detail::fixed_alloc<
//        variable_alloc                           <sizeof(void*) * 1024 * 256>, 
//        fixed_expand_policy<sizeof(void*) * 1024, sizeof(void*) * 1024 * 256>
//    >, 
//    default_recycler >>>;
using async_pool_alloc = ipc::mem::static_alloc;

template <typename T>
using allocator = allocator_wrapper<T, async_pool_alloc>;

} // namespace mem

namespace {

constexpr char const * pf(int)                { return "%d"  ; }
constexpr char const * pf(long)               { return "%ld" ; }
constexpr char const * pf(long long)          { return "%lld"; }
constexpr char const * pf(unsigned int)       { return "%u"  ; }
constexpr char const * pf(unsigned long)      { return "%lu" ; }
constexpr char const * pf(unsigned long long) { return "%llu"; }
constexpr char const * pf(float)              { return "%f"  ; }
constexpr char const * pf(double)             { return "%f"  ; }
constexpr char const * pf(long double)        { return "%Lf" ; }

} // internal-linkage

template <typename T>
struct hash : public std::hash<T> {};

template <typename Key, typename T>
using unordered_map = std::unordered_map<
    Key, T, ipc::hash<Key>, std::equal_to<Key>, ipc::mem::allocator<std::pair<Key const, T>>
>;

template <typename Key, typename T>
using map = std::map<
    Key, T, std::less<Key>, ipc::mem::allocator<std::pair<Key const, T>>
>;

template <typename Char>
using basic_string = std::basic_string<
    Char, std::char_traits<Char>, ipc::mem::allocator<Char>
>;

using string  = basic_string<char>;
using wstring = basic_string<wchar_t>;

/* 字符串哈希特化必须与 std::equal_to(按内容)语义一致: 旧实现哈希 c_str() 指针 ⇒
 * 内容相同、地址不同的 key 落进不同桶, ipc::unordered_map 永不命中已存在条目,
 * 同一逻辑键被反复插入(chunk 池句柄缓存反复 emplace ⇒ 同一 shm 段反复 mmap,
 * 多接收者零拷贝广播地址分裂; 见 docs/uf010_evidence_registration.md)。
 * 这里按内容哈希: string_view 覆盖 [data(), data()+size()), 不拷贝、不看终止符。
 * ⛔ 不得改回指针哈希; 回归器 test/test_uf010_hash_semantics.cpp +
 * test/test_loan.cpp 的 Loan.BroadcastToMultipleReceivers。 */
template <> struct hash<string> {
    std::size_t operator()(string const &val) const noexcept {
        std::string_view sv{val.data(), val.size()};
        return std::hash<std::string_view>{}(sv);
    }
};

template <> struct hash<wstring> {
    std::size_t operator()(wstring const &val) const noexcept {
        std::wstring_view sv{val.data(), val.size()};
        return std::hash<std::wstring_view>{}(sv);
    }
};

template <typename T>
ipc::string to_string(T val) {
    char buf[std::numeric_limits<T>::digits10 + 3] {};
    if (std::snprintf(buf, sizeof(buf), pf(val), val) > 0) {
        return buf;
    }
    return {};
}

/// \brief Check string validity.
constexpr bool is_valid_string(char const *str) noexcept {
    return (str != nullptr) && (str[0] != '\0');
}

/// \brief Make a valid string.
inline ipc::string make_string(char const *str) {
    return is_valid_string(str) ? ipc::string{str} : ipc::string{};
}

/// \brief Combine prefix from a list of strings.
inline ipc::string make_prefix(ipc::string prefix, std::initializer_list<ipc::string> args) {
    prefix += "__IPC_SHM__";
    for (auto const &txt: args) {
        if (txt.empty()) continue;
        prefix += txt;
    }
    return prefix;
}

// loan 和普通 send 使用完全相同的尺寸档，接收与 sniffer 也按本规则定位。
inline std::size_t pool_size_class(std::size_t size) noexcept {
    if (size <= 64 * 1024) return ((size + large_msg_align - 1) / large_msg_align) * large_msg_align;
    std::size_t value = 128 * 1024;
    while (value < size) {
        const auto next = value << 1;
        if (next < value) return size;
        value = next;
    }
    return value;
}

// 每个上下文持有一个跨进程租约；池缓存随最后一个本地持有者释放。
struct pool_identity_header {
    std::uint64_t magic;
    std::uint64_t topic_id;
};
// 独立控制段；chunk_info_t 和十块载荷段的既有布局保持不变。
// 除 waiters/sequence 外，所有字段均由载荷段的池锁保护。
struct pool_credit_state {
    std::uint64_t magic = 0, topic_id = 0, stride = 0, generation = 0;
    std::uint32_t capacity = topic_msg_cache, free = topic_msg_cache;
    std::uint32_t reserve = 1, high_watermark = 0;
    std::atomic<std::uint32_t> sequence{0}, waiters{0};
    std::uint64_t next_ticket = 0;
    std::array<std::uint64_t, topic_msg_cache> tickets{};
    std::array<bool, topic_msg_cache> active{};
    std::uint64_t loan_attempt = 0, loan_success = 0, loan_reject = 0;
    std::uint64_t duplicate_return = 0, invalid_storage_id = 0, pool_chain_corrupt = 0;
    std::uint64_t wait_ns = 0, wait_max_ns = 0;
    std::array<std::uint64_t, 64> wait_histogram{};
};
class topic_pool_context {
    struct impl;
    std::unique_ptr<impl> p_;
public:
    topic_pool_context(ipc::string const& pref, ipc::string const& name);
    ~topic_pool_context();
    bool valid() const noexcept;
    bool same_process() const noexcept;
    std::uint64_t identity() const noexcept;
    void* map_pool(std::size_t stride, std::size_t bytes);
    pool_credit_state* map_credit(std::size_t stride);
};
IPC_EXPORT ipc::string topic_pool_prefix(ipc::string const& pref, ipc::string const& name);
IPC_EXPORT std::shared_ptr<topic_pool_context> acquire_topic_pool(ipc::string const& pref, ipc::string const& name);
IPC_EXPORT bool clear_topic_pools(ipc::string const& pref, ipc::string const& name) noexcept;

} // namespace ipc
