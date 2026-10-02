#include "libipc/pool_alloc.h"
#include "libipc/memory/resource.h"
#include "libipc/shm.h"
#include "libipc/waiter.h"
#include "libipc/utility/log.h"
#include "dzIPC/common/hash.h"
#include <mutex>
#include <map>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

namespace ipc {
namespace mem {
void* pool_alloc::alloc(std::size_t size) noexcept { return async_pool_alloc::alloc(size); }
void pool_alloc::free(void* p, std::size_t size) noexcept { async_pool_alloc::free(p, size); }
}

namespace {
constexpr std::uint64_t registry_magic = 0x445A504F4F4C0002ULL;
constexpr std::uint64_t payload_magic = 0x445A4348554E0002ULL;
// 注册项只存身份，不存载荷。完整名字用于验证哈希碰撞，禁止误删或串池。
struct registry_record {
    std::uint64_t magic, identity;
    std::uint32_t prefix_size, name_size;
    char text[1024];
};
std::string system_name(ipc::string const& n) { return "/" + std::string(n.data(),n.size()); }
std::uint64_t name_id(ipc::string const& n) {
    return dzIPC::common::fnv1a64(std::string(n.data(),n.size()));
}
ipc::string registry_name(ipc::string const& p,ipc::string const& n) {
    return topic_pool_prefix(p,n)+"REG";
}
// 固定锁文件不能 unlink，否则等待旧 inode 的创建者可能与新锁并行。
// 该文件为零字节；只保护创建/清理，不进入逐消息热路径。
struct catalog_guard {
    int fd=-1;
    catalog_guard() {
        fd=::shm_open("/__IPC_POOL_CATALOG_V2",O_RDWR|O_CREAT|O_CLOEXEC,0600);
        if(fd>=0) {
            int rc;do {rc=::flock(fd,LOCK_EX);}while(rc<0 && errno==EINTR);
            if(rc<0){::close(fd);fd=-1;}
        }
    }
    ~catalog_guard(){if(fd>=0)::close(fd);}
    explicit operator bool()const{return fd>=0;}
};
bool matches(registry_record const& r,ipc::string const& p,ipc::string const& n) {
    return r.magic==registry_magic && r.identity==name_id(n) &&
        r.prefix_size==p.size() && r.name_size==n.size() &&
        p.size()+n.size()<=sizeof(r.text) &&
        std::memcmp(r.text,p.data(),p.size())==0 &&
        std::memcmp(r.text+p.size(),n.data(),n.size())==0;
}
bool read_record(int fd,registry_record& r) {
    return ::pread(fd,&r,sizeof(r),0)==static_cast<ssize_t>(sizeof(r));
}
void remove_channel_storage(ipc::string const& p, ipc::string const& n) {
    // 无任何租约时，连同崩溃遗留的 writer 标志/等待资源一起清理。
    // 活跃租约（包括晚释放 Sample）存在时绝不走此路径。
    for (const auto* kind : {"CC_CONN__", "WT_CONN__", "RD_CONN__"})
        ipc::detail::waiter::clear_storage(ipc::make_prefix(p,{kind,n}).c_str());
    ipc::shm::handle::clear_storage(ipc::make_prefix(p,{"AC_CONN__",n}).c_str());
    ipc::shm::handle::clear_storage(ipc::make_prefix(p,{"QU_CONN__",n,"__",
        ipc::to_string(static_cast<std::size_t>(ipc::data_length)),"__",
        ipc::to_string((ipc::detail::min)(static_cast<std::size_t>(ipc::data_length),alignof(std::max_align_t))),"__V4"}).c_str());
}
bool remove_pools(ipc::string const& prefix) {
    DIR* dir=::opendir("/dev/shm");
    if(!dir)return false;
    const auto lead=prefix+"CHUNK_INFO__";
    bool okay=true;
    while(auto* e=::readdir(dir)) {
        if(std::strncmp(e->d_name,lead.c_str(),lead.size())!=0)continue;
        const std::string n="/"+std::string(e->d_name);
        if(::shm_unlink(n.c_str())!=0 && errno!=ENOENT)okay=false;
    }
    ::closedir(dir);return okay;
}
}

ipc::string topic_pool_prefix(ipc::string const& pref,ipc::string const& name) {
    // 固定长度身份键解除原长话题名追加尺寸后缀时的长度回退。
    return make_prefix({},{"TOPIC_POOL_V2__",to_string(name_id(name)),"_",to_string(name_id(pref)),"__"});
}

struct topic_pool_context::impl {
    ipc::string prefix,name,pool_prefix,registry;
    int fd=-1;
    pid_t pid=::getpid();
    std::uint64_t id=0;
    std::mutex mutex;
    std::map<std::size_t,ipc::shm::handle> pools;

