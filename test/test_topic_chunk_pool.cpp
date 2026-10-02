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
    std::size_t take(std::size_t bytes, bool topic = true) {
        for (unsigned n=0; n<50; ++n) {
            auto lo = topic ? tx.loan_topic(bytes) : tx.loan(bytes);
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
        EXPECT_FALSE(p.tx->loan_topic(512,reason).valid());
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

TEST(TopicChunkPool, LegacyGlobalPoolRemainsIndependentAndForty) {
    const auto pref=unique(); Pair a(pref,"a"),b(pref,"b");
    Held old(*a.tx), old_neighbour(*b.tx), topic(*a.tx);
    EXPECT_EQ(old.take(100,false),40u);
    EXPECT_EQ(old_neighbour.take(100,false),0u);
    EXPECT_EQ(topic.take(100),kCount);
}

TEST(TopicChunkPool, SegmentAllocatesTenPayloadBlocks) {
    Pair p; auto lo=p.tx->loan_topic(100); ASSERT_TRUE(lo.valid());
    const std::size_t stride=((lo.size+16+1023)/1024)*1024;
    struct Header { ipc::id_pool<> ids; ipc::spin_lock lock; };
    const auto pref=ipc::topic_pool_prefix(ipc::make_string(p.prefix.c_str()),ipc::make_string(p.name.c_str()));
    const auto name=ipc::make_prefix(pref,{"CHUNK_INFO__",ipc::to_string(stride),"__C10"});
    ipc::shm::handle view;
    ASSERT_TRUE(view.acquire(name.c_str(),sizeof(Header)+kCount*stride,ipc::shm::open));
    // POSIX shm 段尾另有 4 字节引用计数，载荷区确为 10 个步长。
    EXPECT_EQ(view.size(),sizeof(Header)+kCount*stride+sizeof(std::atomic<std::int32_t>));
    view.release_no_unlink();
    p.tx->discard_loan(lo);
}

TEST(TopicChunkPool, SameReceiverAlternatesLegacyAndTopicStorage) {
    Pair p;
    for(int i=0;i<100;++i) {
        auto lo=(i%2) ? p.tx->loan_topic(2000) : p.tx->loan(2000);
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
    a={}; EXPECT_FALSE(p.tx->loan_topic(500).valid());
    b={}; auto recovered=p.tx->loan_topic(500);ASSERT_TRUE(recovered.valid());
    p.tx->discard_loan(recovered);
}

TEST(TopicChunkPool, BufferCanOutliveReceiverAndSenderHandle) {
    const auto pref=unique(), name=unique(); ipc::buff_t kept;
    {
        Pair p(pref,name); auto lo=p.tx->loan_topic(1000);ASSERT_TRUE(lo.valid());
        fill(lo,0x72);ASSERT_TRUE(p.tx->publish_loan(lo));
        kept=p.rx->recv(1000);ASSERT_TRUE(matches(kept,0x72));
    }
    EXPECT_TRUE(matches(kept,0x72));kept={};
    Pair reopened(pref,name);Held held(*reopened.tx);EXPECT_EQ(held.take(1000),kCount);
}

TEST(TopicChunkPool, SnifferResolvesBothPoolsWithoutRecycling) {
    Pair p; ipc::sniffer sniffer;
    ASSERT_TRUE(sniffer.open(ipc::prefix{p.prefix.c_str()},p.name.c_str()));
    sniffer.skip_to_latest();
    EXPECT_TRUE(sniffer.try_recv().empty()); // 首次读建立起点，再发布待观察消息
    for(bool topic : {false,true}) {
        auto lo=topic ? p.tx->loan_topic(2000) : p.tx->loan(2000);
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
    auto lo=tx.loan_topic(4000);ASSERT_TRUE(lo.valid());fill(lo,0x31);
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
        loans[i]=p.tx->loan_topic(8192);
    });
    while(ready.load()!=10)std::this_thread::yield();
    start.store(true,std::memory_order_release);
    for(auto& t:threads)t.join();
    std::set<void*> seen;
    for(auto& lo:loans){EXPECT_TRUE(lo.valid());EXPECT_TRUE(seen.insert(lo.data).second);}
    EXPECT_FALSE(p.tx->loan_topic(8192).valid());
    for(auto& lo:loans)p.tx->discard_loan(lo);
}

TEST(TopicChunkPool, InvalidWireIdsAndOversizeAreRejected) {
    for(auto bad : {-1,-12,std::numeric_limits<int>::min(),40,ipc::detail::topic_storage_tag})
        EXPECT_EQ(ipc::detail::storage_from_wire(bad),-1);
    for(int i=0;i<10;++i) {
        const auto id=ipc::detail::topic_storage_tag+i;
        EXPECT_LT(ipc::detail::storage_to_wire(id),0); // 旧接收器的 id < 0 守卫
        EXPECT_EQ(ipc::detail::storage_from_wire(ipc::detail::storage_to_wire(id)),id);
    }
    Pair p;ipc::loan_status reason{};
    EXPECT_FALSE(p.tx->loan_topic(std::numeric_limits<std::size_t>::max(),reason).valid());
    EXPECT_EQ(reason,ipc::loan_status::size_too_large);
}
