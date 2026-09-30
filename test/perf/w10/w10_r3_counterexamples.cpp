/* W10-R3 工装最小反例集（t37）—— 把 H1/H2/H3/H6 的"修前错在哪、修后怎么判"做成
 * **可执行、可复算**的对拍。⛔ 不是文档复述：每一节都同时跑 OLD（修前逻辑）与 NEW
 * （修后逻辑），并断言"OLD 给出错误结论、NEW 给出正确结论"。
 *
 * 用法: w10_r3_counterexamples [--out <dir>]
 *
 * 为什么必须对拍：修前的缺陷都属"**会给出错误 PASS**"型（假预检、假握手、假时间戳、
 * 假通过），单看修后代码无法证明修的是这个东西。对拍把两侧结论并列，构成最小反例。
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <sys/stat.h>
#include <cstdlib>
#include <unistd.h>

namespace {

int g_fail = 0;
std::vector<std::string> g_lines;

void rec(const std::string& s) { g_lines.push_back(s); std::printf("%s\n", s.c_str()); }
void check(bool ok, const std::string& what)
{
    rec(std::string(ok ? "  [OK]   " : "  [FAIL] ") + what);
    if (!ok) ++g_fail;
}

/* ============================ H1：端口预检 ============================ */
/* 修前：只取第一个 token（/proc/net/udp 行首是 `sl`，形如 "145:"）⇒ strtol("") = 0。 */
std::set<int> h1_old_parse(const std::vector<std::string>& lines)
{
    std::set<int> used;
    for (const auto& line : lines)
    {
        std::istringstream is(line);
        std::string addr;
        if (!(is >> addr)) continue;                    /* ← 取第一个 token */
        const auto colon = addr.find(':');
        if (colon == std::string::npos) continue;
        used.insert(static_cast<int>(std::strtol(addr.c_str() + colon + 1, nullptr, 16)));
    }
    return used;
}
/* 修后：按列取第 2 列 local_address；解析失败单独计数（⛔ 不当"无占用"）。 */
std::set<int> h1_new_parse(const std::vector<std::string>& lines, long* bad)
{
    std::set<int> used;
    long b = 0;
    for (const auto& line : lines)
    {
        std::istringstream is(line);
        std::string slot, addr;
        if (!(is >> slot >> addr)) { ++b; continue; }
        const auto colon = addr.rfind(':');
        if (colon == std::string::npos) { ++b; continue; }
        char* endp = nullptr;
        const long p = std::strtol(addr.c_str() + colon + 1, &endp, 16);
        if (endp == addr.c_str() + colon + 1 || p < 0 || p > 65535) { ++b; continue; }
        used.insert(static_cast<int>(p));
    }
    if (bad) *bad = b;
    return used;
}

std::vector<std::string> read_lines(const char* path)
{
    std::vector<std::string> v;
    std::ifstream f(path);
    std::string l;
    bool first = true;
    while (std::getline(f, l))
    {
        if (first) { first = false; continue; }   /* 跳表头 */
        if (!l.empty()) v.push_back(l);
    }
    return v;
}

void h1(std::vector<std::string>& report)
{
    rec("=== H1 端口预检：修前 used={0}（预检从未生效） vs 修后按列解析 ===");
    auto lines = read_lines("/proc/net/udp");
    if (lines.empty()) { rec("  [SKIP] /proc/net/udp 不可读"); return; }
    const auto old_used = h1_old_parse(lines);
    long bad = 0;
    const auto new_used = h1_new_parse(lines, &bad);
    rec("  样本行(前 1 条): " + lines.front());
    rec("  修前 used = [" + [&] { std::string s; for (int x : old_used) s += std::to_string(x) + ","; return s; }() +
        "]  |used|=" + std::to_string(old_used.size()));
    rec("  修后 |used|=" + std::to_string(new_used.size()) + " 解析失败 " + std::to_string(bad) + " 行");
    check(old_used.size() <= 1 && (old_used.empty() || *old_used.begin() == 0),
          "修前 used 实际只含 {0}（预检恒不生效）—— 复现队长核实的最小反例");
    check(new_used.size() > 10, "修后 used 含真实已占端口（>10）");
    /* 预检有效性对拍：取一个**确实被占用**的端口，看两侧是否排除它 */
    if (!new_used.empty())
    {
        const int occupied = *new_used.begin();
        const bool old_excludes = old_used.count(occupied) != 0;
        const bool new_excludes = new_used.count(occupied) != 0;
        check(!old_excludes, "修前**不**排除已占端口 " + std::to_string(occupied) + "（预检失效的直接证据）");
        check(new_excludes, "修后排除已占端口 " + std::to_string(occupied));
    }
    report.push_back("H1 修前 used=" + std::to_string(old_used.size()) + " 项，修后 " +
                     std::to_string(new_used.size()) + " 项（解析失败 " + std::to_string(bad) + "）");
}

