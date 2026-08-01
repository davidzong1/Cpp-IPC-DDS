#include "dzIPC/common/local_pub_sub_registry.h"
#include <mutex>

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
namespace dzIPC {

LocalPubSubRegistry& LocalPubSubRegistry::instance()
{
    /*
     * Intentionally leaked pointer — never destroyed.
     *
     * A function-local static object (Meyer's singleton) would be
     * destroyed during the C++ static destruction phase, whose order
     * relative to global/static shm_sub_ipc destructors is undefined.
     * A shm_sub_ipc dtor calls unregister_subscriber(), which must
     * access the still-live registry.  Leaking the singleton guarantees
     * that the registry outlives every subscriber destructor; the OS
     * reclaims the memory on process exit.
     */
    static LocalPubSubRegistry* inst = new LocalPubSubRegistry();
    return *inst;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LocalPubSubRegistry::register_subscriber(const ChannelKey& key,
                                               std::weak_ptr<CircularQueue<IpcMsgBase>> queue)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);
    auto& vec = registry_[key];

    /* Dedup: skip if the same queue is already registered */
    auto new_sp = queue.lock();
    if (!new_sp)
    {
        return;   // already expired, nothing to register
    }
    for (auto& wq : vec)
    {
        auto existing = wq.lock();
        if (existing && existing.get() == new_sp.get())
        {
            return;   // duplicate
        }
    }
    vec.push_back(std::move(queue));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LocalPubSubRegistry::unregister_subscriber(const ChannelKey& key,
                                                 std::weak_ptr<CircularQueue<IpcMsgBase>> queue)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);
    auto it = registry_.find(key);
    if (it == registry_.end())
    {
        return;
    }

    auto target = queue.lock();
    auto& vec = it->second;

    for (auto w_it = vec.begin(); w_it != vec.end();)
    {
        auto existing = w_it->lock();
        if (!existing)
        {
            /* expired, remove it */
            w_it = vec.erase(w_it);
            continue;
        }
        if (target && existing.get() == target.get())
        {
            w_it = vec.erase(w_it);
            /* keep going in case of duplicates (shouldn't happen but be safe) */
            continue;
        }
        ++w_it;
    }

    if (vec.empty())
    {
        registry_.erase(it);
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::vector<std::shared_ptr<CircularQueue<IpcMsgBase>>> LocalPubSubRegistry::subscriber_snapshot(
    const ChannelKey& key)
{
    std::vector<std::shared_ptr<CircularQueue<IpcMsgBase>>> result;
    bool need_cleanup = false;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = registry_.find(key);
        if (it == registry_.end())
        {
            return result;
        }

        for (auto& wq : it->second)
        {
            auto sp = wq.lock();
            if (sp)
            {
                result.push_back(std::move(sp));
            }
            else
            {
                need_cleanup = true;
            }
        }
    }

    /* Lazy cleanup: if we saw expired entries, compact under exclusive lock */
    if (need_cleanup)
    {
        std::lock_guard<std::shared_mutex> lock(mutex_);
        auto it = registry_.find(key);
        if (it != registry_.end())
        {
            cleanup_expired(it->second);
            if (it->second.empty())
            {
                registry_.erase(it);
            }
        }
    }

    return result;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LocalPubSubRegistry::cleanup_expired(WeakQueueVec& vec)
{
    for (auto it = vec.begin(); it != vec.end();)
    {
        if (it->expired())
        {
            it = vec.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

}   // namespace dzIPC
