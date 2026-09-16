/* udp_discovery_port_calculate 的越界行为 (docs/shm_defect_fixes.md §3 的残留)
 *
 * 缺陷形态(修复前):
 *     uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + domain_id * hash_value;
 *     if (limited_port + kUdpPortOffsetMax > 65535)
 *         throw std::runtime_error("Calculated port number exceeds the maximum allowed value...");
 *
 * 端口值域 = base + domain_id × (fnv(topic) % 10000), 而 10000 × 2147483647 ≫ 54081, 所以
 * 越界**不是边界情况**。实测(20000+20000 个真实风格的 topic 名, 直接用生产函数):
 *
 *     domain=0..5  0%   domain=6   9.9%   domain=7  22.7%   domain=8  32.5%
 *     domain=10   46.1%  domain=32 83.4%   domain=64 91.8%   domain=100 94.8%
 *     domain=541+ 约 100%
 *
 * 危害不是"报错"而是**报错的地方**: 抛出的三处调用点全是 socket 传输层的构造函数
 * (socket_pub_sub_ipc.cc:30/456、socket_ser_cli_ipc.cc:75/537)与嗅探器
 * (socket_sniffer.cc:23、handshake_probe.h:152) —— 前两处没有任何 try/catch 能接住,
 * 也就是**建连接直接打死进程**; 后两处虽然 catch(...) 降级了, 但降级方式是"嗅探器永久
 * 看不到这个 topic", 且要用户自己去猜 topic 名有什么问题。
 *
 * 修复: 越界时把 offset 折回窗口内, 不再抛。折回用什么方式是**有约束的**:
 *   ① 必须是"凡旧实现能算出结果的输入, 结果逐位不变" —— 否则新旧版本进程绑到不同端口上,
 *      而且**双方都成功**(组播地址不受端口影响), 表现为静默互不通, 比崩溃更难查;
 *   ② 折回后的段尾仍要在 65535 内(基址合法 ≠ 段尾合法)。
 *
 * 本文件的两个承重判据:
 *   · LegacyInputsAreBitIdentical —— ①的判据。逐 (topic, domain) 把旧公式与生产函数比,
 *     凡是旧公式在区间内的必须**逐位相等**。
 *   · FoldIsNotIdentityOnIllegalInputs —— 反向的对照: 旧公式越界的输入必须真的**被改动**,
 *     否则说明修复没生效(这条同时守住"别把抛异常悄悄改成夹到 65535"这种修法 ——
 *     那样 offset ≤ 54081 的输入全被夹到同一个端口上, 是另一种串扰)。
 *
 * ⛔ 本文件不碰 wire: 只调产品导出函数, 不发包、不建 socket。
 */
#include <cstdint>
#include <limits>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/hash.h"
#include "dzIPC/dzipc.h"
#include "ipc_msg/std_msgs/std_string.hpp"
#include "libipc/udp.h"

namespace {

constexpr uint64_t kBase = UDP_DISCOVERY_BASE_PORT;
constexpr uint64_t kWindow = dzIPC::common::kUdpPortWindow;

/* ------------------------------------------------------------------------- *
 * 旧实现(修复前)的逐字转写 —— 本文件的对账基准。
 * 刻意**不**在这里引用任何新常量: 它要还原的是历史行为, 不是"新实现在旧条件下的行为"。
 * ------------------------------------------------------------------------- */
std::optional<uint16_t> old_formula(const std::string& topic_name, int domain_id)
{
    const uint64_t hash_value = static_cast<uint16_t>(dzIPC::common::fnv1a64(topic_name) % 10000);
    const uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + static_cast<uint64_t>(domain_id) * hash_value;
    if (limited_port + dzIPC::common::kUdpPortOffsetMax > 65535)
    {
        return std::nullopt;   /* 旧实现到这里就 throw */
    }
    return static_cast<uint16_t>(limited_port);
}

uint16_t now(const std::string& topic_name, int domain_id)
{
    return dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
}

/* topic 名样本: 覆盖空串/纯 ASCII/含 NUL/超长/高位字节(UTF-8)/真实风格命名。
 * 端口只依赖 fnv1a64 的字节流, 所以这些名字的**取值**才重要 —— 选它们是为了让
 * hash 落在不同区间, 不是为了"语义上像不像 topic"。 */
std::vector<std::string> topic_samples()
{
    std::vector<std::string> v{
        "",
        "a",
        "/",
        "/demo/depth_image",
        "/robot_0/camera/image",
        "sniffer_naming@depth",
        "\xE4\xB8\xAD\xE6\x96\x87topic",                       /* UTF-8 "中文topic" */
        std::string("embedded\0nul", 12),                       /* 名字里带 NUL */
        std::string(4096, 'x'),                                 /* 超长 */
    };
    for (int i = 0; i < 512; ++i)
    {
        v.push_back("/topic_" + std::to_string(i));
        v.push_back("/robot_" + std::to_string(i % 8) + "/camera/image_" + std::to_string(i));
    }
    return v;
}

/* domain 取值: 0/1 是既有测试与部署里最常用的两个; 6 是**实测第一次出现抛死**的 domain;
 * 后面一路到比 IPv6 源端选择高 32 位还大的值, 压住"大 domain 必然全抛"的形态。 */
std::vector<int> domain_samples()
{
    return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 16, 32, 64, 100, 232, 541, 1000, 10000, 100000,
            std::numeric_limits<int>::max()};
}

}   // namespace