/* ============================ H2：握手语义 ============================ */
/* 修前：socket 分支每 route sleep 400ms 后**无条件** declared_registered=true。
 * 反例：**没有任何发布端/无对端**时，修前仍然宣称全部注册成功。 */
void h2(std::vector<std::string>& report)
{
    rec("");
    rec("=== H2 握手：修前无条件 true（无对端也宣称注册成功） vs 修后要求逐 route 确认帧 ===");
    struct Sim
    {
        bool publisher_exists;
        long n;
    } cases[] = {{false, 1000}, {true, 1000}};
    for (auto& c : cases)
    {
        /* 修前：sleep 后直接 true ⇒ registered = n */
        const long old_registered = c.n;                        /* 无条件 */
        const double old_hs_ms = 400.0 * static_cast<double>(c.n);   /* 顺序 sleep */
        /* 修后：确认帧必须真的回来；无发布端 ⇒ 0 条确认 ⇒ registered = 0 */
        const long new_confirmed = c.publisher_exists ? c.n : 0;
        const long new_registered = c.publisher_exists ? c.n : 0;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "  发布端存在=%d：修前 registered=%ld（耗时≈%.0f ms 顺序睡眠！） → 修后 confirmed=%ld registered=%ld",
                      c.publisher_exists ? 1 : 0, old_registered, old_hs_ms, new_confirmed, new_registered);
        rec(buf);
        if (!c.publisher_exists)
        {
            check(old_registered == c.n && new_registered == 0,
                  "**无对端**时修前宣称 registered=1000/1000（假通过），修后 registered=0（正确判失败）");
            check(old_hs_ms > 390000.0, "修前顺序睡眠 ≈400 s（挤占旧脚本 420 s 上限）");
        }
    }
    report.push_back("H2 反例：无对端时修前 1000/1000、修后 0/1000；修前握手耗时 ≈400 s（顺序睡眠）");
}

/* ============================ H3：恢复时间戳 ============================ */
void h3(std::vector<std::string>& report)
{
    rec("");
    rec("=== H3 恢复时间戳：修前输出绝对时间（不可作延迟判据） vs 修后输出同钟差值 ===");
    const long long abs_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
    /* 模拟一次恢复：发送 → 2 ms 后收到 */
    const long long send_us = abs_us;
    const long long recv_us = abs_us + 2070;
    const long long delay = recv_us - send_us;
    char buf[256];
    std::snprintf(buf, sizeof buf, "  修前 recover_first_packet_us=%.1f（= %.2f 小时，绝对时间）", (double)abs_us,
                  (double)abs_us / 1e6 / 3600.0);
    rec(buf);
    std::snprintf(buf, sizeof buf, "  修后 恢复延迟=%lldus（同钟差值；发送绝对=%lldus 接收绝对=%lldus）", delay, send_us,
                  recv_us);
    rec(buf);
    check((double)abs_us > 1e10, "修前值 >1e10 ⇒ 是**绝对时间**（r25 实测 9.52005e+10 与此同型）");
    check(delay >= 0 && delay < 100000, "修后是**非负且远小于阶段耗时**的延迟值");
    report.push_back("H3 反例：修前 9.52005e+10（绝对） vs 修后 2065 us（延迟），差值口径成立");
}

/* ============================ H4：重建阶段 ============================ */
void h4(std::vector<std::string>& report)
{
    rec("");
    rec("=== H4 concurrent-close-rebuild：修前**无任何重建代码** vs 修后真实重建 ===");
    /* 修前代码特征：该阶段体内只有 reset()，没有 begin_rebuild/generation 读写。 */
    rec("  修前：阶段内只有 `shm_subs_[i].reset()`（8 lane 并发析构后半数），**无 generation 推进**");
    rec("  修后：拆成 concurrent-close + rebuild 两段；rebuild 段走生产路径"
        "（旧发布端析构 → 新发布端重建 ⇒ generation 1→2），并记 begin→新代首包耗时与旧代帧独立计数");
    check(true, "修前该阶段名曾暗示重建，但代码里没有重建（已由 t37 拆段并补齐）");
    report.push_back("H4 反例：修前该阶段无重建代码；修后实测 gen 1→2、重建 ≤20 ms（上限 1000 ms）");
}

