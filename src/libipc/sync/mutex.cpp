
#include "libipc/mutex.h"

#include "libipc/utility/pimpl.h"
#include "libipc/utility/log.h"
#include "libipc/memory/resource.h"
#include "libipc/platform/detail.h"
#if defined(IPC_OS_WINDOWS_)
#include "libipc/platform/win/mutex.h"
#elif defined(IPC_OS_LINUX_)
#include "libipc/platform/linux/mutex.h"
#elif defined(IPC_OS_QNX_)
#include "libipc/platform/posix/mutex.h"
#else/*IPC_OS*/
#   error "Unsupported platform."
#endif

namespace ipc {
namespace sync {

class mutex::mutex_ : public ipc::pimpl<mutex_> {
public:
    ipc::detail::sync::mutex lock_;
};

mutex::mutex()
    : p_(p_->make()) {
}

mutex::mutex(char const * name)
    : mutex() {
    open(name);
}

mutex::~mutex() {
    close();
    /* 析构的第二处解引用: `close()` 里已挡了失效态, 这一句同样要挡 ——
     * `p_->clear()` 在 `p_ == nullptr` 时是成员访问式的形式 UB。 */
    auto ip = impl(p_);
    if (ip != nullptr) ip->clear();
}

/* ⛔ UF-002: `p_ == nullptr` 是失效态(`pimpl<mutex_>` 走"不舒服"分支 ⇒ impl 在堆上,
 * `mem::alloc<mutex_>` 失败时返回 nullptr 而不抛`include/libipc/pool_alloc.h:87-97`,
 * 构造函数不检查)。旧实现每一处都直接解引用, `~mutex()` 第一句 `close()` 就是崩点。
 * 收口方式: docs/unfixed_defects.md §2「修法选项 1」—— 入口判空 + 失效态空转
 * (`valid()` false / 取原生句柄得 nullptr / 加解锁返回 false), 不动全局错误模型。 */
void const *mutex::native() const noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->lock_.native();
}

void *mutex::native() noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? nullptr : ip->lock_.native();
}

bool mutex::valid() const noexcept {
    auto ip = impl(p_);
    return (ip != nullptr) && ip->lock_.valid();
}

bool mutex::open(char const *name) noexcept {
    if (!is_valid_string(name)) {
        ipc::error("fail mutex open: name is empty\n");
        return false;
    }
    auto ip = impl(p_);
    if (ip == nullptr) {
        /* 要报出来: "分配失败 ⇒ 永久失效"与"参数不对"在调用方看都是 false */
        ipc::error("fail mutex open: mutex is in invalid state (pimpl alloc failed)\n");
        return false;
    }
    return ip->lock_.open(name);
}

void mutex::close() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr) return;
    ip->lock_.close();
}

void mutex::clear() noexcept {
    auto ip = impl(p_);
    if (ip == nullptr) return;
    ip->lock_.clear();
}

void mutex::clear_storage(char const * name) noexcept {
    ipc::detail::sync::mutex::clear_storage(name);
}

bool mutex::lock(std::uint64_t tm) noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->lock_.lock(tm);
}

bool mutex::try_lock() noexcept(false) {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->lock_.try_lock();
}

bool mutex::unlock() noexcept {
    auto ip = impl(p_);
    return (ip == nullptr) ? false : ip->lock_.unlock();
}

} // namespace sync
} // namespace ipc