/* ------------------------------------------------------------------------- *
 * 1. 承重判据 ①: 旧实现能算出结果的输入, 新实现逐位不变。
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, LegacyInputsAreBitIdentical)
{
    std::size_t legacy = 0;
    std::size_t illegal = 0;
    for (const auto& t : topic_samples())
    {
        for (int d : domain_samples())
        {
            const auto old = old_formula(t, d);
            const uint16_t fresh = now(t, d);
            if (old.has_value())
            {
                ++legacy;
                /* 逐位相等 —— 不是"落在附近"、不是"差不大于 1"。改一位就是改端口,
                 * 就是新旧进程静默互不通。 */
                ASSERT_EQ(fresh, *old) << "topic='" << t << "' domain=" << d
                                       << ": 修复改了既有合法输入的端口 — 这会破坏新旧版本互通";
            }
            else
            {
                ++illegal;
            }
        }
    }
    /* 样本必须**同时**覆盖两类, 否则上面那个循环可能一条都没验到。 */
    EXPECT_GT(legacy, 0u) << "样本里没有一个旧实现能算出的输入, 判据空转";
    EXPECT_GT(illegal, 0u) << "样本里没有一个旧实现抛死的输入, 那这条判据就只是回归测试";
}

/* ------------------------------------------------------------------------- *
 * 2. 承重判据 ②: 旧实现抛死的输入, 新实现**不抛**, 且结果落在窗口内的确定位置。
 *
 * 同时守住反向: 修复必须真的改动这些输入。把 throw 换成 min(offset, window-1) 之类的
 * "夹取"能让上面这条也过, 但会把 54081 以上的所有 offset 压到同一个端口上 —— 那是把
 * "崩"换成"静默串扰"。折回必须保持窗口内尽可能的分散度。
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, FoldIsNotIdentityOnIllegalInputs)
{
    std::size_t illegal = 0;
    std::size_t changed = 0;
    for (const auto& t : topic_samples())
    {
        for (int d : domain_samples())
        {
            if (old_formula(t, d).has_value())
            {
                continue;   /* 只审旧实现抛死的那一类 */
            }
            ++illegal;
            const uint64_t h = dzIPC::common::fnv1a64(t) % 10000;
            const uint64_t offset = static_cast<uint64_t>(d) * h;
            /* 不抛 —— 直接调即可, 抛异常会让整个用例红。 */
            const uint16_t p = now(t, d);
            ASSERT_GE(p, kBase);
            ASSERT_LE(static_cast<uint64_t>(p), kBase + kWindow - 1) << "折回后基址越出合法窗口";
            /* 落在 offset % window 这个确定位置上, 而不是被夹到窗口顶端。 */
            EXPECT_EQ(p, kBase + offset % kWindow) << "topic='" << t << "' domain=" << d;
            if (offset % kWindow != kWindow - 1)
            {
                ++changed;   /* 与"全夹到上界"的修法在行为上分开: 不落在同一点 */
            }
        }
    }
    EXPECT_GT(illegal, 0u);
    /* 抛死样本里, 绝大多数折回后**互不相同** —— 若这条塌了, 说明折回退化成夹取。 */
    EXPECT_GT(changed, illegal / 2)
        << "越界输入的折回结果过于集中(" << changed << "/" << illegal
        << " 落在非上界点), 疑似退化成夹取 —— 那会把崩溃换成静默串扰";
}

