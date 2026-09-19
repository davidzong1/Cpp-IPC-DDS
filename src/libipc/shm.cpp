
#include <string>
#include <utility>

#include "libipc/shm.h"

#include "libipc/utility/pimpl.h"
#include "libipc/utility/log.h"
#include "libipc/memory/resource.h"

namespace ipc {
namespace shm {

class handle::handle_ : public pimpl<handle_> {
public:
    shm::id_t id_ = nullptr;
    void*     m_  = nullptr;

    ipc::string n_;
    std::size_t s_ = 0;
};

handle::handle()
    : p_(p_->make()) {
}

handle::handle(char const * name, std::size_t size, unsigned mode)
    : handle() {
    acquire(name, size, mode);
}

handle::handle(handle&& rhs)
    : handle() {
    swap(rhs);
}

handle::~handle() {
    release();
    /* 析构的第二处解引用: `release()` 里已挡了失效态, 这一句同样要挡 ——
     * `p_->clear()` 在 `p_ == nullptr` 时是成员访问式的形式 UB(不是"跑不跑得到"的问题),
     * 写法与文件内其余入口统一。 */
    auto ip = impl(p_);
    if (ip != nullptr) ip->clear();
}

void handle::swap(handle& rhs) {
    std::swap(p_, rhs.p_);
}

handle& handle::operator=(handle rhs) {
    swap(rhs);
    return *this;
}

/* ⛔ UF-002: `p_ == nullptr` 是本类的**失效态**, 不是一个不可能发生的状态。
 *
 * `pimpl<handle_>` 走的是"不舒服"分支(`sizeof(handle_) > sizeof(void*)`)⇒ impl 在堆上,
 * 由 `mem::alloc<handle_>` 取得; 该分配器**失败时返回 nullptr 而不抛**
 * (`include/libipc/pool_alloc.h:87-97`), 而构造函数 `p_(p_->make())` 不检查它
 * ⇒ `p_` 为空。此后旧实现里**每一处** `impl(p_)->...` 都是直接解引用空指针。
 *
 * 为什么"构造 + 析构"这一个最普通的动作就会崩: `~handle()` 第一句是 `release()`, 而
 * `release()` 的第一句是 `impl(p_)->id_ == nullptr` —— **先解引用 `p_` 再判 `id_`**(冻结
 * 订正那条), 判空拦不住; `valid()` 读 `impl(p_)->m_`, 地址 0x8 附近。
 *
 * 收口方式按 docs/unfixed_defects.md §2「修法选项 1」(已拍: 失效态语义, 不动全局错误模型):
 *   - `valid()` 返回 false, 其余入口安全空转(不再新增 API, 也不抛);
 *   - **先判 `p_`, 再碰 `impl(p_)->…`**(次序本身就是要修的东西, 不是"补一句 if");
 *   - 写法与已加固的样板 `src/libipc/buffer.cpp:81-99` 一致。
 * 未做: 不动全局错误模型(失效态不抛, 见 UF-002 的 ABI 裁定); 析构第二句
 * `p_->clear()` 已随本轮"补析构判空"一并挡掉。 */
bool handle::valid() const noexcept {
    auto ip = impl(p_);
    return (ip != nullptr) && (ip->m_ != nullptr);
}

std::size_t handle::size() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? 0 : ip->s_;
}

char const * handle::name() const noexcept {
    auto ip = impl(p_);
    /* 失效态没有名字可给; 返回空串与"未 acquire 的 handle"同名(handle_::n_ 本来就是空) */
    return (ip == nullptr) ? "" : ip->n_.c_str();
}

std::int32_t handle::ref() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? -1 : shm::get_ref(ip->id_);
}

void handle::sub_ref() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr) return;
    shm::sub_ref(ip->id_);
}

bool handle::acquire(char const * name, std::size_t size, unsigned mode) {
    if (!is_valid_string(name)) {
        ipc::error("fail acquire: name is empty\n");
        return false;
    }
    if (size == 0) {
        ipc::error("fail acquire: size is 0\n");
        return false;
    }
    auto ip = impl(p_);
    if (ip == nullptr) {
        /* 失效态: 这里**要报**, 不能静默返回 false —— 否则"分配失败 ⇒ 永久失效"
         * 这个根因在调用方看来和"参数不对"长得一模一样。 */
        ipc::error("fail acquire: handle is in invalid state (pimpl alloc failed)\n");
        return false;
    }
    release();
    const auto id = shm::acquire(name, size, mode);
    if (!id) {
        return false;
    }
    ip->id_ = id;
    ip->n_  = name;
    ip->m_  = shm::get_mem(ip->id_, &(ip->s_));
    return valid();
}

std::int32_t handle::release() {
    auto ip = impl(p_);
    if (ip == nullptr || ip->id_ == nullptr) return -1;
    return shm::release(detach());
}

std::int32_t handle::release_no_unlink() {
    auto ip = impl(p_);
    if (ip == nullptr || ip->id_ == nullptr) return -1;
    return shm::release_no_unlink(detach());
}

void handle::clear() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr || ip->id_ == nullptr) return;
    shm::remove(detach());
}

void handle::clear_storage(char const * name) noexcept {
    if (name == nullptr) {
        return;
    }
    shm::remove(name);
}

void* handle::get() const {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->m_;
}

void handle::attach(id_t id) {
    if (id == nullptr) return;
    auto ip = impl(p_);
    if (ip == nullptr) return;
    release();
    ip->id_ = id;
    ip->m_  = shm::get_mem(ip->id_, &(ip->s_));
}

id_t handle::detach() {
    auto ip = impl(p_);
    if (ip == nullptr) return nullptr;
    auto old = ip->id_;
    ip->id_ = nullptr;
    ip->m_  = nullptr;
    ip->s_  = 0;
    ip->n_.clear();
    return old;
}

} // namespace shm
} // namespace ipc
