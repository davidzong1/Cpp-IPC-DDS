
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <cstring>
#include <vector>

#include "libipc/shm.h"
#include "libipc/def.h"
#include "libipc/pool_alloc.h"

#include "libipc/utility/log.h"
#include "libipc/memory/resource.h"

namespace {

struct info_t {
    std::atomic<std::int32_t> acc_;
};

struct id_info_t {
    int         fd_   = -1;
    void*       mem_  = nullptr;
    std::size_t size_ = 0;
    ipc::string name_;
};

constexpr std::size_t calc_size(std::size_t size) {
    return ((((size - 1) / alignof(info_t)) + 1) * alignof(info_t)) + sizeof(info_t);
}

inline auto& acc_of(void* mem, std::size_t size) {
    return reinterpret_cast<info_t*>(static_cast<ipc::byte_t*>(mem) + size - sizeof(info_t))->acc_;
}

/* ---- UF-004 保留面收口(2026-09-20): 本进程创建的段名名单 ----
 * acquire() 以 create 模式(O_CREAT|O_EXCL)成功即登记段名;
 * unlink_created_segments() 对本进程条目做名字级扫除 —— 供不展开栈的退出路径
 * (dzipc 超时/RequestShutdown 收尾)回收段名, 不触碰映射与实例。
 * 条目记录创建时的 pid, fork 继承的条目(pid 不符)不会被误扫;
 * release/remove unlink 段名时摘除条目, 避免易主后对同名再扫。 */
std::mutex& created_registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::pair<::pid_t, ipc::string>>& created_registry() {
    static std::vector<std::pair<::pid_t, ipc::string>> v;
    return v;
}

void record_created(ipc::string const & name) {
    ::pid_t const self = ::getpid();
    std::lock_guard<std::mutex> guard(created_registry_mutex());
    auto & v = created_registry();
    for (auto const & e : v) {
        if ((e.first == self) && (e.second == name)) return;
    }
    v.emplace_back(self, name);
}

void unrecord_created(ipc::string const & name) noexcept {
    ::pid_t const self = ::getpid();
    std::lock_guard<std::mutex> guard(created_registry_mutex());
    auto & v = created_registry();
    v.erase(std::remove_if(v.begin(), v.end(),
                [&](std::pair<::pid_t, ipc::string> const & e) {
                    return (e.first == self) && (e.second == name);
                }),
            v.end());
}

} // internal-linkage