/* ------------------------------------------------------------------------- *
 * 3. 段尾必须放得下 ACK 通道 —— 基址合法 **不等于** 段尾合法。
 *    (旧实现的上界校验本意就是这条, 修复不能把它一起丢了。)
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, WholeSegmentFitsBelow65535)
{
    for (const auto& t : topic_samples())
    {
        for (int d : domain_samples())
        {
            const uint16_t base = now(t, d);
            const uint32_t last = static_cast<uint32_t>(base) + dzIPC::common::kUdpPortOffsetMax;
            ASSERT_LE(last, 65535u) << "topic='" << t << "' domain=" << d << ": 基址 " << base
                                    << " 的段尾 " << last << " 越界 —— ACK 通道会静默回绕到低端口";
        }
    }
}

/* ------------------------------------------------------------------------- *
 * 4. 边界与不变式
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, WindowEdgesAndInvariants)
{
    /* 窗口上界确实是"段尾刚好贴住 65535": 不是随便挑的 65531。 */
    EXPECT_EQ(static_cast<uint32_t>(kBase) + kWindow - 1 + dzIPC::common::kUdpPortOffsetMax, 65535u);

    /* 取模能当"修复而不改映射"用, 靠的是旧实现在 offset ≥ window 时才抛 —— 即
     * 合法输入的 offset 恒 < window。这里把那个前提本身钉住。 */
    for (const auto& t : topic_samples())
    {
        for (int d : domain_samples())
        {
            if (const auto old = old_formula(t, d))
            {
                const uint64_t offset = static_cast<uint64_t>(d) * (dzIPC::common::fnv1a64(t) % 10000);
                ASSERT_LT(offset, kWindow) << "旧实现返回了结果, 但 offset ≥ 窗口 —— 取模的前提不成立";
                ASSERT_EQ(*old, kBase + offset);
            }
        }
    }

    /* gcd(10000, window) == 1: 折回不是把 offset 空间压扁成子群 —— 这是 §2 判据的
     * 数学前提, 塌了它上面那些"没变"的结论都只是样本碰巧。 */
    EXPECT_EQ(std::gcd<uint64_t>(10000, kWindow), 1u);
}

/* ------------------------------------------------------------------------- *
 * 5. 反向对照: 旧实现真的会抛 —— 证明上面那些用例里"illegal"这一类不是空集,
 *    也把缺陷本身固定在文档与测试里(这条修复后应当**保持**通过: 它测的是旧公式,
 *    不是产品函数)。
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, OldFormulaReallyThrewOnTheseInputs)
{
    /* lambda 内直接写旧公式并抛, 不经过 old_formula 的 nullopt 通道 ——
     * 免得哪天有人把 old_formula 改成"不抛", 这条对照就一起失效了。 */
    const auto legacy_throws = [](const std::string& topic_name, int domain_id) {
        const uint64_t hash_value = static_cast<uint16_t>(dzIPC::common::fnv1a64(topic_name) % 10000);
        const uint64_t limited_port = UDP_DISCOVERY_BASE_PORT + static_cast<uint64_t>(domain_id) * hash_value;
        if (limited_port + dzIPC::common::kUdpPortOffsetMax > 65535)
        {
            throw std::runtime_error("port out of range");
        }
        return static_cast<uint16_t>(limited_port);
    };

    /* 这条名字是**实测**在 domain=6 就抛死的: h=9444 (offset=56664 > 窗口 54081)。
     * 不用 /topic_0 —— 它 h=2294, 到 domain=6 只有 offset=13764, 照样能算出结果。 */
    EXPECT_THROW(legacy_throws("/robot_0/camera/image_0", 6), std::runtime_error);
    /* 修复后的产品函数对同一个输入必须不抛。 */
    EXPECT_NO_THROW(now("/robot_0/camera/image_0", 6));

    /* 而 domain ≤ 5 时旧公式对样本里的名字都不抛 —— 这正是"0% 抛死"的那一段,
     * 也是新旧实现必须逐位一致的那一段。 */
    EXPECT_NO_THROW(legacy_throws("/topic_0", 5));
    EXPECT_EQ(now("/topic_0", 5), legacy_throws("/topic_0", 5));
}

