// 每话题每尺寸档 10 块：容量、隔离、广播、跨进程和归还的真实队列回归。
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <limits>
#include <filesystem>
#include <fstream>
#include "dzIPC/common/hash.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "gtest/gtest.h"
#include "libipc/ipc.h"
#include "libipc/sniffer.h"
#include "libipc/shm.h"
#include "libipc/utility/id_pool.h"
#include "libipc/memory/resource.h"

namespace {
constexpr std::size_t kCount = 10;
std::string unique() {
    static std::atomic<unsigned> seq{0};
    return "topic10_" + std::to_string(getpid()) + "_" + std::to_string(seq++);
}
struct Pair {
    std::string prefix, name;
    std::unique_ptr<ipc::route> tx, rx;
    explicit Pair(std::string p = unique(), std::string n = unique()) : prefix(std::move(p)), name(std::move(n)) {
        tx = std::make_unique<ipc::route>(ipc::prefix{prefix.c_str()},name.c_str(),ipc::sender,false);
        rx = std::make_unique<ipc::route>(ipc::prefix{prefix.c_str()},name.c_str(),ipc::receiver,false);
    }
};
struct Held {
    ipc::route& tx;
    std::vector<ipc::loan_t> items;
    explicit Held(ipc::route& t) : tx(t) {}
    ~Held() { for (auto& lo : items) tx.discard_loan(lo); }
    std::size_t take(std::size_t bytes) {
        for (unsigned n=0; n<50; ++n) {
            auto lo = tx.loan(bytes);
            if (!lo.valid()) break;
            items.push_back(lo);
        }
        return items.size();
    }
};
void fill(ipc::loan_t& lo, unsigned char value) { std::memset(lo.data,value,lo.size); }
bool matches(ipc::buff_t const& b, unsigned char value) {
    if (b.empty()) return false;
    auto* p=static_cast<unsigned char const*>(b.data());
    for(std::size_t i=0;i<b.size();++i) if(p[i]!=value) return false;
    return true;
}
}

TEST(TopicChunkPool, TenBlocksThenRejectAndRecover) {
    Pair p; ASSERT_TRUE(p.tx->wait_for_recv(1,1000));
    for(int cycle=0;cycle<100;++cycle) {
        Held held(*p.tx); ASSERT_EQ(held.take(512),kCount);
        ipc::loan_status reason{};
        EXPECT_FALSE(p.tx->loan(512,reason).valid());
        EXPECT_EQ(reason,ipc::loan_status::pool_exhausted);
        std::set<void*> addresses;
        for(auto& lo:held.items) EXPECT_TRUE(addresses.insert(lo.data).second);
    }
}

TEST(TopicChunkPool, DifferentTopicsAndPrefixesAndSizesAreIndependent) {
    const auto pref=unique(); Pair a(pref,"a"), b(pref,"b"), c(unique(),"a");
    Held ha(*a.tx), hb(*b.tx), hc(*c.tx), other_size(*a.tx), same_class(*a.tx);
    ASSERT_EQ(ha.take(100),kCount);
    EXPECT_EQ(same_class.take(500),0u);
    EXPECT_EQ(hb.take(100),kCount);
    EXPECT_EQ(hc.take(100),kCount);
    EXPECT_EQ(other_size.take(4000),kCount);
    EXPECT_NE(ha.items[0].data,hb.items[0].data);
}

TEST(TopicChunkPool, SameTopicAcrossHandlesSharesExactlyTen) {
    Pair p;
    ipc::route other(ipc::prefix{p.prefix.c_str()},p.name.c_str(),ipc::sender,false);
    Held first(*p.tx), second(other);
    ASSERT_EQ(first.take(2000),kCount);
    EXPECT_EQ(second.take(2000),0u);
}

TEST(TopicChunkPool, AllTopicsAreIndependentWithNoGlobalPool) {
    const auto pref=unique(); Pair a(pref,"a"),b(pref,"b");
    Held first(*a.tx), neighbour(*b.tx), same(*a.tx);
    EXPECT_EQ(first.take(100),kCount);
    EXPECT_EQ(neighbour.take(100),kCount);
    EXPECT_EQ(same.take(100),0u);
}

