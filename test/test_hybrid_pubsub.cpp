#include "gtest/gtest.h"
#include "dzIPC/hybrid_pub_sub_ipc.h"
#include "dzIPC/topic_ipc.h"
#include "dzIPC/common/channel_scope.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "hybrid_sniffer.h"
#include "transport_select.h"
#include <chrono>
#include <thread>
#include <set>
#include <future>
using namespace std::chrono_literals;
using namespace dzIPC;
namespace {
std::string topic(){return "/hybrid/test/"+std::to_string(hybrid::new_endpoint_id());}
auto data(){return std::make_shared<TopicData>(std::make_shared<Msg::StdImage>(),51);}
auto message(unsigned n,std::size_t bytes=128){auto m=std::make_shared<Msg::StdImage>();m->width=n;m->height=1;m->data.assign(bytes,static_cast<unsigned char>(n));m->set_msg_id(51);return m;}
template<class Pred> bool wait(Pred pred,int ms=3000){auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);do{if(pred())return true;std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<end);return false;}
template<class Sub> bool read(Sub& sub,unsigned& value){
    dzIPC::Sample sample;if(sub.try_get(sample)){Msg::StdImage msg;if(!msg.dzflat_read(sample.data(),sample.size()))return false;value=msg.width;return !msg.data.empty() && msg.data.front()==static_cast<unsigned char>(value);}
    auto d=data();if(sub.try_get_clone(d)){auto msg=std::static_pointer_cast<Msg::StdImage>(d->topic());value=msg->width;return !msg->data.empty() && msg->data.front()==static_cast<unsigned char>(value);}return false;
}
}
TEST(HybridPubSub, PublicApiDefaultsToShmAndRegistersOneLogicalEndpoint){
    auto name=topic();auto d=data();pimpl::publisher_ipc_impl pub(d,name,0,IPCType::Socket);pimpl::subscriber_ipc_impl sub(d,name,0,10,IPCType::Socket);
    sub.InitChannel();pub.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));std::this_thread::sleep_for(1200ms);
    auto& c=measure::CounterRegistry::instance();auto before=c.get(measure::CounterId::hybrid_udp_sends);auto logical=c.get(measure::CounterId::hybrid_publish_calls);
    ASSERT_TRUE(pub.publish(message(7)));unsigned n=0;ASSERT_TRUE(wait([&]{return read(sub,n);}));EXPECT_EQ(n,7u);
    EXPECT_EQ(c.get(measure::CounterId::hybrid_publish_calls),logical+1);EXPECT_EQ(c.get(measure::CounterId::hybrid_udp_sends),before);
    std::size_t entries=0;for(auto& e:info_pool::IpcInfoPool::instance().snapshot())if(e.topic_name==name){++entries;EXPECT_EQ(e.extra,"hybrid");}
    EXPECT_EQ(entries,2u);EXPECT_FALSE(wait([&]{return read(sub,n);},100));
}
TEST(HybridPubSub, MixedLocalAndSocketOnlyHaveNoDuplicateDelivery){
    auto name=topic();auto d=data();hybrid::Publisher pub(d,name,0);hybrid::Subscriber local(d,name,0,10);socket::socket_sub_ipc udp(d,name,0,10);
    local.InitChannel();udp.InitChannel();pub.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));std::this_thread::sleep_for(400ms);
    for(unsigned i=1;i<=10;++i){ASSERT_TRUE(pub.publish(message(i)));unsigned a=0,b=0;ASSERT_TRUE(wait([&]{return read(local,a);}));ASSERT_TRUE(wait([&]{return read(udp,b);}));EXPECT_EQ(a,i);EXPECT_EQ(b,i);}
    unsigned n=0;EXPECT_FALSE(wait([&]{return read(local,n);},150));EXPECT_GT(local.suppressed_datagrams(),0u);
}
TEST(HybridPubSub, SocketOnlyPublisherAndReliableHybridReachHybridSubscriber){
    auto name=topic();auto d=data();hybrid::Subscriber sub(d,name,0,10);socket::socket_pub_ipc pure(d,name,0);hybrid::Publisher pub(d,name,0);
    sub.InitChannel();pure.InitChannel();pub.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));
    ASSERT_TRUE(pure.publish_for_sniffer(message(3)));unsigned n=0;ASSERT_TRUE(wait([&]{return read(sub,n);}));EXPECT_EQ(n,3u);
    ASSERT_TRUE(pub.publish_blocking(message(4),1000));ASSERT_TRUE(wait([&]{return read(sub,n);}));EXPECT_EQ(n,4u);EXPECT_FALSE(wait([&]{return read(sub,n);},100));
}
TEST(HybridPubSub, LateSocketSubscriberTriggersNetworkAndLeaseExpires){
    auto name=topic();auto d=data();hybrid::Publisher pub(d,name,0);hybrid::Subscriber local(d,name,0,10);local.InitChannel();pub.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));std::this_thread::sleep_for(1200ms);
    auto& c=measure::CounterRegistry::instance();auto before=c.get(measure::CounterId::hybrid_udp_sends);
    {socket::socket_sub_ipc sub(d,name,0,10);sub.InitChannel();std::this_thread::sleep_for(400ms);ASSERT_TRUE(pub.publish(message(5)));unsigned n;ASSERT_TRUE(wait([&]{return read(sub,n);}));EXPECT_EQ(n,5u);}
    EXPECT_GT(c.get(measure::CounterId::hybrid_udp_sends),before);std::this_thread::sleep_for(1800ms);before=c.get(measure::CounterId::hybrid_udp_sends);ASSERT_TRUE(pub.publish(message(6)));EXPECT_EQ(c.get(measure::CounterId::hybrid_udp_sends),before);
}
TEST(HybridPubSub, BlockingViewReadWakesFromEitherLeg){
    auto name=topic();auto d=data();hybrid::Publisher pub(d,name,0);hybrid::Subscriber sub(d,name,0,10);sub.InitChannel();pub.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));
    auto future=std::async(std::launch::async,[&]{dzIPC::Sample s;return sub.get(s,2000) && s.valid();});std::this_thread::sleep_for(30ms);ASSERT_TRUE(pub.publish(message(9)));EXPECT_TRUE(future.get());
    dzIPC::Sample empty;EXPECT_FALSE(sub.get(empty,20));
}
TEST(HybridPubSub, DomainIsolationAndTwoPublishers){
    auto name=topic();auto d=data();hybrid::Publisher a(d,name,0);socket::socket_pub_ipc b(d,name,0);hybrid::Publisher foreign(d,name,1ULL<<32);hybrid::Subscriber sub(d,name,0,10);sub.InitChannel();a.InitChannel();b.InitChannel();foreign.InitChannel();ASSERT_TRUE(wait([&]{return a.has_subscribed();}));std::this_thread::sleep_for(500ms);
    ASSERT_TRUE(a.publish(message(11)));ASSERT_TRUE(b.publish_for_sniffer(message(12)));ASSERT_TRUE(foreign.publish(message(99)));std::set<unsigned> seen;unsigned n;
    ASSERT_TRUE(wait([&]{if(read(sub,n))seen.insert(n);return seen.size()==2;}));EXPECT_EQ(seen,(std::set<unsigned>{11,12}));EXPECT_FALSE(wait([&]{return read(sub,n);},100));
}
TEST(HybridPubSub, TopicCatChoosesLogicalHybridAndReadsOnce){
    auto name=topic();auto d=data();hybrid::Publisher pub(d,name,0);pub.InitChannel();auto sel=dzipc_topic_cat::select_sniffer_entry(info_pool::IpcInfoPool::instance().snapshot(),name,false,dzipc_topic_cat::TransportPreference::Auto);ASSERT_TRUE(sel.hybrid);
    hybrid_sniffer sniff(name,0,51);socket::socket_sub_ipc network(d,name,0,10);network.InitChannel();ASSERT_TRUE(wait([&]{return pub.has_subscribed();}));std::this_thread::sleep_for(350ms);
    ASSERT_TRUE(pub.publish(message(20)));sniffer_info got;ASSERT_TRUE(wait([&]{got=sniff.try_recv();return !got.request.empty();}));Msg::StdImage decoded;ASSERT_TRUE(decoded.dzflat_read(got.request.data(),got.request.size()));EXPECT_EQ(decoded.width,20u);
    EXPECT_FALSE(wait([&]{return !sniff.try_recv().request.empty();},100));
}