/* ------------------------------------------------------------------------- *
 * 5b. 负 domain: 只有 int 窄化能产生(公共 API 的 domain_id 是 size_t)。
 *
 * 旧实现在这一支上是**两套坏行为**: 多数输入无符号回绕后越界 ⇒ 抛; 少数恰好回绕到
 * 区间内 ⇒ 静默返回一个**低于基址**的端口(domain=-1 时恒等于 11451-hash, 甚至能到 0)。
 * 修复后一律折回窗口: 确定、在界内、不抛 —— 这里如实钉住"与旧行为不同"这件事,
 * 而不是假装它是旧合法输入。
 * ------------------------------------------------------------------------- */
TEST(UdpPortBoundary, NegativeDomainFoldsInsteadOfWrappingBelowBase)
{
    for (int d : {-1, -2, -3, -1000, std::numeric_limits<int>::min()})
    {
        for (const auto& t : topic_samples())
        {
            uint16_t p = 0;
            ASSERT_NO_THROW(p = now(t, d)) << "topic='" << t << "' domain=" << d;
            EXPECT_GE(p, kBase) << "topic='" << t << "' domain=" << d << ": 落了基址以下的端口";
            EXPECT_LE(static_cast<uint32_t>(p) + dzIPC::common::kUdpPortOffsetMax, 65535u);
            /* 确定性: 同输入同输出(不能是靠未初始化/时间戳凑出来的值)。 */
            EXPECT_EQ(now(t, d), p);
        }
    }
}

/* ------------------------------------------------------------------------- *
 * 6. 回归: 既有部署/测试用到的具体取值。
 *
 * 这些字面量是**跨版本契约**: 改了它们等于改了寻址, 与旧进程静默互不通。
 * 取值全部录自**修复前**的 libipc(build/lib 里的旧库, 直接打表得到), 不是本实现
 * 现算现抄 —— 否则焊死的是"实现与自己一致", 不是"实现与历史一致"。
 *
 * 窗口上界是 54081(=65531-11450), 所以 h ≥ 9014 的名字在 domain=6 就已经抛死:
 * 表里 /robot_0/camera/image_0(h=9444) 与 recv_cap_probe(h=8103) 的上界分别为
 * domain 5 与 6。 */
TEST(UdpPortBoundary, KnownDeploymentsUnchanged)
{
    struct Golden
    {
        const char* topic;
        int domain;
        uint16_t port;   /* 修复前的实际返回值 */
    };
    const Golden kGolden[] = {
        {"/demo/depth_image",        0, 11451},
        {"/demo/depth_image",        1, 14668},
        {"/demo/depth_image",        2, 17885},
        {"/demo/depth_image",        5, 27536},
        {"/demo/depth_image",        6, 30753},   /* 已接近上界但仍合法 —— 修复不得动它 */
        {"/topic_0",                 1, 13745},
        {"iso_udp_topic_A",          1, 12553},
        {"iso_udp_topic_B",          1, 14342},
        {"endpoint_split_self",      0, 11451},
        {"endpoint_split_self",      1, 19445},
        {"endpoint_split_self",      6, 59415},
        {"reliable_crc_probe",       1, 17902},
        {"reliable_crc_probe",       6, 50157},
        {"handshake_probe_topic",    1, 12122},
        {"recv_cap_probe",           0, 11451},
        {"recv_cap_probe",           1, 19554},
        {"recv_cap_probe",           6, 60069},
        {"",                         1, 17488},
    };
    for (const auto& g : kGolden)
    {
        EXPECT_EQ(now(g.topic, g.domain), g.port)
            << "topic='" << g.topic << "' domain=" << g.domain
            << " 的端口与修复前不一致 —— 新旧版本进程会各自绑到不同端口, 且双方都成功(静默互不通)";
    }

    /* 窗口边界的**两侧**(名字由 hash 反查得到, 值录自修复前的旧库):
     *   g16052: h=6760, domain=8 ⇒ offset 54080 = window-1 ⇒ 旧库返回 65531 = 基址上界。
     *           这是旧实现能返回的**最大**基址, 再大一位就抛。修复后必须仍是 65531。
     *   g4030 : h=6761, domain=8 ⇒ offset 54088 ⇒ 旧库抛死 ⇒ 修复后折回 54088-54081=7。
     * 这一对是"取模只在越界时起作用"的现场证据: 边界内侧一位不动, 外侧才折。 */
    ASSERT_EQ(old_formula("g16052", 8), std::optional<uint16_t>(65531));
    EXPECT_EQ(now("g16052", 8), 65531) << "窗口内侧的输入被改动了 —— 取模不再恒等";
    ASSERT_FALSE(old_formula("g4030", 8).has_value());
    EXPECT_EQ(now("g4030", 8), kBase + 7) << "越界输入的折回位置不对";

    /* domain=6 上的同一对(实测 domain=6 首个抛死的 hash 就是 9014)。 */
    ASSERT_EQ(old_formula("g870", 6), std::optional<uint16_t>(65529));
    EXPECT_EQ(now("g870", 6), 65529);
    ASSERT_FALSE(old_formula("g25786", 6).has_value());
    EXPECT_EQ(now("g25786", 6), kBase + 3);

    /* domain=0 时公式第二项恒为 0 ⇒ 端口与 topic 无关(这是已登记的另一条缺陷, 未在本次
     * 修复范围内): 本用例只钉住"没被我改"。 */
    EXPECT_EQ(now("/demo/depth_image", 0), kBase);
    EXPECT_EQ(now("", 0), kBase);
}

