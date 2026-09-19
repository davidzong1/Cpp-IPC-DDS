
#include "libipc/condition.h"

#include "libipc/utility/pimpl.h"
#include "libipc/utility/log.h"
#include "libipc/memory/resource.h"
#include "libipc/platform/detail.h"
#if defined(IPC_OS_WINDOWS_)
#include "libipc/platform/win/condition.h"
#elif defined(IPC_OS_LINUX_)
#include "libipc/platform/linux/condition.h"
#elif defined(IPC_OS_QNX_)
#include "libipc/platform/posix/condition.h"
#else/*IPC_OS*/
#   error "Unsupported platform."
#endif

namespace ipc {
namespace sync {

class condition::condition_ : public ipc::pimpl<condition_> {
public:
    ipc::detail::sync::condition cond_;
};

condition::condition()
    : p_(p_->make()) {
}

condition::condition(char const * name)
    : condition() {
    open(name);
}

condition::~condition() {
    close();
    /* 析构的第二处解引用: `close()` 里已挡了失效态, 这一句同样要挡 ——
     * `p_->clear()` 在 `p_ == nullptr` 时是成员访问式的形式 UB。 */
    auto ip = impl(p_);
    if (ip != nullptr) ip->clear();
}

/* ⛔ UF-002: `p_ == nullptr` 是失效态(`pimpl<condition_>` 走"不舒服"分支 ⇒ impl 在堆上,
 * `mem::alloc<condition_>` 失败时返回 nullptr 而不抛, 构造函数不检查)。旧实现在
 * `~condition()` 的第一句 `close()` 就解引用空指针。收口见 docs/unfixed_defects.md
 * §2「修法选项 1」: 入口判空 + 失效态空转, 不动全局错误模型。 */
void const *condition::native() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->cond_.native();
}

void *condition::native() noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->cond_.native();
}

bool condition::valid() const noexcept {
    auto ip = impl(p_);
    return (ip != nullptr) && ip->cond_.valid();
}

bool condition::open(char const *name) noexcept {
    if (!is_valid_string(name)) {
        ipc::error("fail condition open: name is empty\n");
        return false;
    }
    auto ip = impl(p_);
    if (ip == nullptr) {
        ipc::error("fail condition open: condition is in invalid state (pimpl alloc failed)\n");
        return false;
    }
    return ip->cond_.open(name);
}

void condition::close() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr) return;
    ip->cond_.close();
}

void condition::clear() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr) return;
    ip->cond_.clear();
}

void condition::clear_storage(char const * name) noexcept {
    ipc::detail::sync::condition::clear_storage(name);
}

bool condition::wait(ipc::sync::mutex &mtx, std::uint64_t tm) noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->cond_.wait(mtx, tm);
}

bool condition::notify(ipc::sync::mutex &mtx) noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->cond_.notify(mtx);
}

bool condition::broadcast(ipc::sync::mutex &mtx) noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->cond_.broadcast(mtx);
}

} // namespace sync
} // namespace ipc