namespace ipc {
namespace shm {

ipc::string object_name(char const *name) {
    return (name != nullptr && name[0] == '/') ? ipc::string{name} : ipc::string{"/"} + name;
}

id_t acquire(char const * name, std::size_t size, unsigned mode) {
    if (!is_valid_string(name)) {
        ipc::error("fail acquire: name is empty\n");
        return nullptr;
    }
    // For portable use, a shared memory object should be identified by name of the form /somename.
    // see: https://man7.org/linux/man-pages/man3/shm_open.3.html
    ipc::string op_name = object_name(name);
    // Open the object for read-write access.
    // Open the object for read-write access.
    int fd = -1;
    bool created_here = false;
    if (mode == open) {
        size = 0;
        fd = ::shm_open(op_name.c_str(), O_RDWR, 0);
    }
    else {
        // 创建者判定必须原子: O_CREAT|O_EXCL 只有创建者成功;
        // create 模式保持"存在即失败"; default(create|open) 对 EEXIST 回落 attach。
        int const excl_flag = O_RDWR | O_CREAT | O_EXCL;
        fd = ::shm_open(op_name.c_str(), excl_flag,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        if (fd != -1) {
            created_here = true;
        }
        else if ((mode != create) && (errno == EEXIST)) {
            // default: 已存在 -> attach(本进程非创建者)。
            fd = ::shm_open(op_name.c_str(), O_RDWR, 0);
            if ((fd == -1) && (errno == ENOENT)) {
                // 探测与 attach 之间被 unlink 的竞态: 按原 O_CREAT 语义重试。
                fd = ::shm_open(op_name.c_str(), O_RDWR | O_CREAT,
                                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
                if (fd != -1) created_here = true;
            }
        }
    }
    if (fd == -1) {
        // only open shm not log error when file not exist
        if (open != mode || ENOENT != errno) {
            ipc::error("fail shm_open[%d]: %s\n", errno, op_name.c_str());
        }
        return nullptr;
    }
    ::fchmod(fd, S_IRUSR | S_IWUSR | 
                 S_IRGRP | S_IWGRP | 
                 S_IROTH | S_IWOTH);
    auto ii = mem::alloc<id_info_t>();
    ii->fd_   = fd;
    ii->size_ = size;
    ii->name_ = std::move(op_name);
    if (created_here) {
        record_created(ii->name_);
    }
    return ii;
}

std::int32_t get_ref(id_t id) {
    if (id == nullptr) {
        return 0;
    }
    auto ii = static_cast<id_info_t*>(id);
    if (ii->mem_ == nullptr || ii->size_ == 0) {
        return 0;
    }
    return acc_of(ii->mem_, ii->size_).load(std::memory_order_acquire);
}

void sub_ref(id_t id) {
    if (id == nullptr) {
        ipc::error("fail sub_ref: invalid id (null)\n");
        return;
    }
    auto ii = static_cast<id_info_t*>(id);
    if (ii->mem_ == nullptr || ii->size_ == 0) {
        ipc::error("fail sub_ref: invalid id (mem = %p, size = %zd)\n", ii->mem_, ii->size_);
        return;
    }
    acc_of(ii->mem_, ii->size_).fetch_sub(1, std::memory_order_acq_rel);
}

void * get_mem(id_t id, std::size_t * size) {
    if (id == nullptr) {
        ipc::error("fail get_mem: invalid id (null)\n");
        return nullptr;
    }
    auto ii = static_cast<id_info_t*>(id);
    if (ii->mem_ != nullptr) {
        if (size != nullptr) *size = ii->size_;
        return ii->mem_;
    }
    int fd = ii->fd_;
    if (fd == -1) {
        ipc::error("fail get_mem: invalid id (fd = -1)\n");
        return nullptr;
    }
    if (ii->size_ == 0) {
        struct stat st;
        if (::fstat(fd, &st) != 0) {
            ipc::error("fail fstat[%d]: %s, size = %zd\n", errno, ii->name_.c_str(), ii->size_);
            return nullptr;
        }
        ii->size_ = static_cast<std::size_t>(st.st_size);
        if ((ii->size_ <= sizeof(info_t)) || (ii->size_ % sizeof(info_t))) {
            ipc::error("fail get_mem: %s, invalid size = %zd\n", ii->name_.c_str(), ii->size_);
            return nullptr;
        }
    }
    else {
        ii->size_ = calc_size(ii->size_);
        if (::ftruncate(fd, static_cast<off_t>(ii->size_)) != 0) {
            ipc::error("fail ftruncate[%d]: %s, size = %zd\n", errno, ii->name_.c_str(), ii->size_);
            return nullptr;
        }
    }
    void* mem = ::mmap(nullptr, ii->size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        ipc::error("fail mmap[%d]: %s, size = %zd\n", errno, ii->name_.c_str(), ii->size_);
        return nullptr;
    }
    ::close(fd);
    ii->fd_  = -1;
    ii->mem_ = mem;
    if (size != nullptr) *size = ii->size_;
    acc_of(mem, ii->size_).fetch_add(1, std::memory_order_release);
    return mem;
}

std::int32_t release(id_t id) noexcept {
    if (id == nullptr) {
        ipc::error("fail release: invalid id (null)\n");
        return -1;
    }
    std::int32_t ret = -1;
    auto ii = static_cast<id_info_t*>(id);
    if (ii->mem_ == nullptr || ii->size_ == 0) {
        ipc::error("fail release: invalid id (mem = %p, size = %zd), name = %s\n",
                    ii->mem_, ii->size_, ii->name_.c_str());
    }
    else if ((ret = acc_of(ii->mem_, ii->size_).fetch_sub(1, std::memory_order_acq_rel)) <= 1) {
        ::munmap(ii->mem_, ii->size_);
        if (!ii->name_.empty()) {
            ::shm_unlink(ii->name_.c_str());
        }
            unrecord_created(ii->name_);
    }
    else ::munmap(ii->mem_, ii->size_);
    mem::free(ii);
    return ret;
}

std::int32_t release_no_unlink(id_t id) noexcept {
    if (id == nullptr) {
        ipc::error("fail release_no_unlink: invalid id (null)\n");
        return -1;
    }
    auto ii = static_cast<id_info_t*>(id);
    if (ii->mem_ == nullptr || ii->size_ == 0) {
        ipc::error("fail release_no_unlink: invalid id (mem = %p, size = %zd), name = %s\n",
                    ii->mem_, ii->size_, ii->name_.c_str());
    }
    else {
        acc_of(ii->mem_, ii->size_).fetch_sub(1, std::memory_order_acq_rel);
        ::munmap(ii->mem_, ii->size_);
    }
    mem::free(ii);
    return 0;
}

void remove(id_t id) noexcept {
    if (id == nullptr) {
        ipc::error("fail remove: invalid id (null)\n");
        return;
    }
    auto ii = static_cast<id_info_t*>(id);
    auto name = std::move(ii->name_);
    release(id);
    if (!name.empty()) {
        ::shm_unlink(name.c_str());
    }
    unrecord_created(name);
}

void remove(char const * name) noexcept {
    if (!is_valid_string(name)) {
        ipc::error("fail remove: name is empty\n");
        return;
    }
    const ipc::string op_name = object_name(name);
    ::shm_unlink(op_name.c_str());
    unrecord_created(op_name);
}

std::size_t unlink_created_segments() noexcept {
    std::vector<std::pair<::pid_t, ipc::string>> taken;
    {
        std::lock_guard<std::mutex> guard(created_registry_mutex());
        taken.swap(created_registry());
    }
    std::vector<std::pair<::pid_t, ipc::string>> keep;
    std::size_t n = 0;
    ::pid_t const self = ::getpid();
    for (auto & e : taken) {
        if (e.first != self) {
            // fork 继承的父进程条目: 不属于本进程, 退回注册表。
            keep.push_back(std::move(e));
            continue;
        }
        if (::shm_unlink(e.second.c_str()) == 0) {
            ++n;
        }
        else if (errno != ENOENT) {
            ipc::error("fail unlink_created_segments[%d]: %s\n", errno, e.second.c_str());
        }
    }
    if (!keep.empty()) {
        std::lock_guard<std::mutex> guard(created_registry_mutex());
        auto & v = created_registry();
        v.insert(v.end(), keep.begin(), keep.end());
    }
    return n;
}

} // namespace shm
} // namespace ipc