TEST(TopicChunkPool, SegmentAllocatesTenPayloadBlocks) {
    Pair p; auto lo=p.tx->loan(100); ASSERT_TRUE(lo.valid());
    const std::size_t stride=((lo.size+16+1023)/1024)*1024;
    struct alignas(8) Header { ipc::pool_identity_header identity; ipc::id_pool<> ids; ipc::spin_lock lock; };
    const auto pref=ipc::topic_pool_prefix(ipc::make_string(p.prefix.c_str()),ipc::make_string(p.name.c_str()));
    const auto name=pref+"CHUNK_INFO__"+ipc::to_string(stride)+"__C10";
    ipc::shm::handle view;
    ASSERT_TRUE(view.acquire(name.c_str(),sizeof(Header)+kCount*stride,ipc::shm::open));
    // POSIX shm 段尾另有 4 字节引用计数，载荷区确为 10 个步长。
    EXPECT_EQ(view.size(),sizeof(Header)+kCount*stride+sizeof(std::atomic<std::int32_t>));
    EXPECT_EQ(static_cast<Header*>(view.get())->identity.topic_id,dzIPC::common::fnv1a64(p.name.c_str()));
    view.release_no_unlink();
    p.tx->discard_loan(lo);
}

TEST(TopicChunkPool, RepeatedLoanPublishAndReceive) {
    Pair p;
    for(int i=0;i<100;++i) {
        auto lo=p.tx->loan(2000);
        ASSERT_TRUE(lo.valid()); fill(lo,static_cast<unsigned char>(i));
        ASSERT_TRUE(p.tx->publish_loan(lo));
        auto b=p.rx->recv(1000);
        EXPECT_EQ(b.data(),lo.data);
        EXPECT_TRUE(matches(b,static_cast<unsigned char>(i)));
    }
    Held held(*p.tx); EXPECT_EQ(held.take(2000),kCount);
}

TEST(TopicChunkPool, LastBroadcastHolderReleasesBlock) {
    Pair p;
    ipc::route second(ipc::prefix{p.prefix.c_str()},p.name.c_str(),ipc::receiver,false);
    ASSERT_TRUE(p.tx->wait_for_recv(2,1000));
    Held pending(*p.tx); ASSERT_EQ(pending.take(500),kCount);
    auto lo=pending.items.back();pending.items.pop_back();fill(lo,0xA6);
    ASSERT_TRUE(p.tx->publish_loan(lo));
    auto a=p.rx->recv(1000),b=second.recv(1000);
    ASSERT_TRUE(matches(a,0xA6));ASSERT_TRUE(matches(b,0xA6));
    EXPECT_EQ(a.data(),b.data());
    a={}; EXPECT_FALSE(p.tx->loan(500).valid());
    b={}; auto recovered=p.tx->loan(500);ASSERT_TRUE(recovered.valid());
    p.tx->discard_loan(recovered);
}

TEST(TopicChunkPool, BufferCanOutliveReceiverAndSenderHandle) {
    const auto pref=unique(), name=unique(); ipc::buff_t kept;
    {
        Pair p(pref,name); auto lo=p.tx->loan(1000);ASSERT_TRUE(lo.valid());
        fill(lo,0x72);ASSERT_TRUE(p.tx->publish_loan(lo));
        kept=p.rx->recv(1000);ASSERT_TRUE(matches(kept,0x72));
    }
    EXPECT_TRUE(matches(kept,0x72));kept={};
    Pair reopened(pref,name);Held held(*reopened.tx);EXPECT_EQ(held.take(1000),kCount);
}

