#include <set>
#include "gtest/gtest.h"
#include "dzIPC/common/channel_scope.h"
#include "libipc/udp.h"
#include <cstring>
#include <limits>
#include <string>
#include <unistd.h>

using dzIPC::common::ScopeKind;
using namespace dzIPC::common;
TEST(ChannelScope, DomainAndKindArePartOfIdentity) {
    const std::string topic="scope/topic";
    auto a=channel_scope_token(topic,0,ScopeKind::PubSub);
    EXPECT_NE(a,channel_scope_token(topic,kUdpPortWindow,ScopeKind::PubSub));
    EXPECT_NE(a,channel_scope_token(topic,1ULL<<32,ScopeKind::PubSub));
    EXPECT_NE(a,channel_scope_token(topic,0,ScopeKind::Service));
    EXPECT_NE(a,channel_scope_token("scope_topic",0,ScopeKind::PubSub));
    EXPECT_EQ(a,channel_scope_token(topic,0,ScopeKind::PubSub));
    EXPECT_NE(channel_scope_key("1:x",2,ScopeKind::PubSub),channel_scope_key("x",21,ScopeKind::PubSub));
}
TEST(ChannelScope, PortsAreBoundedAndAlignedWithDistinctDomainZeroTopics) {
    std::set<std::uint16_t> ports;
    for(unsigned i=0;i<1000;++i) {
        const auto topic="topic"+std::to_string(i);
        for(auto domain:{0ULL,54081ULL,1ULL<<32,~0ULL}) {
            const auto p=socket_scope_port(topic,domain,ScopeKind::PubSub);
            EXPECT_GE(p,UDP_DISCOVERY_BASE_PORT);EXPECT_LE(p+kUdpPortOffsetMax,32767);
            EXPECT_EQ((p-UDP_DISCOVERY_BASE_PORT)%5,0);
        }
        ports.insert(socket_scope_port(topic,0,ScopeKind::PubSub));
    }
    EXPECT_GT(ports.size(),850u); // 4263 个基址槽中取 1000 个话题，允许正常散列碰撞。
}
TEST(ChannelScope, ForcedEndpointCollisionRejectsForeignAndLegacyFrames) {
    const auto topic="scope_forced_"+std::to_string(getpid());
    const auto group=socket_scope_address(topic,7,ScopeKind::PubSub);
    const auto port=socket_scope_port(topic,7,ScopeKind::PubSub);
    ipc::socket::UDPNode rx(topic.c_str(),group.c_str(),port,ipc::socket::NodeRole::RecvOnly);
    ipc::socket::UDPNode tx(topic.c_str(),group.c_str(),port,ipc::socket::NodeRole::SendOnly);
    auto own=channel_scope_token(topic,7,ScopeKind::PubSub);
    rx.set_scope(own);ASSERT_TRUE(rx.connect());ASSERT_TRUE(tx.connect());
    std::array<std::uint8_t,ipc::wire_packet_size> payload{};payload.fill(0xAC);
    ipc::buffer data(payload.data(),payload.size(),nullptr);
    ASSERT_TRUE(tx.send(data));EXPECT_TRUE(rx.receive(100).empty());
    for(auto foreign:{channel_scope_token(topic,8,ScopeKind::PubSub),
                      channel_scope_token(topic,7,ScopeKind::Service),
                      channel_scope_token(topic+"_other",7,ScopeKind::PubSub)}) {
        tx.set_scope(foreign);ASSERT_TRUE(tx.send(data));EXPECT_TRUE(rx.receive(100).empty());
    }
    tx.set_scope(own);ASSERT_TRUE(tx.send(data));auto got=rx.receive(1000);
    ASSERT_EQ(got.size(),payload.size());EXPECT_EQ(std::memcmp(got.data(),payload.data(),payload.size()),0);
}

#include "dzIPC/common/name_operator.h"
#include "dzIPC/ipc_info_pool.h"
TEST(ChannelScope, ShmScopeSeparatesAliasesKindsAndFullWidthDomains) {
    EXPECT_NE(shm_topic_segment_name("/a/b",0),shm_topic_segment_name("_a_b",0));
    EXPECT_NE(shm_service_prefix("/a/b",0),shm_service_prefix("_a_b",0));
    EXPECT_NE(shm_topic_segment_name("x",0),shm_topic_segment_name("x",1ULL<<32));
    EXPECT_NE(shm_service_prefix("x",0),shm_service_prefix("x",1ULL<<32));
    EXPECT_NE(shm_service_prefix("x",0)+"_ser_r",shm_topic_segment_name("x",0));
    EXPECT_LT(shm_topic_segment_name(std::string(1000,'x'),~0ULL).size(),100u);
}
TEST(ChannelScope, DiscoveryPreservesFullDomain) {
    using namespace dzIPC::info_pool;
    auto& pool=IpcInfoPool::instance();
    const auto domain=(1ULL<<40)+3;
    auto slot=pool.register_entry({EntryKind::SocketSub,"scope_domain64","","socket",domain,""});
    ASSERT_GE(slot,0);bool found=false;
    for(const auto& e:pool.snapshot())if(e.slot==slot){found=true;EXPECT_EQ(e.domain_id,domain);}
    EXPECT_TRUE(found);pool.unregister_entry(slot);
}

