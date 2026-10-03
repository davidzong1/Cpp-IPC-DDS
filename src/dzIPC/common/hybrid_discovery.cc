#include "dzIPC/common/hybrid_discovery.h"
#include "dzIPC/common/channel_scope.h"
#include "dzIPC/ipc_info_pool.h"
#include "libipc/udp.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dzIPC::hybrid {
namespace {
using Clock=std::chrono::steady_clock;
int process_id() noexcept {
#ifdef _WIN32
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
void put(std::uint8_t* p,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[n-1-i]=std::uint8_t(v>>(8*i));}
std::uint64_t get(const std::uint8_t* p,unsigned n){std::uint64_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|p[i];return v;}
std::string key(const DiscoveryState& s){std::string k(reinterpret_cast<const char*>(s.scope.data()),32);std::uint8_t id[4];put(id,s.msg_id,4);k.append(reinterpret_cast<char*>(id),4);return k;}
struct Peer {Identity locality{}; bool shm=false; Clock::time_point seen;};
class Discovery {
public:
    Discovery():pid_(process_id()),thread_([this]{run();}){}
    ~Discovery(){stop_.store(true);cv_.notify_all();thread_.join();}
    std::shared_ptr<DiscoveryState> add(const std::string& topic,std::uint64_t domain,std::uint32_t id,bool sub,bool shm){
        auto s=std::make_shared<DiscoveryState>();s->scope=common::channel_scope_token(topic,domain,common::ScopeKind::PubSub);
        s->locality=local_identity();s->endpoint=new_endpoint_id();s->msg_id=id;s->subscriber=sub;s->local_shm=shm;
        // fork 后不可复用父进程线程，保持网络发送的保守默认值。
        if(process_id()!=pid_)return s;
        std::lock_guard<std::mutex> l(mutex_);states_.push_back(s);cv_.notify_all();return s;
    }
private:
    void run(){
        ipc::socket::UDPNode wire("dzipc-hybrid-discovery","239.255.250.250",11245);
        std::array<std::uint8_t,32> discovery_scope{};
        std::memcpy(discovery_scope.data(), "DZHDISC1", 8);
        wire.set_scope(discovery_scope);
        if(!wire.connect())return; // 所有状态保留 network_needed=true。
        auto next=Clock::now();
        std::unordered_map<std::string,std::unordered_map<std::uint64_t,Peer>> peers;
        bool healthy=true;
        while(!stop_.load()){
            std::vector<std::shared_ptr<DiscoveryState>> states;
            {std::lock_guard<std::mutex> l(mutex_);
             for(auto it=states_.begin();it!=states_.end();){if(auto s=it->lock();s&&s->active.load()){states.push_back(s);++it;}else it=states_.erase(it);}}
            auto now=Clock::now();
            if(now>=next){
                std::array<std::uint8_t,1416> frame{};std::memcpy(frame.data(),"DZHD0001",8);std::size_t count=0;
                auto flush=[&]{if(count){ipc::buffer b(frame.data(),8+64*count,nullptr);if(!wire.send(b))healthy=false;count=0;}};
                for(const auto& s:states){
                    if(!s->subscriber)continue;
                    auto* p=frame.data()+8+64*count;std::memcpy(p,s->scope.data(),32);std::memcpy(p+32,s->locality.data(),16);
                    put(p+48,s->endpoint,8);put(p+56,s->msg_id,4);put(p+60,s->local_shm?1:0,4);
                    if(++count==22)flush();
                }
                flush();next=now+std::chrono::milliseconds(250);
            }
            for(unsigned budget=0;budget<4096 && wire.readable();++budget){
                auto b=wire.receive_nowait();if(b.size()<72 || (b.size()-8)%64!=0)continue;
                const auto* p=static_cast<const std::uint8_t*>(b.data());if(std::memcmp(p,"DZHD0001",8))continue;
                for(std::size_t at=8;at+64<=b.size();at+=64){
                    auto* r=p+at;std::string k(reinterpret_cast<const char*>(r),32);k.append(reinterpret_cast<const char*>(r+56),4);
                    if(peers.size()>8192){healthy=false;break;}
                    auto& bucket=peers[k];if(bucket.size()>=4096){healthy=false;continue;}
                    auto& peer=bucket[get(r+48,8)];std::memcpy(peer.locality.data(),r+32,16);peer.shm=get(r+60,4)==1;peer.seen=now;
                }
            }
            for(auto it=peers.begin();it!=peers.end();){
                auto& bucket=it->second;
                for(auto p=bucket.begin();p!=bucket.end();)if(now-p->second.seen>std::chrono::milliseconds(1500))p=bucket.erase(p);else ++p;
                if(bucket.empty())it=peers.erase(it);else ++it;
            }
            for(auto& s:states){
                if(s->subscriber)continue;
                bool any=false,network=!healthy;
                if(auto p=peers.find(key(*s));p!=peers.end())for(auto& entry:p->second){
                    any=true;if(!s->local_shm || !entry.second.shm || entry.second.locality!=s->locality)network=true;
                }
                s->any_subscriber.store(any);s->network_needed.store(network);
            }
            std::unique_lock<std::mutex> l(mutex_);cv_.wait_for(l,std::chrono::milliseconds(20));
        }
    }
    int pid_;std::mutex mutex_;std::condition_variable cv_;std::vector<std::weak_ptr<DiscoveryState>> states_;
    std::atomic<bool> stop_{false};std::thread thread_;
};
}
std::uint64_t new_endpoint_id(){std::random_device r;return (std::uint64_t(r())<<32)^r()^std::uint64_t(Clock::now().time_since_epoch().count());}
Identity local_identity(){return info_pool::IpcInfoPool::instance().local_identity();}
std::shared_ptr<DiscoveryState> discover(const std::string& topic,std::uint64_t domain,std::uint32_t id,bool sub,bool shm){
    static Discovery service;return service.add(topic,domain,id,sub,shm);
}
}