TEST(TopicChunkPool, SnifferResolvesTopicPoolWithoutRecycling) {
    Pair p; ipc::sniffer sniffer;
    ASSERT_TRUE(sniffer.open(ipc::prefix{p.prefix.c_str()},p.name.c_str()));
    sniffer.skip_to_latest();
    EXPECT_TRUE(sniffer.try_recv().empty()); // 首次读建立起点，再发布待观察消息
    for(int repeat=0;repeat<2;++repeat) {
        auto lo=p.tx->loan(2000);
        ASSERT_TRUE(lo.valid());fill(lo,0x94);
        ASSERT_TRUE(p.tx->publish_loan(lo));
        auto watched=sniffer.recv(1000);EXPECT_TRUE(matches(watched,0x94));
        auto owned=p.rx->recv(1000);EXPECT_TRUE(matches(owned,0x94));
        sniffer.close();
        EXPECT_TRUE(matches(owned,0x94));
        ASSERT_TRUE(sniffer.open(ipc::prefix{p.prefix.c_str()},p.name.c_str()));
        sniffer.skip_to_latest();
        EXPECT_TRUE(sniffer.try_recv().empty()); // 首次读建立起点，再发布待观察消息
    }
}

TEST(TopicChunkPool, FreshChildMapsTopicPoolAndReleases) {
    // fork 在建池前，避免只验证继承的映射。子进程用独立 route 打开相同话题。
    const auto pref=unique(), name=unique();
    int gate[2];ASSERT_EQ(pipe(gate),0);
    const pid_t pid=fork();ASSERT_GE(pid,0);
    if(pid==0) {
        close(gate[1]);char value;
        if(read(gate[0],&value,1)!=1)_exit(2);
        close(gate[0]);
        ipc::route rx(ipc::prefix{pref.c_str()},name.c_str(),ipc::receiver,false);
        auto b=rx.recv(3000);
        const bool okay=matches(b,0x31);b={};rx.disconnect();
        _exit(okay?0:3);
    }
    close(gate[0]);
    ipc::route tx(ipc::prefix{pref.c_str()},name.c_str(),ipc::sender,false);
    EXPECT_EQ(write(gate[1],"x",1),1);close(gate[1]);
    ASSERT_TRUE(tx.wait_for_recv(1,3000));
    auto lo=tx.loan(4000);ASSERT_TRUE(lo.valid());fill(lo,0x31);
    ASSERT_TRUE(tx.publish_loan(lo));
    int status=0;ASSERT_EQ(waitpid(pid,&status,0),pid);
    ASSERT_TRUE(WIFEXITED(status));EXPECT_EQ(WEXITSTATUS(status),0);
    ipc::route rx(ipc::prefix{pref.c_str()},name.c_str(),ipc::receiver,false);
    Held held(tx);EXPECT_EQ(held.take(4000),kCount);
}

TEST(TopicChunkPool, ConcurrentFirstLoansRemainUnique) {
    Pair p; std::vector<ipc::loan_t> loans(10);std::vector<std::thread> threads;
    std::atomic<int> ready{0};std::atomic<bool> start{false};
    for(int i=0;i<10;++i)threads.emplace_back([&,i]{
        ++ready;while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        loans[i]=p.tx->loan(8192);
    });
    while(ready.load()!=10)std::this_thread::yield();
    start.store(true,std::memory_order_release);
    for(auto& t:threads)t.join();
    std::set<void*> seen;
    for(auto& lo:loans){EXPECT_TRUE(lo.valid());EXPECT_TRUE(seen.insert(lo.data).second);}
    EXPECT_FALSE(p.tx->loan(8192).valid());
    for(auto& lo:loans)p.tx->discard_loan(lo);
}

TEST(TopicChunkPool, InvalidWireIdsAndOversizeAreRejected) {
    for(auto bad : {-1,-12,std::numeric_limits<int>::min(),0,10})
        EXPECT_EQ(ipc::detail::storage_from_wire(bad),-1);
    for(int i=0;i<10;++i) {
        const auto id=i;
        EXPECT_LT(ipc::detail::storage_to_wire(id),0); // 旧接收器的 id < 0 守卫
        EXPECT_EQ(ipc::detail::storage_from_wire(ipc::detail::storage_to_wire(id)),id);
    }
    Pair p;ipc::loan_status reason{};
    EXPECT_FALSE(p.tx->loan(std::numeric_limits<std::size_t>::max(),reason).valid());
    EXPECT_EQ(reason,ipc::loan_status::size_too_large);
}

