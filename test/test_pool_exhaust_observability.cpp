// 十块话题池的耗尽日志归因与正常循环阴性对照。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/ipc.h"
#include "libipc/memory/resource.h"

namespace {

constexpr int kChunkPoolSize = static_cast<int>(ipc::topic_msg_cache);

constexpr std::size_t kPayloadExhaust = 6144;  // 尺寸类 7168
constexpr std::size_t kPayloadCycle   = 2048;  // 尺寸类 3072

constexpr char kMarker[] = "chunk pool exhausted";
constexpr char kProbe[]  = "POOLOBS_CAPTURE_PROBE_MARKER";

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(n, fill);
}

class StderrCapture
{
public:
    StderrCapture() { testing::internal::CaptureStderr(); }
    ~StderrCapture()
    {
        if (!done_) testing::internal::GetCapturedStderr();
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string finish()
    {
        done_ = true;
        return testing::internal::GetCapturedStderr();
    }

private:
    bool done_ = false;
};

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

}   // namespace

TEST(PoolExhaustObservability, ExhaustionIsReported)
{
    const std::string name = "pool_obs_exhaust";
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

    const auto payload = make_payload(kPayloadExhaust, 0xC3);

    StderrCapture cap;
    for (int i = 0; i < kChunkPoolSize + 1; ++i)
    {
        ASSERT_TRUE(tx.send(payload.data(), payload.size()))
            << "第 " << i << " 条发送失败 —— 池空应静默降级成分片, 而不是返回失败";
    }
    const std::string err = cap.finish();

    if (!err.empty())
        std::fprintf(stdout, "[pool-obs] captured stderr:\n%s\n", err.c_str());

    EXPECT_TRUE(contains(err, kMarker))
        << "池被取空却一条报告都没有 —— 观测面失效。捕获到的 stderr:\n" << err;
    EXPECT_TRUE(contains(err, "kind = send")) << err;

    EXPECT_TRUE(contains(err, "count = 1 (本进程)")) << err;
    EXPECT_TRUE(contains(err, ("pool capacity = " +
                               std::to_string(ipc::topic_msg_cache)).c_str()))
        << err;
    EXPECT_TRUE(contains(err, "chunk_size = 7168")) << err;

    EXPECT_TRUE(contains(err, ipc::topic_pool_prefix({},ipc::make_string(name.c_str())).c_str())) << err;

    {
        const std::string name2 = "pool_obs_exhaust_2";
        ipc::route::clear_storage(name2.c_str());

        ipc::route tx2{name2.c_str(), ipc::sender};
        ipc::route rx2{name2.c_str(), ipc::receiver};   // 同样从不 recv
        ASSERT_TRUE(tx2.wait_for_recv(1, 2000)) << "第二档接收方未在超时内连上";

        const auto payload2 = make_payload(4096, 0x3C);  // calc_chunk_size(4096) = 5120

        StderrCapture cap2;
        for (int i = 0; i < kChunkPoolSize + 1; ++i)
        {
            ASSERT_TRUE(tx2.send(payload2.data(), payload2.size()))
                << "第二档第 " << i << " 条发送失败";
        }
        const std::string err2 = cap2.finish();
        if (!err2.empty())
            std::fprintf(stdout, "[pool-obs] captured stderr (2nd size class):\n%s\n",
                         err2.c_str());

        EXPECT_TRUE(contains(err2, "chunk_size = 5120"))
            << "第二档池(5120)被取空却没有报告 —— 节流把'另一个池在饿'吞掉了。"
               "捕获到的 stderr:\n" << err2;
        EXPECT_TRUE(contains(err2, "count = 1 (本进程)"))
            << "第二档的计数不是 1 —— 说明计数仍在跨档共用(旧缺陷)。stderr:\n" << err2;
        EXPECT_TRUE(contains(err2, ("pool capacity = " +
                                    std::to_string(ipc::topic_msg_cache)).c_str()))
            << err2;

        EXPECT_TRUE(contains(err2, ipc::topic_pool_prefix({},ipc::make_string(name2.c_str())).c_str())) << err2;
    }

    {
        const std::string name3 = "pool_obs_exhaust_3";
        ipc::route::clear_storage(ipc::prefix{"poolobs_a"}, name3.c_str());

        ipc::route tx3{ipc::prefix{"poolobs_a"}, name3.c_str(), ipc::sender};
        ipc::route rx3{ipc::prefix{"poolobs_a"}, name3.c_str(), ipc::receiver};  // 从不 recv
        ASSERT_TRUE(tx3.wait_for_recv(1, 2000)) << "第三档(带前缀)接收方未在超时内连上";

        const auto payload3 = make_payload(3072, 0xA5);  // calc_chunk_size(3072) = 4096

        StderrCapture cap3;
        for (int i = 0; i < kChunkPoolSize + 1; ++i)
        {
            ASSERT_TRUE(tx3.send(payload3.data(), payload3.size()))
                << "第三档第 " << i << " 条发送失败";
        }
        const std::string err3 = cap3.finish();
        if (!err3.empty())
            std::fprintf(stdout, "[pool-obs] captured stderr (prefixed pool):\n%s\n",
                         err3.c_str());

        EXPECT_TRUE(contains(err3, "chunk_size = 4096")) << err3;
        EXPECT_TRUE(contains(err3, ipc::topic_pool_prefix(ipc::make_string("poolobs_a"),ipc::make_string(name3.c_str())).c_str()))
            << "带前缀的池没有把 prefix 原样报出来 —— 归因字段不承重"
               "(硬编码空串也会绿)。stderr:\n" << err3;

        EXPECT_FALSE(contains(err3, "空前缀"))
            << "非空前缀的池却打了'空前缀'的说明 —— 点明句不是条件输出。stderr:\n" << err3;
    }

}

TEST(PoolExhaustObservability, NoReportWhenPoolCycles)
{
    const std::string name = "pool_obs_cycle";
    ipc::route::clear_storage(name.c_str());

    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000)) << "接收方未在超时内连上";

    const auto payload = make_payload(kPayloadCycle, 0x5A);

    StderrCapture cap;

    std::fprintf(stderr, "%s\n", kProbe);

    constexpr int kRounds = 8;
    for (int i = 0; i < kRounds; ++i)
    {
        ASSERT_TRUE(tx.send(payload.data(), payload.size())) << "第 " << i << " 轮发送失败";
        ipc::buff_t got = rx.recv(2000);
        ASSERT_FALSE(got.empty()) << "第 " << i << " 轮未收到";
    }   // got 每轮析构 ⇒ chunk 归还池
    const std::string err = cap.finish();

    ASSERT_TRUE(contains(err, kProbe))
        << "stderr 捕获面本身失效 —— 本用例的阴性结论不成立(不能当作'没有耗尽'):\n"
        << err;
    EXPECT_FALSE(contains(err, kMarker))
        << "池运行在容量内却报了耗尽(池可能被上一次运行/其他进程污染):\n" << err;
}