/* ------------------------------------------------------------------------- *
 * 7. 端到端: 一个**修复前必然打死进程**的 (topic, domain) 现在真的能通信。
 *
 * 这是"抛异常"这件事的现场判据, 前 6 个用例都只是函数级的。旧实现在
 * socket_pub_ipc 的构造函数里调本函数(socket_pub_sub_ipc.cc:30), 没有任何 catch ——
 * 所以**修复前这个用例跑不起来**: 进程直接终止, 用例红在"崩"而不是"断言失败"。
 *
 * 组播不可用的环境(容器/无网络)会 skip: 本用例的判据是"链路真的通", 不能降级成
 * "构造函数没抛就算过" —— 那样就变成自证了。
 * ------------------------------------------------------------------------- */
namespace {

constexpr std::uint32_t kE2eMsgId = 0;   /* 默认值, 与真实部署一致 */

bool multicast_available(const std::string& topic, int domain_id)
{
    const std::string group = dzIPC::common::udp_discovery_addr_calculate(topic);
    ipc::socket::UDPNode probe(topic.c_str(), group.c_str(),
                               dzIPC::common::udp_discovery_port_calculate(topic, domain_id),
                               ipc::socket::NodeRole::RecvOnly);
    return probe.connect();
}

}   // namespace

TEST(UdpPortBoundary, PreviouslyFatalTopicNowConnectsEndToEnd)
{
    using namespace std::chrono_literals;

    /* h=9014 ⇒ offset = 6×9014 = 54084 > 窗口 54081 ⇒ 旧实现抛死。 */
    const std::string topic = "g25786";
    constexpr std::size_t kDomain = 6;
    ASSERT_EQ(dzIPC::common::fnv1a64(topic) % 10000, 9014u);
    ASSERT_FALSE(old_formula(topic, static_cast<int>(kDomain)).has_value())
        << "前提没了: 这条 topic/domain 在修复前是能算出结果的, 本用例就失去判据力";

    if (!multicast_available(topic, static_cast<int>(kDomain)))
    {
        GTEST_SKIP() << "UDP multicast unavailable in this environment";
    }

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kE2eMsgId);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kE2eMsgId);
    auto pub = dzIPC::PublisherIPCPtrMake(pub_td, topic, kDomain, dzIPC::IPC_SOCKET, false);
    auto sub = dzIPC::SubscriberIPCPtrMake(sub_td, topic, kDomain, 32, dzIPC::IPC_SOCKET, false);
    pub->InitChannel("port_boundary");
    sub->InitChannel("port_boundary");
    std::this_thread::sleep_for(1500ms);   /* 等握手 */

    std::atomic<int> received{0};
    std::atomic<bool> run{true};
    std::thread reader([&] {
        auto rcv = dzIPC::TopicDataPtrMake<dzIPC::Msg::StdString>(kE2eMsgId);
        while (run.load(std::memory_order_acquire))
        {
            if (sub->try_get_clone(rcv))
            {
                received.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(1ms);
        }
    });

    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "folded_port_payload";
    msg->set_msg_id(kE2eMsgId);
    int sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        pub->publish(msg);
        ++sent;
        std::this_thread::sleep_for(4ms);
    }
    run.store(false, std::memory_order_release);
    reader.join();

    EXPECT_GT(received.load(), 0)
        << "发了 " << sent << " 条一条都没到 —— 折回后的端口两端没对上(修复前这里是构造即崩)";
}