namespace {
std::vector<std::filesystem::path> pools_for(const std::string& prefix,const std::string& name) {
    const auto key=ipc::topic_pool_prefix(ipc::make_string(prefix.c_str()),ipc::make_string(name.c_str()))+"CHUNK_INFO__";
    std::vector<std::filesystem::path> found;
    for(const auto& e:std::filesystem::directory_iterator("/dev/shm"))
        if(e.path().filename().string().rfind(key.c_str(),0)==0)found.push_back(e.path());
    return found;
}
}

TEST(TopicPoolLifecycle, NoPayloadPoolUntilFirstUseAndDestroyRemovesAllSizes) {
    const auto pref=unique(),name=unique();
    for(int round=0;round<20;++round) {
        {
            Pair p(pref,name);
            EXPECT_TRUE(pools_for(pref,name).empty());
            for(auto size:{100u,4000u,1048577u}) {
                auto lo=p.tx->loan(size);ASSERT_TRUE(lo.valid());p.tx->discard_loan(lo);
            }
            EXPECT_EQ(pools_for(pref,name).size(),3u);
        }
        EXPECT_TRUE(pools_for(pref,name).empty());
    }
}

TEST(TopicPoolLifecycle, RawLoanOutlivesRoutesAndLastCopyReturnsIt) {
    const auto pref=unique(),name=unique();ipc::loan_t held,copy;
    {
        Pair p(pref,name);held=p.tx->loan(4000);ASSERT_TRUE(held.valid());
        fill(held,0x75);copy=held;
    }
    ASSERT_EQ(pools_for(pref,name).size(),1u);
    EXPECT_EQ(static_cast<unsigned char*>(held.data)[100],0x75);
    held={};EXPECT_EQ(pools_for(pref,name).size(),1u);
    copy={};EXPECT_TRUE(pools_for(pref,name).empty());
}

TEST(TopicPoolLifecycle, ReceivedSampleOutlivesRoutesAndDefersDeletion) {
    const auto pref=unique(),name=unique();ipc::buff_t held;
    {
        Pair p(pref,name);auto lo=p.tx->loan(4000);ASSERT_TRUE(lo.valid());
        fill(lo,0x63);ASSERT_TRUE(p.tx->publish_loan(lo));
        held=p.rx->recv(1000);ASSERT_TRUE(matches(held,0x63));
    }
    EXPECT_EQ(pools_for(pref,name).size(),1u);
    EXPECT_TRUE(matches(held,0x63));
    held={};EXPECT_TRUE(pools_for(pref,name).empty());
}

TEST(TopicPoolLifecycle, ClearStorageAndNewHandlesReuseActivePool) {
    Pair p;auto lo=p.tx->loan(4000);ASSERT_TRUE(lo.valid());fill(lo,0x48);
    const auto files=pools_for(p.prefix,p.name);ASSERT_EQ(files.size(),1u);
    struct stat before{},after{};ASSERT_EQ(stat(files[0].c_str(),&before),0);
    ipc::route::clear_storage(ipc::prefix{p.prefix.c_str()},p.name.c_str());
    p.tx->disconnect(); // route 为单发布者；让新句柄取得发布权，借样仍在用
    {
        ipc::route other(ipc::prefix{p.prefix.c_str()},p.name.c_str(),ipc::sender,false);
        Held remaining(other);EXPECT_EQ(remaining.take(4000),9u);
        EXPECT_EQ(static_cast<unsigned char*>(lo.data)[20],0x48);
    }
    ASSERT_EQ(stat(files[0].c_str(),&after),0);EXPECT_EQ(before.st_ino,after.st_ino);
    ASSERT_TRUE(p.tx->publish_loan(lo));EXPECT_TRUE(matches(p.rx->recv(1000),0x48));
}

