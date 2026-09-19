
#include "libipc/semaphore.h"

#include "libipc/utility/pimpl.h"
#include "libipc/utility/log.h"
#include "libipc/memory/resource.h"
#include "libipc/platform/detail.h"
#if defined(IPC_OS_WINDOWS_)
#include "libipc/platform/win/semaphore.h"
#elif defined(IPC_OS_LINUX_) || defined(IPC_OS_QNX_)
#include "libipc/platform/posix/semaphore_impl.h"
#else /*IPC_OS*/
#error "Unsupported platform."
#endif

namespace ipc
{
    namespace sync
    {

        class semaphore::semaphore_ : public ipc::pimpl<semaphore_>
        {
        public:
            ipc::detail::sync::semaphore sem_;
        };

        semaphore::semaphore()
            : p_(p_->make())
        {
        }

        semaphore::semaphore(char const *name, std::uint32_t count)
            : semaphore()
        {
            open(name, count);
        }

        semaphore::~semaphore()
        {
            close();
            /* 析构的第二处解引用: `close()` 里已挡了失效态, 这一句同样要挡 ——
             * `p_->clear()` 在 `p_ == nullptr` 时是成员访问式的形式 UB。 */
            auto ip = impl(p_);
            if (ip != nullptr) ip->clear();
        }

        /* ⛔ UF-002: `p_ == nullptr` 是失效态(`pimpl<semaphore_>` 走"不舒服"分支 ⇒ impl
         * 在堆上, `mem::alloc<semaphore_>` 失败时返回 nullptr 而不抛, 构造函数不检查)。
         * 旧实现在 `~semaphore()` 的第一句 `close()` 就解引用空指针。收口见
         * docs/unfixed_defects.md §2「修法选项 1」: 入口判空 + 失效态空转。 */
        void const *semaphore::native() const noexcept
        {
            auto ip = impl(p_);
            return (ip == nullptr) ? nullptr : ip->sem_.native();
        }

        void *semaphore::native() noexcept
        {
            auto ip = impl(p_);
            return (ip == nullptr) ? nullptr : ip->sem_.native();
        }

        bool semaphore::valid() const noexcept
        {
            auto ip = impl(p_);
            return (ip != nullptr) && ip->sem_.valid();
        }

        bool semaphore::open(char const *name, std::uint32_t count) noexcept
        {
            if (!is_valid_string(name))
            {
                ipc::error("fail semaphore open: name is empty\n");
                return false;
            }
            auto ip = impl(p_);
            if (ip == nullptr)
            {
                ipc::error("fail semaphore open: semaphore is in invalid state (pimpl alloc failed)\n");
                return false;
            }
            return ip->sem_.open(name, count);
        }

        void semaphore::close() noexcept
        {
            auto ip = impl(p_);
            if (ip == nullptr) return;
            ip->sem_.close();
        }

        void semaphore::clear() noexcept
        {
            auto ip = impl(p_);
            if (ip == nullptr) return;
            ip->sem_.clear();
        }

        void semaphore::clear_storage(char const *name) noexcept
        {
            ipc::detail::sync::semaphore::clear_storage(name);
        }

        bool semaphore::try_wait() noexcept
        {
            auto ip = impl(p_);
            return (ip == nullptr) ? false : ip->sem_.try_wait();
        }

        bool semaphore::wait(std::uint64_t tm) noexcept
        {
            auto ip = impl(p_);
            return (ip == nullptr) ? false : ip->sem_.wait(tm);
        }

        bool semaphore::post(std::uint32_t count) noexcept
        {
            auto ip = impl(p_);
            return (ip == nullptr) ? false : ip->sem_.post(count);
        }

    } // namespace sync
} // namespace ipc
