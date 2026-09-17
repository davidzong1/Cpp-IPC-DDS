#include "libipc/buffer.h"
#include "libipc/utility/pimpl.h"

#include <cstring>

namespace ipc {

bool operator==(buffer const & b1, buffer const & b2) {
    return (b1.size() == b2.size()) && (std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
}

bool operator!=(buffer const & b1, buffer const & b2) {
    return !(b1 == b2);
}

class buffer::buffer_ : public pimpl<buffer_> {
public:
    void*       p_;
    std::size_t s_;
    void*       a_;
    buffer::destructor_t d_;

    buffer_(void* p, std::size_t s, buffer::destructor_t d, void* a)
        : p_(p), s_(s), a_(a), d_(d) {
    }

    ~buffer_() {
        if (d_ == nullptr) return;
        d_((a_ == nullptr) ? p_ : a_, s_);
    }
};

buffer::buffer()
    : buffer(nullptr, 0, nullptr, nullptr) {
}

buffer::buffer(void* p, std::size_t s, destructor_t d)
    : p_(p_->make(p, s, d, nullptr)) {
}

buffer::buffer(void* p, std::size_t s, destructor_t d, void* additional)
    : p_(p_->make(p, s, d, additional)) {
}

buffer::buffer(void* p, std::size_t s)
    : buffer(p, s, nullptr) {
}

buffer::buffer(char const & c)
    : buffer(const_cast<char*>(&c), 1) {
}

buffer::buffer(buffer&& rhs)
    : buffer() {
    swap(rhs);
}

buffer::~buffer() {
    p_->clear();
}

void buffer::swap(buffer& rhs) {
    std::swap(p_, rhs.p_);
}

buffer& buffer::operator=(buffer rhs) {
    swap(rhs);
    return *this;
}

/* ⛔ 下面三个访问器必须容忍 impl 为空。
 *
 * `buffer_` 走的是 pimpl 的"不舒服"分支(sizeof(T) > sizeof(void*)) ⇒ 它是
 * `mem::alloc<buffer_>()` 拿到的, **分配失败时 p_ 就是 nullptr**; 而这个分配器
 * (`static_alloc` = malloc)在失败/尺寸为 0 时返回空而不抛。旧实现直接
 * `impl(p_)->p_` 解引用空指针 —— 又一个"崩在地址 0、内核日志只有一个 ip"的入口。
 * 实测: 把 malloc 定向注成失败后, `buffer::data()`/`empty()` 都会踩到这里
 * (见 docs/shm_defect_fixes.md 第 7 条"同一族"一节)。
 * 返回空/0 与 buffer 自身的"空"语义一致, 调用方本来就要处理空 buffer
 * (例如 peer cache_t::append 就是用 data() == nullptr 当作"这条消息丢弃")。 */
bool buffer::empty() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) || (ip->p_ == nullptr) || (ip->s_ == 0);
}

void* buffer::data() noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->p_;
}

void const * buffer::data() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->p_;
}

std::size_t buffer::size() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? 0 : ip->s_;
}

} // namespace ipc