    impl(ipc::string const& p,ipc::string const& n)
        :prefix(p),name(n),pool_prefix(topic_pool_prefix(p,n)),registry(registry_name(p,n)),id(name_id(n)) {
        if(p.size()+n.size()>sizeof(registry_record::text))return;
        catalog_guard guard;if(!guard)return;
        const auto sys=system_name(registry);
        int file=::shm_open(sys.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
        if(file<0)return;
        registry_record old{};
        const bool readable=read_record(file,old);
        struct stat st{};
        if(::fstat(file,&st)!=0){::close(file);return;}
        // 包括无法验证的非空残段：不覆盖、不删除，明确失败。
        if(st.st_size!=0 && (!readable || !matches(old,p,n))) {
            ipc::error("topic pool identity mismatch: %s\n",registry.c_str());
            ::close(file);return;
        }
        const bool alone=::flock(file,LOCK_EX|LOCK_NB)==0;
        if(!alone && errno!=EWOULDBLOCK && errno!=EAGAIN){::close(file);return;}
        if(alone) {
            // 没有活跃 route / Sample / 未发布借样；崩溃遗留也可在此整体清理。
            if(!remove_pools(pool_prefix)){::close(file);return;}
            remove_channel_storage(prefix,name);
            registry_record record{};
            record.magic=registry_magic;record.identity=id;
            record.prefix_size=p.size();record.name_size=n.size();
            std::memcpy(record.text,p.data(),p.size());
            std::memcpy(record.text+p.size(),n.data(),n.size());
            if(::ftruncate(file,sizeof(record))!=0 ||
               ::pwrite(file,&record,sizeof(record),0)!=static_cast<ssize_t>(sizeof(record))) {
                ::close(file);return;
            }
        } else if(!readable) {::close(file);return;}
        // 转共享锁仍在 catalog 锁内；不能与另一创建者/清理者产生空窗。
        if(::flock(file,LOCK_SH|LOCK_NB)!=0){::close(file);return;}
        fd=file;
    }
    ~impl() {
        // 不让 ipc::shm 的普通引用计数自动 unlink；生杀由跨进程租约决定。
        for(auto& item:pools)item.second.release_no_unlink();
        pools.clear();
        if(fd<0)return;
        if(pid!=::getpid()){::close(fd);return;} // fork 继承的 OFD 不主动解锁父进程
        catalog_guard guard;
        if(guard && ::flock(fd,LOCK_EX|LOCK_NB)==0) {
            if(remove_pools(pool_prefix)) {
                remove_channel_storage(prefix,name);
                ::shm_unlink(system_name(registry).c_str());
            }
        }
        ::close(fd);
    }
};
topic_pool_context::topic_pool_context(ipc::string const& p,ipc::string const& n):p_(new impl(p,n)){}
topic_pool_context::~topic_pool_context()=default;
bool topic_pool_context::valid()const noexcept{return p_->fd>=0;}
bool topic_pool_context::same_process()const noexcept{return p_->pid==::getpid();}
std::uint64_t topic_pool_context::identity()const noexcept{return p_->id;}

void* topic_pool_context::map_pool(std::size_t stride,std::size_t bytes) {
    if(!valid() || !same_process() || bytes<sizeof(pool_identity_header))return nullptr;
    std::lock_guard<std::mutex> local(p_->mutex);
    auto found=p_->pools.find(stride);
    if(found!=p_->pools.end())return found->second.get();
    catalog_guard guard;if(!guard)return nullptr;
    const auto name=p_->pool_prefix+"CHUNK_INFO__"+to_string(stride)+"__C10";
    const auto sys=system_name(name);
    int file=::shm_open(sys.c_str(),O_RDWR|O_CLOEXEC,0600);
    bool existed=file>=0;
    if(existed) {
        struct stat st{};
        const bool okay=::fstat(file,&st)==0 && static_cast<std::size_t>(st.st_size)==bytes+sizeof(std::int32_t);
        ::close(file);
        if(!okay)return nullptr;
    } else if(errno!=ENOENT)return nullptr;
    ipc::shm::handle h;
    if(!h.acquire(name.c_str(),bytes))return nullptr;
    auto* head=static_cast<pool_identity_header*>(h.get());
    if(!existed){head->magic=payload_magic;head->topic_id=p_->id;}
    if(head->magic!=payload_magic || head->topic_id!=p_->id) {
        h.release_no_unlink();return nullptr;
    }
    auto* result=h.get();
    p_->pools.emplace(stride,std::move(h));
    return result;
}

std::shared_ptr<topic_pool_context> acquire_topic_pool(ipc::string const& pref,ipc::string const& name) {
    // weak_ptr 不延长池寿命；每次查询删去过期项，避免话题 churn 累积索引。
    static std::mutex mutex;
    static std::map<ipc::string,std::weak_ptr<topic_pool_context>> contexts;
    std::lock_guard<std::mutex> guard(mutex);
    for(auto i=contexts.begin();i!=contexts.end();) {
        if(i->second.expired())i=contexts.erase(i);else ++i;
    }
    auto key=make_prefix({}, {to_string(pref.size()),"_",pref,name});
    auto& weak=contexts[key];
    if(auto current=weak.lock())if(current->same_process())return current;
    auto created=std::make_shared<topic_pool_context>(pref,name);
    if(!created->valid()){contexts.erase(key);return {};}
    weak=created;return created;
}

bool clear_topic_pools(ipc::string const& pref,ipc::string const& name) noexcept {
    try {
        catalog_guard guard;if(!guard)return false;
        const auto registry=registry_name(pref,name);
        const auto sys=system_name(registry);
        int fd=::shm_open(sys.c_str(),O_RDWR|O_CLOEXEC,0600);
        if(fd<0) {
            if(errno!=ENOENT)return false;
            const bool okay=remove_pools(topic_pool_prefix(pref,name));
            if(okay)remove_channel_storage(pref,name);
            return okay;
        }
        // 活跃池原样复用；不得解除别人的租约、重置空闲链或删除任何池。
        if(::flock(fd,LOCK_EX|LOCK_NB)!=0){::close(fd);return false;}
        registry_record r{};
        const bool okay=read_record(fd,r) && matches(r,pref,name) && remove_pools(topic_pool_prefix(pref,name));
        if(okay) {remove_channel_storage(pref,name);::shm_unlink(sys.c_str());}
        ::close(fd);return okay;
    } catch(...) {return false;}
}
} // namespace ipc