TEST(ChannelScope, SharedPythonCppNamingVectors) {
    EXPECT_EQ(shm_topic_segment_name("/test/foo",0ULL),"dz_ipc_d0_s2_b70217718f7616a7900a27e8c02f6029_topic");
    EXPECT_EQ(shm_topic_segment_name("/test/foo",7ULL),"dz_ipc_d7_s2_ade835750cc9be22f59e4fd1cbfb0920_topic");
    EXPECT_EQ(shm_topic_segment_name("/中文/话题",1099511627776ULL),"dz_ipc_d1099511627776_s2_90c48a09216d80adce3570e96297310b_topic");
}

#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "dzIPC/common/nodelet_config.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"
#include <chrono>
#include <thread>
#include <sys/wait.h>
namespace {
struct DisableFastPath {
    bool old=dzIPC::IsNodeletEnabled(),flat=dzIPC::IsDzFlatEnabled();
    DisableFastPath(){dzIPC::EnableNodelet(false);dzIPC::EnableDzFlat(false);}
    ~DisableFastPath(){dzIPC::EnableNodelet(old);dzIPC::EnableDzFlat(flat);}
};
auto scope_td(){return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),71);}
template<class Pub,class Sub> void pubsub_domains() {
    DisableFastPath guard;
    const auto topic="scope_live_"+std::to_string(getpid());
    auto p0=scope_td(),p1=scope_td(),s0=scope_td(),s1=scope_td();
    Pub a(p0,topic,0,false),b(p1,topic,1ULL<<32,false);
    Sub ar(s0,topic,0,10,false),br(s1,topic,1ULL<<32,10,false);
    a.InitChannel();ar.InitChannel();b.InitChannel();br.InitChannel();
    auto send=[&](Pub& p,unsigned char value){auto m=std::make_shared<dzIPC::Msg::StdImage>();m->set_msg_id(71);m->data.assign(4096,value);p.publish(m);};
    bool gota=false,gotb=false;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<end && (!gota||!gotb)) {
        send(a,0xA1);send(b,0xB2);
        if(ar.try_get_clone(s0)){gota=true;EXPECT_EQ(s0->topic()->template msgcast<dzIPC::Msg::StdImage>()->data,std::vector<std::uint8_t>(4096,0xA1));}
        if(br.try_get_clone(s1)){gotb=true;EXPECT_EQ(s1->topic()->template msgcast<dzIPC::Msg::StdImage>()->data,std::vector<std::uint8_t>(4096,0xB2));}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(gota);EXPECT_TRUE(gotb);
}
auto scope_sd(){return std::make_shared<dzIPC::ServiceData>(std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());}
template<class Server,class Client> void rpc_domains() {
    // fork 发生在线程创建前；子进程服务两个同名 domain，父进程逐一验证响应身份。
    int ready[2],done[2];ASSERT_EQ(pipe(ready),0);ASSERT_EQ(pipe(done),0);
    const auto topic="scope_rpc_"+std::to_string(getpid());
    const auto child=fork();ASSERT_GE(child,0);
    if(child==0) {
        close(ready[0]);close(done[1]);DisableFastPath guard;
        auto cb=[](double marker){return [marker](std::shared_ptr<dzIPC::ServiceData>& sd){sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response={marker};};};
        {
            Server a(topic,scope_sd(),cb(10),0,false),b(topic,scope_sd(),cb(20),1ULL<<32,false);
            a.InitChannel();b.InitChannel();char c=1;
            if(write(ready[1],&c,1)!=1)_exit(2);
            if(read(done[0],&c,1)!=1)_exit(3);
        }
        _exit(0);
    }
    struct ChildGuard {pid_t pid;int out,in;~ChildGuard(){char c=0;write(out,&c,1);close(out);close(in);int status;waitpid(pid,&status,0);}} child_guard{child,done[1],ready[0]};
    close(ready[1]);close(done[0]);char c;ASSERT_EQ(read(ready[0],&c,1),1);
    DisableFastPath guard;
    Client a(topic,scope_sd(),0,false),b(topic,scope_sd(),1ULL<<32,false);a.InitChannel();b.InitChannel();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while((!a.handshake_completed() || !b.handshake_completed()) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(a.handshake_completed());ASSERT_TRUE(b.handshake_completed());
    for(unsigned i=0;i<10;++i)for(auto pair:{std::make_pair(&a,10.0),std::make_pair(&b,20.0)}) {
        auto sd=scope_sd();sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request={double(i)};
        ASSERT_TRUE(pair.first->send_request(sd,2000));
        EXPECT_EQ(sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response,std::vector<double>{pair.second});
    }
}
}
TEST(ChannelScope, SocketRpcDomainsCrossProcess){rpc_domains<dzIPC::socket::socket_ser_ipc,dzIPC::socket::socket_cli_ipc>();}
TEST(ChannelScope, ShmRpcDomainsCrossProcess){rpc_domains<dzIPC::shm::shm_ser_ipc,dzIPC::shm::shm_cli_ipc>();}
TEST(ChannelScope, SocketPubSubDomainsUseDistinctWire){pubsub_domains<dzIPC::socket::socket_pub_ipc,dzIPC::socket::socket_sub_ipc>();}
TEST(ChannelScope, ShmPubSubDomainsUseDistinctWire){pubsub_domains<dzIPC::shm::shm_pub_ipc,dzIPC::shm::shm_sub_ipc>();}
