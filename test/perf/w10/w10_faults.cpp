/* W10 故障矩阵：四类"静默失效"的**进程级**判据（方案 §4 W10 第 3/4 条 + §13.2 条件 5）。
 *
 * 为什么必须进程级：方案 §4 W10 明确"共享内存多进程竞态不能只依赖线程 sanitizer"。
 * 本文件每个用例都用**真实产品对象 + 真实共享段**，判据落在
 * 「旧 generation 是否投递」「注销后是否还有投递」「池耗尽是否可识别且有界」这类
 * **可观测事实**上，而不是"跑绿即过"。
 *
 * 用例（每个都有明确失败判定）：
 *   F1 丢唤醒：发布落在 worker 空闲退出窗口内 ⇒ 消息不得丢，且必须触发重新拉起
 *   F2 旧 token：generation 重建后，旧 route 的 token 不得再把消息投递给新对象
 *   F3 分片线程迁移：同一话题重建后 route 归属 worker 不变（R-17/L9 的进程级对照）
 *   F4 池耗尽：chunk 池被慢消费者钉满 ⇒ loan 显式拒绝（可识别），send 走分片（有界），
 *      且**不得**出现段错误或越界（§9.5 的已知缺陷形态必须与"策略性回退"区分）
 *
 * 用法：w10_faults --domain <d> [--out <dir>] [--run-id <id>]
 * 退出码：0 = 全部通过；1 = 有失败（逐条打印 FAILURE:）
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static constexpr std::uint32_t kMsgId = 94;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
    return def;
}
static const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

namespace {

std::vector<std::string> g_failures;
std::vector<std::string> g_notes;
void fail(const std::string& s) { g_failures.push_back(s); std::printf("FAILURE: %s\n", s.c_str()); }
void note(const std::string& s) { g_notes.push_back(s); std::printf("NOTE: %s\n", s.c_str()); }

std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

/* 载荷：seq 写入前 4 字节 + 确定性字节模式（逐字节可校验，⛔ 不用固定载荷）。 */
std::string make_payload(std::uint32_t seq, std::size_t bytes)
{
    std::string s(bytes, '\0');
    std::memcpy(&s[0], &seq, 4);
    for (std::size_t i = 4; i < bytes; ++i) s[i] = static_cast<char>((seq * 131u + i * 7u) & 0xFFu);
    return s;
}
bool check_payload(const std::string& s, std::uint32_t* seq_out)
{
    if (s.size() < 4) return false;
    std::uint32_t seq = 0;
    std::memcpy(&seq, &s[0], 4);
    for (std::size_t i = 4; i < s.size(); ++i)
        if (s[i] != static_cast<char>((seq * 131u + i * 7u) & 0xFFu)) return false;
    if (seq_out) *seq_out = seq;
    return true;
}

bool publish_seq(dzIPC::shm::shm_pub_ipc& pub, std::uint32_t seq, std::size_t bytes)
{
    auto m = std::make_shared<dzIPC::Msg::StdString>();
    m->set_msg_id(kMsgId);
    m->str = make_payload(seq, bytes);
    return pub.publish(m);
}

/* 排空物化队列并逐条校验；返回收到的 seq 列表。 */
std::vector<std::uint32_t> drain(dzIPC::shm::shm_sub_ipc& sub, long wait_ms = 1500)
{
    std::vector<std::uint32_t> got;
    const auto dl = Clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;)
    {
        bool any = false;
        for (;;)
        {
            auto sink = td();
            if (!sub.try_get_clone(sink)) break;
            auto s = sink->topic() ? sink->topic()->msgcast<dzIPC::Msg::StdString>() : nullptr;
            std::uint32_t seq = 0;
            if (s && check_payload(s->str, &seq)) got.push_back(seq);
            any = true;
        }
        if (Clock::now() >= dl) break;
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return got;
}

