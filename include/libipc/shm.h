#pragma once

#include <cstddef>
#include <cstdint>

#include "libipc/export.h"

namespace ipc {
namespace shm {

using id_t = void*;

enum : unsigned {
    create = 0x01,
    open   = 0x02
};

IPC_EXPORT id_t         acquire(char const * name, std::size_t size, unsigned mode = create | open);
IPC_EXPORT void *       get_mem(id_t id, std::size_t * size);
IPC_EXPORT std::int32_t release(id_t id) noexcept;
IPC_EXPORT std::int32_t release_no_unlink(id_t id) noexcept;
IPC_EXPORT void         remove (id_t id) noexcept;
IPC_EXPORT void         remove (char const * name) noexcept;
/* UF-004 保留面收口: 名字级 unlink 本进程以 create 模式创建(已登记)的段。
 * 供不展开栈的退出路径(如 dzipc 超时收尾)使用 —— 不触碰映射/实例;
 * 已被 release/remove unlink 过的名字不会再扫; fork 继承的登记项(pid 不符)不误扫。 */
IPC_EXPORT std::size_t unlink_created_segments() noexcept;

IPC_EXPORT std::int32_t get_ref(id_t id);
IPC_EXPORT void sub_ref(id_t id);

class IPC_EXPORT handle {
public:
    handle();
    handle(char const * name, std::size_t size, unsigned mode = create | open);
    handle(handle&& rhs);

    ~handle();

    void swap(handle& rhs);
    handle& operator=(handle rhs);

    bool         valid() const noexcept;
    std::size_t  size () const noexcept;
    char const * name () const noexcept;

    std::int32_t ref() const noexcept;
    void sub_ref() noexcept;

    bool acquire(char const * name, std::size_t size, unsigned mode = create | open);
    std::int32_t release();
    std::int32_t release_no_unlink();

    // Clean the handle file.
    void clear() noexcept;
    static void clear_storage(char const * name) noexcept;

    void* get() const;

    void attach(id_t);
    id_t detach();

private:
    class handle_;
    handle_* p_;
};

} // namespace shm
} // namespace ipc