TEST(TopicPoolLifecycle, OrdinarySendAndLoanUseSameSizePool) {
    Pair p;auto lo=p.tx->loan(100);ASSERT_TRUE(lo.valid());fill(lo,0x37);
    ASSERT_TRUE(p.tx->publish_loan(lo));auto first=p.rx->recv(1000);ASSERT_TRUE(matches(first,0x37));
    std::vector<unsigned char> value(100,0x39);
    ASSERT_TRUE(p.tx->send(value.data(),value.size()));
    auto second=p.rx->recv(1000);ASSERT_TRUE(matches(second,0x39));
    EXPECT_EQ(pools_for(p.prefix,p.name).size(),1u);
    Held remainder(*p.tx);EXPECT_EQ(remainder.take(100),8u);
}

TEST(TopicPoolLifecycle, CrossProcessInitDoesNotDeleteActivePool) {
    Pair p;Held held(*p.tx);ASSERT_EQ(held.take(4000),10u);
    fill(held.items[0],0x69);
    p.tx->disconnect(); // 池租约保留，释放单发布者队列的发布权
    const auto child=fork();ASSERT_GE(child,0);
    if(child==0) {
        ipc::route::clear_storage(ipc::prefix{p.prefix.c_str()},p.name.c_str());
        Pair other(p.prefix,p.name);
        ipc::loan_status why{};auto lo=other.tx->loan(4000,why);
        _exit(!lo.valid() && why==ipc::loan_status::pool_exhausted ? 0:2);
    }
    int status;ASSERT_EQ(waitpid(child,&status,0),child);
    ASSERT_TRUE(WIFEXITED(status));EXPECT_EQ(WEXITSTATUS(status),0);
    EXPECT_EQ(static_cast<unsigned char*>(held.items[0].data)[100],0x69);
}

TEST(TopicPoolLifecycle, CrashResidueIsDeletedBeforeRecreate) {
    const auto pref=unique(),name=unique();
    const auto child=fork();ASSERT_GE(child,0);
    if(child==0) {
        Pair p(pref,name);Held held(*p.tx);
        if(held.take(4000)!=10u)_exit(2);
        _exit(0); // 不运行析构，模拟进程突然消失
    }
    int status;ASSERT_EQ(waitpid(child,&status,0),child);
    ASSERT_TRUE(WIFEXITED(status));ASSERT_EQ(WEXITSTATUS(status),0);
    const auto files=pools_for(pref,name);ASSERT_EQ(files.size(),1u);
    struct stat before{},after{};ASSERT_EQ(stat(files[0].c_str(),&before),0);
    {
        Pair p(pref,name);
        EXPECT_TRUE(pools_for(pref,name).empty());
        Held held(*p.tx);ASSERT_EQ(held.take(4000),10u);
        ASSERT_EQ(stat(files[0].c_str(),&after),0);EXPECT_NE(before.st_ino,after.st_ino);
    }
    EXPECT_TRUE(pools_for(pref,name).empty());
}

TEST(TopicPoolLifecycle, LateSampleAndNewGenerationShareCapacity) {
    const auto pref=unique(),name=unique();ipc::buff_t sample;
    {
        Pair p(pref,name);auto lo=p.tx->loan(4000);ASSERT_TRUE(lo.valid());fill(lo,0x51);
        ASSERT_TRUE(p.tx->publish_loan(lo));sample=p.rx->recv(1000);
    }
    {
        Pair p(pref,name);Held remaining(*p.tx);ASSERT_EQ(remaining.take(4000),9u);
        EXPECT_TRUE(matches(sample,0x51));
        sample={};auto last=p.tx->loan(4000);ASSERT_TRUE(last.valid());p.tx->discard_loan(last);
    }
    EXPECT_TRUE(pools_for(pref,name).empty());
}

TEST(TopicPoolLifecycle, DiscardAndPublishCopiesCannotReturnTwice) {
    Pair p;auto lo=p.tx->loan(100);auto copy=lo;
    ASSERT_TRUE(lo.valid());p.tx->discard_loan(copy);p.tx->discard_loan(lo);
    EXPECT_FALSE(p.tx->publish_loan(lo));
    Held all(*p.tx);EXPECT_EQ(all.take(100),10u);
}