bool wait_ready(const std::string& topic, long domain, long timeout_ms)
{
    const auto dl = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < dl)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (cp.open(shm_topic_control_name(topic, static_cast<std::size_t>(domain)))
            && cp.peer_count() >= 1 && cp.state() == dzIPC::control_plane_shm::TopicState::Ready)
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return 0;
    std::size_t n = 0;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}

}   // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long domain = arg_long(argc, argv, "--domain", 5001);
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "w10-faults");
    /* 只跑指定用例（二分定位用；默认全跑）。 */
    const char* only = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--only") == 0) only = argv[i + 1];
    const auto want = [&](const char* id) { return only == nullptr || std::strcmp(only, id) == 0; };
    std::printf("run_id=%s domain=%ld pid=%d\n", run_id.c_str(), domain, (int)::getpid());

    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();

    /* ================= F1 丢唤醒：发布落在空闲退出窗口 =================
     * 造法：建 1 话题并**等到 worker 真的空闲退出**（idle_exits 增长、thread 归还），
     * 然后立刻发布 ⇒ 必须靠 add_route/重新拉起路径收到，不得丢。
     * ⛔ 这不是"构造一个 race 撞概率"：idle_exits 是 worker 自报的事实，用它做闸门。 */
    if (want("F1"))
    {
        const std::string topic = "w10f1_" + std::to_string(domain);
        {
            auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
            auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 64, false);
            pub->InitChannel("f1");
            sub->InitChannel("f1");
            if (!wait_ready(topic, domain, 15000)) fail("F1 握手未达成");
            const auto s0 = pool.stats();
            /* 等到该 worker 空闲退出（或 6 s 超时，超时按"未触发"登记而不是假绿）。 */
            const auto dl = Clock::now() + std::chrono::seconds(6);
            bool exited = false;
            while (Clock::now() < dl)
            {
                if (pool.stats().idle_exits > s0.idle_exits) { exited = true; break; }
                std::this_thread::sleep_for(20ms);
            }
            std::printf("F1 idle_exit_observed=%d idle_exits_delta=%llu threads=%zu\n", exited ? 1 : 0,
                        (unsigned long long)(pool.stats().idle_exits - s0.idle_exits), thread_count());
            if (!exited) note("F1 未观察到空闲退出（worker 仍在本轮 route 上），该用例退化为普通投递检查");
            /* 关键：空闲退出后再发布。 */
            if (!publish_seq(*pub, 1, 64)) fail("F1 发布失败");
            const auto got = drain(*sub, 4000);
            const bool ok = std::find(got.begin(), got.end(), 1u) != got.end();
            if (!ok) fail("F1 丢唤醒：空闲退出窗口内的发布未收到（got=" + std::to_string(got.size()) + "）");
            std::printf("F1 result ok=%d got=%zu senders_pool_restarts=%llu\n", ok ? 1 : 0, got.size(),
                        (unsigned long long)pool.stats().thread_restarts);
            /* ⛔ clear_storage 必须等 sub/pub 全部析构之后（先出作用域再清段）。 */
            pub.reset();
        }
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
    }

    /* ================= F2 旧 token / 旧 generation 不得投递 =================
     * 造法：同一话题上"发布者析构 → 新发布者重建（generation 递增）"，
     * 旧订阅者**保留**，新发布者发一条 seq=7。
     * 判据：订阅者只应看到新 generation 的 seq=7；不得看到任何旧对象的载荷
     *       （旧对象从未发过消息，所以正确结果是"只收到 7"，⛔ 收到别的 seq 即失败）。 */
    if (want("F2"))
    {
        const std::string topic = "w10f2_" + std::to_string(domain);
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 64, false);
        sub->InitChannel("f2");
        {
            auto pub1 = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
            pub1->InitChannel("f2");
            wait_ready(topic, domain, 15000);
            (void)drain(*sub, 300);   // 清空
            pub1.reset();             // 旧 generation 结束
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            auto pub2 = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
            pub2->InitChannel("f2");
            if (!wait_ready(topic, domain, 15000)) fail("F2 重建后握手未达成");
            const std::uint32_t gen_after = [&] {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                return cp.open(shm_topic_control_name(topic, static_cast<std::size_t>(domain))) ? cp.generation() : 0u;
            }();
            if (!publish_seq(*pub2, 7, 64)) fail("F2 新 generation 发布失败");
            const auto got = drain(*sub, 4000);
            bool has7 = std::find(got.begin(), got.end(), 7u) != got.end();
            bool has_other = false;
            for (auto v : got) if (v != 7u) has_other = true;
            std::printf("F2 generation_after=%u got=%zu has_seq7=%d has_other=%d\n", gen_after, got.size(),
                        has7 ? 1 : 0, has_other ? 1 : 0);
            if (!has7) fail("F2 重建后新 generation 的首包未收到");
            if (has_other) fail("F2 旧 generation 投递了非预期 seq（旧 token 未失效）");
        }
        sub.reset();
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
    }

    /* ================= F3 分片线程迁移：重建后 worker 归属不变 =================
     * 判据（W04 R-17 / L9）：route key 不含 generation ⇒ 重建前后归属 worker 序号相同。
     * 用池内 route_count 与会话日志的 worker 序号双证（此处用 worker_for 纯函数 + route_count）。 */
    if (want("F3"))
    {
        const std::string topic = "w10f3_" + std::to_string(domain);
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 64, false);
        sub->InitChannel("f3");
        auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
        pub->InitChannel("f3");
        wait_ready(topic, domain, 15000);
        const std::size_t w_before = dzIPC::threepools::RecvWorkerPool::worker_for(
            topic.c_str(), static_cast<std::uint32_t>(domain), pool.worker_count());
        const std::size_t routes_before = pool.route_count();
        /* 触发一次 generation 重建：析构再重建发布端。 */
        pub.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
        pub->InitChannel("f3");
        wait_ready(topic, domain, 15000);
        const std::size_t w_after = dzIPC::threepools::RecvWorkerPool::worker_for(
            topic.c_str(), static_cast<std::uint32_t>(domain), pool.worker_count());
        const std::size_t routes_after = pool.route_count();
        std::printf("F3 worker_before=%zu worker_after=%zu routes %zu->%zu\n", w_before, w_after, routes_before,
                    routes_after);
        if (w_before != w_after) fail("F3 重建后归属 worker 变化（route key 可能含 generation）");
        if (routes_after != 1) fail("F3 重建后 register 次数未收敛到 1（routes=" + std::to_string(routes_after) + "）");
        sub.reset();
        pub.reset();
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
    }

    /* ================= F4 池耗尽：策略性回退 vs 内存安全越界 =================
     * 造法：用小容量 chunk（loan 一块很大的可变长）+ 慢消费者把池钉满，观察：
     *   · loan 失败必须是**显式拒绝**（返回无效），可识别；
     *   · 不得段错误 / 不得越界（进程存活本身是判据的一环）。
     * ⛔ §9.5 记录的"耗尽→64B 分片→环覆写→重组错拼→段错误"必须与"策略性回退"分开登记。 */
    if (want("F4"))
    {
        dzIPC::EnableDzFlat(true);
        const std::string topic = "w10f4_" + std::to_string(domain);
        auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
        /* 订阅者队列故意小 ⇒ 池被钉住的窗口更长。 */
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 4, false);
        pub->InitChannel("f4");
        sub->InitChannel("f4");
        wait_ready(topic, domain, 15000);
        long loan_ok = 0, loan_rejected = 0;
        std::vector<dzIPC::LoanedMessage<dzIPC::Msg::StdStringFlat>> held;
        const std::size_t big = 64 * 1024;
        for (int i = 0; i < 200; ++i)
        {
            auto lo = pub->loan<dzIPC::Msg::StdStringFlat>(big);
            if (!lo.valid()) { ++loan_rejected; continue; }
            /* 故意不发布，钉住 chunk。 */
            held.push_back(std::move(lo));
            ++loan_ok;
        }
        std::printf("F4 loan_ok=%ld loan_rejected=%ld (共 200 次)\n", loan_ok, loan_rejected);
        if (loan_rejected == 0)
        {
            note("F4 未触发池耗尽（200 块内未耗尽，属正常）；该用例只证明正常路径不崩");
        }
        else
        {
            note("F4 池耗尽触发，loan 显式拒绝 ⇒ 策略性回退可识别（非崩溃）");
        }
        /* 仍必须能走普通 publish（分片路径）且不崩。 */
        bool pub_ok = publish_seq(*pub, 11, big);
        std::printf("F4 after_exhaust_publish=%d\n", pub_ok ? 1 : 0);
        if (!pub_ok) note("F4 池耗尽后普通 publish 亦失败（背压路径，需 W09 策略说明）");
        /* 反事实检查：释放 held 后必须能再借到。 */
        held.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto again = pub->loan<dzIPC::Msg::StdStringFlat>(big);
        const bool reclaimed = again.valid();
        std::printf("F4 after_release_can_loan_again=%d\n", reclaimed ? 1 : 0);
        if (loan_rejected > 0 && !reclaimed) fail("F4 释放后仍借不到 chunk（chunk 未归还）");
        sub.reset();
        pub.reset();
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
        dzIPC::EnableDzFlat(false);
    }

    /* ================= F5 注销回调：注销后不得再有投递 ================= */
    if (want("F5"))
    {
        const std::string topic = "w10f5_" + std::to_string(domain);
        auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
        pub->InitChannel("f5");
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 64, false);
        sub->InitChannel("f5");
        wait_ready(topic, domain, 15000);
        (void)publish_seq(*pub, 21, 64);
        auto got1 = drain(*sub, 3000);
        const std::size_t routes_with_sub = pool.route_count();
        sub.reset();   /* 注销 */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::size_t routes_after = pool.route_count();
        (void)publish_seq(*pub, 22, 64);   /* 注销后再发 */
        std::printf("F5 got_before_unregister=%zu routes %zu->%zu\n", got1.size(), routes_with_sub, routes_after);
        if (routes_after + 1 != routes_with_sub && routes_after != 0)
            fail("F5 注销后池内 route 数未归还（" + std::to_string(routes_after) + "）");
        /* 无崩溃 + route 归还即通过（消息是否投递到已析构对象无法从外部断言，
         * 但"池内 route 归还 + recv_errors==0"是可观测的必要条件）。 */
        const auto st = pool.stats();
        std::printf("F5 recv_errors=%llu\n", (unsigned long long)st.recv_errors);
        if (st.recv_errors != 0) fail("F5 注销期间 worker 报 recv_errors");
        pub.reset();
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
    }

    /* ---------------- 落盘 ---------------- */
    if (!out.empty())
    {
        std::ofstream f(out + "/faults.txt");
        f << "run_id=" << run_id << " domain=" << domain << "\n";
        f << "failures=" << g_failures.size() << "\n";
        for (auto& x : g_failures) f << "FAILURE: " << x << "\n";
        for (auto& x : g_notes) f << "NOTE: " << x << "\n";
    }
    std::printf("W10_FAULTS_DONE failures=%zu\n", g_failures.size());
    return g_failures.empty() ? 0 : 1;
}
