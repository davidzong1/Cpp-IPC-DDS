// 此用例需要在真实共享队列中精确注入半条消息，公共 send() 不暴露该边界。
// Linux 下在测试翻译单元编译实现；不向产品增加测试 API 或运行期开关。
#include <gtest/gtest.h>
#if defined(__linux__)
#include "../src/libipc/ipc.cpp"
#include <string>

namespace {
using CacheTestFlag = ipc::wr<ipc::relat::single, ipc::relat::multi, ipc::trans::broadcast>;
using CacheTestTransport = detail_impl<policy_t<CacheTestFlag>>;
struct CacheRouteNames {
    std::string a, b;
    CacheRouteNames() {
        static unsigned serial=0;
        const auto suffix=std::to_string(::getpid())+"_"+std::to_string(serial++);
        a="cache_iso_a_"+suffix;b="cache_iso_b_"+suffix;
    }
    ~CacheRouteNames() {
        ipc::route::clear_storage(a.c_str());ipc::route::clear_storage(b.c_str());
    }
};
bool inject_fragment(ipc::route& sender, std::uint32_t id, std::int32_t remaining, const char* data) {
    auto* info=CacheTestTransport::info_of(sender.handle());
    auto* queue=CacheTestTransport::queue_of(sender.handle());
    const bool ok=queue->push([](void*,ipc::circ::cc_t){return true;},
                             info->cc_id_,id,remaining,data,ipc::data_length);
    if(ok) CacheTestTransport::notify_readers(info);
    return ok;
}
}

TEST(RecvFragmentIsolation, InterleavedRoutesWithSameMessageIdRemainIndependent) {
    CacheRouteNames names;
    ipc::route pa(names.a.c_str(),ipc::sender,false),ra(names.a.c_str(),ipc::receiver,false);
    ipc::route pb(names.b.c_str(),ipc::sender,false),rb(names.b.c_str(),ipc::receiver,false);
    const std::string a=std::string(64,'A')+std::string(64,'a');
    const std::string b=std::string(64,'B')+std::string(64,'b');
    ASSERT_TRUE(inject_fragment(pa,17,64,a.data()));
    EXPECT_TRUE(ra.recv(0).empty());  // A 的首片留待下次 recv
    ASSERT_TRUE(inject_fragment(pb,17,64,b.data()));
    ASSERT_TRUE(inject_fragment(pb,17,0,b.data()+64));
    auto got_b=rb.recv(0);
    ASSERT_FALSE(got_b.empty());
    EXPECT_EQ(std::string(static_cast<const char*>(got_b.data()),got_b.size()),b);
    ASSERT_TRUE(inject_fragment(pa,17,0,a.data()+64));
    auto got_a=ra.recv(0);
    ASSERT_FALSE(got_a.empty());
    EXPECT_EQ(std::string(static_cast<const char*>(got_a.data()),got_a.size()),a);
}

TEST(RecvFragmentIsolation, DisconnectingNeighbourDoesNotErasePartialMessage) {
    CacheRouteNames names;
    ipc::route pa(names.a.c_str(),ipc::sender,false),ra(names.a.c_str(),ipc::receiver,false);
    ipc::route pb(names.b.c_str(),ipc::sender,false),rb(names.b.c_str(),ipc::receiver,false);
    const std::string a=std::string(64,'X')+std::string(64,'x');
    ASSERT_TRUE(inject_fragment(pa,21,64,a.data()));
    EXPECT_TRUE(ra.recv(0).empty());
    rb.disconnect();
    ASSERT_TRUE(inject_fragment(pa,21,0,a.data()+64));
    auto got=ra.recv(0);
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(std::string(static_cast<const char*>(got.data()),got.size()),a);
}
#else
TEST(RecvFragmentIsolation, RequiresLinuxInternalQueueHarness) {
    GTEST_SKIP() << "精确分片注入工装当前仅支持 Linux";
}
#endif
