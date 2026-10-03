#include "dzIPC/hybrid_pub_sub_ipc.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <typeinfo>

namespace dzIPC::hybrid {
namespace {
bool nonzero(const Identity& id){return std::any_of(id.begin(),id.end(),[](auto b){return b!=0;});}
}
Publisher::Publisher(const std::shared_ptr<TopicData>& msg,const std::string& topic,size_t domain,bool verbose,bool qos,int cpu,int priority)
    :pub_ipc_base(msg,topic,domain,verbose),topic_(topic),domain_(domain),msg_id_(msg->msg_id()),identity_(local_identity()),topic_msg_(msg) {
    if(nonzero(identity_)){shm_=std::make_unique<shm::shm_pub_ipc>(msg,topic,domain,verbose,qos,cpu,priority);shm_->set_internal(true);}
    socket_=std::make_unique<socket::socket_pub_ipc>(msg,topic,domain,verbose,qos,cpu,priority);socket_->set_hybrid_mode(true);socket_->set_internal(true);
}
Publisher::~Publisher(){if(discovery_)discovery_->active.store(false);}
void Publisher::refresh_discovery(){
    if(discovery_)discovery_->active.store(false);
    discovery_=discover(topic_,domain_,msg_id_,false,shm_ && shm_->channel_ready());
    started_=std::chrono::steady_clock::now();
}
void Publisher::InitChannel(std::string extra){
    std::lock_guard<std::mutex> l(mutex_);
    if(shm_)shm_->InitChannel(extra);
    socket_->InitChannel(extra);
    const std::string type_name = (topic_msg_ && topic_msg_->topic()) ? info_pool::demangle(typeid(*topic_msg_->topic()).name()) : std::string{};
    pool_reg_.rebind({info_pool::EntryKind::SocketPub, topic_, type_name, "hybrid", static_cast<std::uint64_t>(domain_), "hybrid"});
    endpoint_=new_endpoint_id();sequence_=0;refresh_discovery();
}
void Publisher::reset_message(const std::shared_ptr<TopicData>& msg){
    std::lock_guard<std::mutex> l(mutex_);if(shm_)shm_->reset_message(msg);socket_->reset_message(msg);topic_msg_=msg;msg_id_=msg->msg_id();refresh_discovery();
}
bool Publisher::network_needed() const {
    return !discovery_ || discovery_->network_needed.load() ||
           std::chrono::steady_clock::now()-started_<std::chrono::milliseconds(1000);
}
std::array<std::uint8_t,32> Publisher::source(bool local){
    std::array<std::uint8_t,32> out{};if(local)std::copy(identity_.begin(),identity_.end(),out.begin());
    auto put=[&](unsigned at,std::uint64_t n){for(unsigned i=0;i<8;++i)out[at+7-i]=std::uint8_t(n>>(8*i));};
    put(16,endpoint_);put(24,++sequence_);return out;
}
bool Publisher::publish_best_effort(std::shared_ptr<IpcMsgBase> msg){
    std::lock_guard<std::mutex> l(mutex_);if(!msg)return false;
    auto& counters=measure::CounterRegistry::instance();counters.inc(measure::CounterId::hybrid_publish_calls);
    // 只有控制面确认有本机 SHM 订阅者才尝试该腿；无订阅者时 SHM 的正常 false
    // 不能阻断远端 UDP。确认存在后若提交失败，则不跨腿补发以免部分本机重复。
    const bool local=shm_ && shm_->channel_ready() && shm_->has_subscribed();
    // 先本机完成提交，网络发送不持有 SHM 借样。部分本机失败不再跨路径重投。
    const bool local_ok=!local || shm_->publish_best_effort(msg);
    if(local && local_ok)counters.inc(measure::CounterId::hybrid_shm_sends);
    // SHM 已初始化但提交失败时，不再走 UDP 补发：失败可能发生在部分订阅者已入队之后，
    // 跨腿重投会让这些订阅者收到重复消息。调用方可据 false 处理本次发送失败。
    if(local && !local_ok)return false;
    bool network_ok=true;
    if(network_needed()){socket_->set_hybrid_source(source(local));network_ok=socket_->publish_for_sniffer(msg);if(network_ok)counters.inc(measure::CounterId::hybrid_udp_sends);}
    return network_ok;
}
bool Publisher::publish_blocking(std::shared_ptr<IpcMsgBase> msg,std::uint64_t tm){
    std::lock_guard<std::mutex> l(mutex_);socket_->set_hybrid_source(source(false));
    return socket_->publish_blocking(std::move(msg),tm);
}
bool Publisher::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg){
    std::lock_guard<std::mutex> l(mutex_);socket_->set_hybrid_source(source(false));
    return socket_->publish_for_sniffer(std::move(msg));
}
bool Publisher::publish_prebuilt_segment(const void* seg,std::size_t size){
    std::lock_guard<std::mutex> l(mutex_);
    // 不允许一腿已投递后返回 false 让应用再发 TLV。先验证，再分别提交；失败不跨路径重投。
    if(!IsDzFlatEnabled() || !seg || !dzflat::looks_like_dzflat(seg,size))return false;
    dzflat::SegHeader header{};std::memcpy(&header,seg,sizeof(header));if(header.msg_id!=msg_id_)return false;
    // 预构造接口 false 的契约是允许调用方重发；混合双腿无法原子提交，暂以纯 UDP 保持此契约。
    socket_->set_hybrid_source(source(false));return socket_->publish_prebuilt_segment(seg,size);
}
bool Publisher::has_subscribed() const {
    std::lock_guard<std::mutex> l(mutex_);return (shm_ && shm_->has_subscribed()) || (discovery_ && discovery_->any_subscriber.load());
}
Subscriber::Subscriber(const std::shared_ptr<TopicData>& msg,const std::string& topic,size_t domain,size_t queue_size,bool verbose,bool qos,int cpu,int priority)
    :sub_ipc_base(msg,topic,domain,queue_size,verbose),topic_(topic),domain_(domain),msg_id_(msg->msg_id()),topic_msg_(msg) {
    const auto id=local_identity();const std::weak_ptr<Wakeup> weak=wake_;
    auto notify=[weak]{if(auto wake=weak.lock())wake->notify();};
    if(nonzero(id)){
        shm_=std::make_unique<shm::shm_sub_ipc>(msg,topic,domain,queue_size,verbose,qos,cpu,priority);
        shm_->set_internal(true);shm_->set_receive_notifier(notify);
    }
    socket_=std::make_unique<socket::socket_sub_ipc>(msg,topic,domain,queue_size,verbose,qos,cpu,priority);
    socket_->set_internal(true);socket_->configure_hybrid(id,notify);
}
Subscriber::~Subscriber(){socket_.reset();shm_.reset();wake_->notify();}
void Subscriber::InitChannel(std::string extra){if(shm_)shm_->InitChannel(extra);socket_->InitChannel(extra);const std::string type_name=(topic_msg_ && topic_msg_->topic())?info_pool::demangle(typeid(*topic_msg_->topic()).name()):std::string{};pool_reg_.rebind({info_pool::EntryKind::SocketSub,topic_,type_name,"hybrid",static_cast<std::uint64_t>(domain_),"hybrid"});}
void Subscriber::reset_message(const std::shared_ptr<TopicData>& msg){if(shm_)shm_->reset_message(msg);socket_->reset_message(msg);topic_msg_=msg;msg_id_=msg->msg_id();}
bool Subscriber::try_get(Sample& out){
    if(turn_.fetch_add(1)%2)return socket_->try_get(out) || (shm_ && shm_->try_get(out));
    return (shm_ && shm_->try_get(out)) || socket_->try_get(out);
}
bool Subscriber::try_get_clone(std::shared_ptr<TopicData>& out){
    if(turn_.fetch_add(1)%2)return socket_->try_get_clone(out) || (shm_ && shm_->try_get_clone(out));
    return (shm_ && shm_->try_get_clone(out)) || socket_->try_get_clone(out);
}
void Subscriber::get(Sample& out){(void)get(out,std::numeric_limits<std::uint64_t>::max());}
bool Subscriber::get(Sample& out,std::uint64_t tm){
    const bool forever=tm==std::numeric_limits<std::uint64_t>::max();
    const auto deadline=forever?std::chrono::steady_clock::time_point::max():std::chrono::steady_clock::now()+std::chrono::milliseconds(tm);
    for(;;){
        std::uint64_t seen;{std::lock_guard<std::mutex> l(wake_->mutex);seen=wake_->generation;}
        if(try_get(out))return true;
        std::unique_lock<std::mutex> l(wake_->mutex);
        if(!wake_->cv.wait_until(l,deadline,[&]{return seen!=wake_->generation;}))return false;
    }
}
void Subscriber::get_clone(std::shared_ptr<TopicData>& out){
    for(;;){
        std::uint64_t seen;{std::lock_guard<std::mutex> l(wake_->mutex);seen=wake_->generation;}
        if(try_get_clone(out))return;
        std::unique_lock<std::mutex> l(wake_->mutex);wake_->cv.wait(l,[&]{return seen!=wake_->generation;});
    }
}
}