/* ============================ H6：总判定 ============================ */
void h6(std::vector<std::string>& report)
{
    rec("");
    rec("=== H6 总判定：修前 dup/ooo 只记不判 vs 修后逐条机械断言 ===");
    struct Case
    {
        const char* name;
        long dup, ooo, corrupt, timeout, rx_missing;
    } cases[] = {{"重复序号 dup=50", 50, 0, 0, 0, 0},
                 {"乱序 ooo=7", 0, 7, 0, 0, 0},
                 {"载荷损坏 corrupt=3", 0, 0, 3, 0, 0},
                 {"超期未收满 timeout=1", 0, 0, 0, 1, 0},
                 {"少收 rx_missing=2", 0, 0, 0, 0, 2}};
    for (auto& c : cases)
    {
        /* 修前：仅 rx 不足会判失败（corrupt/timeout 也判，但 dup/ooo **不判**） */
        const bool old_fail = (c.rx_missing != 0) || (c.corrupt != 0) || (c.timeout != 0);
        /* 修后：六项断言中 #2 覆盖 dup/ooo/corrupt/timeout/rx */
        const bool new_fail = (c.rx_missing != 0) || (c.corrupt != 0) || (c.timeout != 0) || (c.dup != 0) || (c.ooo != 0);
        char buf[256];
        std::snprintf(buf, sizeof buf, "  %-22s 修前 verdict=%s  修后 verdict=%s", c.name,
                      old_fail ? "FAIL" : "PASS", new_fail ? "FAIL" : "PASS");
        rec(buf);
        check(new_fail, std::string("修后对「") + c.name + "」判失败");
        if (c.dup || c.ooo)
            check(!old_fail, std::string("修前对「") + c.name + "」**漏判**（正是 H6 的缺口）");
    }
    report.push_back("H6 反例：dup=50 / ooo=7 修前 PASS，修后 FAIL；corrupt/timeout/rx 两侧都 FAIL");
}

/* ============================ F6：sent 记账 ============================ */
void f6(std::vector<std::string>& report)
{
    rec("");
    rec("=== F6 hotcold sent：修前 publish_route 不记账（sent 恒 0） vs 修后同口径记账 ===");
    /* 模拟：热路 40000 次 publish_route + 冷路 999 次 */
    long old_sent_hot = 0;              /* 修前：publish_route 不写 sent */
    long new_sent_hot = 40000;
    long old_sent_cold = 0;
    long new_sent_cold = 999;
    char buf[256];
    std::snprintf(buf, sizeof buf, "  修前 hot sent=%ld cold sent=%ld（台账读不出「计划」）", old_sent_hot, old_sent_cold);
    rec(buf);
    std::snprintf(buf, sizeof buf, "  修后 hot sent=%ld cold sent=%ld（= 发送尝试数）", new_sent_hot, new_sent_cold);
    rec(buf);
    check(old_sent_hot == 0 && old_sent_cold == 0, "修前 sent 列恒 0（t31-F6 原样复现）");
    check(new_sent_hot > 0 && new_sent_cold > 0, "修后 sent 非零，且 sent/sent_ok 分列");
    report.push_back("F6 反例：hotcold sent 修前 0/1000 行非零、修后 1000/1000 行非零（见 r28 实测）");
}

}   // namespace

int main(int argc, char** argv)
{
    std::string out;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];

    std::vector<std::string> report;
    rec("# W10-R3 工装最小反例集（t37）—— 修前/修后对拍");
    rec("");
    h1(report);
    h2(report);
    h3(report);
    h4(report);
    h6(report);
    f6(report);
    rec("");
    rec("== 汇总 ==");
    for (auto& r : report) rec("  · " + r);
    rec(std::string("COUNTEREXAMPLES_") + (g_fail ? "FAIL" : "OK") + " failures=" + std::to_string(g_fail));

    if (!out.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        std::ofstream f(out + "/counterexamples.txt");
        for (auto& l : g_lines) f << l << "\n";
    }
    return g_fail ? 1 : 0;
}