TEST(TopicPoolLifecycle, ConcurrentCreateAndDestroyPreserveOtherTopic) {
    Pair stable;auto hold=stable.tx->loan(100);ASSERT_TRUE(hold.valid());fill(hold,0x21);
    const auto pref=unique(),name=unique();std::atomic<unsigned> errors{0};
    std::vector<std::thread> threads;
    for(int t=0;t<6;++t)threads.emplace_back([&]{
        for(int i=0;i<50;++i) {
            auto pool=ipc::acquire_topic_pool(ipc::make_string(pref.c_str()),ipc::make_string(name.c_str()));
            if(!pool || !pool->valid())++errors;
        }
    });
    for(auto& t:threads)t.join();
    EXPECT_EQ(errors.load(),0u);EXPECT_EQ(static_cast<unsigned char*>(hold.data)[0],0x21);
    stable.tx->discard_loan(hold);
}

TEST(TopicPoolLifecycle, LongNamesAndDomainsHaveDistinctIdentity) {
    const auto pref=unique();const std::string a="dz_ipc_d1_"+std::string(180,'a');
    const std::string b="dz_ipc_d2_"+std::string(180,'a');
    {
        Pair first(pref,a),second(pref,b);Held x(*first.tx),y(*second.tx);
        ASSERT_EQ(x.take(4000),10u);ASSERT_EQ(y.take(4000),10u);
        EXPECT_NE(pools_for(pref,a),pools_for(pref,b));
        EXPECT_LT(pools_for(pref,a)[0].filename().string().size(),255u);
    }
    EXPECT_TRUE(pools_for(pref,a).empty());EXPECT_TRUE(pools_for(pref,b).empty());
}

TEST(TopicPoolLifecycle, LastCrossProcessUserOwnsDeletion) {
    const auto pref=unique(),name=unique();int ready[2],done[2];
    ASSERT_EQ(pipe(ready),0);ASSERT_EQ(pipe(done),0);
    const auto child=fork();ASSERT_GE(child,0);
    if(child==0) {
        close(ready[0]);close(done[1]);
        {
            Pair p(pref,name);auto lo=p.tx->loan(4000);
            if(!lo.valid())_exit(2);fill(lo,0x76);
            char c='R';if(write(ready[1],&c,1)!=1)_exit(3);
            if(read(done[0],&c,1)!=1)_exit(4);
            if(pools_for(pref,name).size()!=1 || static_cast<unsigned char*>(lo.data)[0]!=0x76)_exit(5);
        }
        _exit(pools_for(pref,name).empty()?0:6);
    }
    close(ready[1]);close(done[0]);char c=0;
    EXPECT_EQ(read(ready[0],&c,1),1);
    {
        auto context=ipc::acquire_topic_pool(ipc::make_string(pref.c_str()),ipc::make_string(name.c_str()));
        EXPECT_TRUE(context);
        ipc::route::clear_storage(ipc::prefix{pref.c_str()},name.c_str());
    }
    EXPECT_EQ(pools_for(pref,name).size(),1u);
    EXPECT_EQ(write(done[1],&c,1),1);close(done[1]);close(ready[0]);
    int status=0;ASSERT_EQ(waitpid(child,&status,0),child);
    ASSERT_TRUE(WIFEXITED(status));EXPECT_EQ(WEXITSTATUS(status),0);
    EXPECT_TRUE(pools_for(pref,name).empty());
}

TEST(TopicPoolLifecycle, InvalidRegistryIdentityIsNeverOverwrittenOrDeleted) {
    const auto pref=unique(),name=unique();
    const auto key=ipc::topic_pool_prefix(ipc::make_string(pref.c_str()),ipc::make_string(name.c_str()));
    const auto file=std::filesystem::path("/dev/shm")/(std::string(key.c_str())+"REG");
    {std::ofstream out(file);out<<"invalid identity";}
    EXPECT_FALSE(ipc::acquire_topic_pool(ipc::make_string(pref.c_str()),ipc::make_string(name.c_str())));
    ipc::route::clear_storage(ipc::prefix{pref.c_str()},name.c_str());
    std::ifstream in(file);std::string content;std::getline(in,content);
    EXPECT_EQ(content,"invalid identity");std::filesystem::remove(file);
}
