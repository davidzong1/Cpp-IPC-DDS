/* dzipc_topic_cat 的 CLI 参数矩阵 —— 重点钉住 -w/--watch_handshake 的语义。
 *
 * 为什么值得单独一组用例(这不是"补个覆盖率"):
 *   `-w` 先后用过两种类型, 两种都"看着能跑"却都是坏的:
 *     ① Type::BOOL —— 无条件把下一个 token 当自己的值。于是 `-w -f 4` 里 `-w` 把 `-f`
 *        吞成值(convert<bool>("-f")==false ⇒ 开关**静默关闭**), 剩下孤零零的 `4` 落到
 *        位置参数被丢弃 ⇒ 频率一起丢。**两个参数同时失效**, 屏幕只表现为"没有握手流量"。
 *     ② Type::FLAG —— 不再吞 token(这一点对), 但 `--watch_handshake=false` 也变成真:
 *        用户**明确写了关**却被打开。
 *   两种失效都不会报错、不会崩, 与"确实没有流量"无法区分 —— 正是本仓最难查的一类。
 *   现在用的是 Type::OPT_BOOL(裸写即真 + 显式值优先), 下面把期望语义逐条钉死。
 *
 * 两组用例:
 *   ① ArgParser 级 —— 快, 且能断言 freq 这类**无法从外部观测**的量;
 *   ② 真二进制级 —— 直接跑 build/app/dzipc_topic_cat。这一组不依赖任何"注册副本",
 *      所以 main.cc 若把 -w 的 Type 改回去, 它会立刻红(①会因为副本漂移而漏掉)。
 */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include "argparser.h"

namespace {

/* ------------------------------------------------------------------ ① ArgParser 级 */

struct WatchParse
{
    bool watch{false};
    int freq{0};
};

/* ⛔ 这是 main.cc 注册的**副本**, 会与 main.cc 漂移 —— 改动 main.cc 的参数表时
 *    记得同步这里; 真正不可能漂移的是下面的 ② 组。 */
WatchParse parse_watch(const std::vector<std::string>& extra)
{
    ArgParser p("dzipc_topic_cat", "Topic cat for dzIPC");
    p.add_argument("--topic", "-t", "Topic name", ArgParser::Type::STRING, true);
    p.add_argument("--ser_or_topic", "-s", "Service(true) or Publish(false) flag", ArgParser::Type::BOOL, true);
    p.add_argument("--msg_id", "-m", "Message ID to filter (optional)", ArgParser::Type::INT, false, "0");
    p.add_argument("--freq", "-f", "Receive frequency in Hz (default: 20)", ArgParser::Type::INT, false, "0");
    /* 与 main.cc 一致: 必须是 OPT_BOOL */
    p.add_argument("--watch_handshake", "-w", "watch handshake, read-only", ArgParser::Type::OPT_BOOL);
    p.add_argument("--transport", "", "Transport to sniff", ArgParser::Type::STRING, false, "auto");

    std::vector<std::string> toks{"-t", "cli_none", "-s", "true"};
    toks.insert(toks.end(), extra.begin(), extra.end());

    std::vector<char*> argv;
    argv.reserve(toks.size() + 1);
    std::string prog = "dzipc_topic_cat";
    argv.push_back(prog.data());
    for (auto& t : toks)
    {
        argv.push_back(t.data());
    }
    p.parse(static_cast<int>(argv.size()), argv.data());

    WatchParse r;
    r.watch = p.get<bool>("--watch_handshake");
    r.freq = p.get<int>("--freq");
    return r;
}

struct Case
{
    const char* what;
    std::vector<std::string> extra;
    bool expect_watch;
    int expect_freq;
};

TEST(TopicCatCliMatrix, WatchHandshakeSemantics)
{
    const std::vector<Case> cases{
        /* --- 默认与裸写 --- */
        {"absent", {}, false, 0},
        {"-w", {"-w"}, true, 0},
        {"--watch_handshake", {"--watch_handshake"}, true, 0},
        /* --- 显式值(本次任务的核心目标) --- */
        {"-w true", {"-w", "true"}, true, 0},
        {"-w false", {"-w", "false"}, false, 0},
        {"--watch_handshake=true", {"--watch_handshake=true"}, true, 0},
        {"--watch_handshake=false", {"--watch_handshake=false"}, false, 0},
        {"-w=false", {"-w=false"}, false, 0},
        /* --- 同一套词汇的其它写法 --- */
        {"-w 1", {"-w", "1"}, true, 0},
        {"-w 0", {"-w", "0"}, false, 0},
        {"-w yes", {"-w", "yes"}, true, 0},
        {"-w no", {"-w", "no"}, false, 0},
        {"-w on", {"-w", "on"}, true, 0},
        {"-w off", {"-w", "off"}, false, 0},
        /* --- 承重回归: 开关**不得吞掉**后面的参数 --- */
        {"-w -f 4", {"-w", "-f", "4"}, true, 4},
        {"-f 4 -w", {"-f", "4", "-w"}, true, 4},
        {"-w false -f 4", {"-w", "false", "-f", "4"}, false, 4},
        {"-w -m 7 -f 9", {"-w", "-m", "7", "-f", "9"}, true, 9},
        /* --- 非布尔字面量不作为值消费(落到位置参数, 与既有 BOOL 行为一致) --- */
        {"-w 3", {"-w", "3"}, true, 0},
    };

    for (const auto& c : cases)
    {
        SCOPED_TRACE(std::string("case: ") + c.what);
        const WatchParse got = parse_watch(c.extra);
        EXPECT_EQ(got.watch, c.expect_watch) << "watch 语义不符";
        EXPECT_EQ(got.freq, c.expect_freq) << "freq 被吞/未生效";
    }
}

/* 既有类型必须**零行为变更** —— argparser.h 只有一份:
 * exec/dzipc_pub/include/argparser.h 是**软链**到 exec/dzipc_topic_cat/include/argparser.h 的
 * (git 里就是 120000 symlink), 所以"改局部不动 dzipc_pub"这个选项并不存在 —— 动这个头就是在动 dzipc_pub。
 * 这里把它的既有类型逐条钉住。 */
TEST(TopicCatCliMatrix, ExistingTypesUnchanged)
{
    /* 用给定 token 解析一个含 FLAG/BOOL/INT 的最小 parser。 */
    auto run = [](const std::vector<std::string>& toks)
    {
        ArgParser p("p", "");
        p.add_argument("--once", "-1", "pure switch", ArgParser::Type::FLAG);
        p.add_argument("--s", "-s", "bool with value", ArgParser::Type::BOOL);
        p.add_argument("--n", "-n", "int", ArgParser::Type::INT, false, "0");

        std::string prog = "p";
        std::vector<std::string> owned = toks;   /* 持有这些 string, argv 只存指针 */
        std::vector<char*> argv;
        argv.push_back(prog.data());
        for (auto& t : owned)
        {
            argv.push_back(t.data());
        }
        p.parse(static_cast<int>(argv.size()), argv.data());
        return p;
    };

    /* FLAG: 出现即真; `--once=false` 仍是**真**(历史语义, 本次改动不许碰它),
     * 且后面那个孤立的 "false" 落位置参数被忽略 —— 一并钉住, 免得日后"顺手修好"成行为变更。 */
    EXPECT_FALSE(run({}).get<bool>("--once"));
    EXPECT_TRUE(run({"--once"}).get<bool>("--once"));
    EXPECT_TRUE(run({"--once", "false"}).get<bool>("--once"))
        << "FLAG 的 `--once=false` 必须仍是真(dzipc_pub --once 的历史语义)";

    /* BOOL / INT: `=` 形式与分离形式都照旧。 */
    EXPECT_TRUE(run({"-s", "true"}).get<bool>("--s"));
    EXPECT_FALSE(run({"-s", "false"}).get<bool>("--s"));
    EXPECT_EQ(run({"--n=5"}).get<int>("--n"), 5);
    EXPECT_EQ(run({"-n", "5"}).get<int>("--n"), 5);
}

/* ------------------------------------------------------------------ ② 真二进制级 */

/* build/bin/test_* → build/app/dzipc_topic_cat */
std::string tool_path()
{
    char buf[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
    {
        return {};
    }
    std::string self(buf, static_cast<size_t>(n));
    const auto slash = self.find_last_of('/');
    if (slash == std::string::npos)
    {
        return {};
    }
    return self.substr(0, slash) + "/../app/dzipc_topic_cat";
}

/* 跑真工具并抓 stderr+stdout。
 * 观测量: main.cc 在 `-s true` + 非 socket 传输时, watch 为真才打印那行 `note:` ——
 * 于是"有没有 note"就是 watch 的真值, 不需要真 topic(工具会停在等待态, 由 timeout 收掉)。 */
std::string run_tool(const std::string& extra)
{
    const std::string tool = tool_path();
    if (tool.empty())
    {
        return {};
    }
    const std::string cmd =
        "timeout 1 '" + tool + "' -t cli_matrix_probe -s true " + extra + " 2>&1";
    FILE* f = ::popen(cmd.c_str(), "r");
    if (f == nullptr)
    {
        return {};
    }
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f) != nullptr)
    {
        out += buf;
    }
    ::pclose(f);
    return out;
}

bool note_present(const std::string& out)
{
    return out.find("note: --watch_handshake") != std::string::npos;
}

TEST(TopicCatCliMatrix, WatchHandshakeAgainstTheRealBinary)
{
    /* 工具没构建(或不在预期位置)时跳过, 而不是留下一个假红。 */
    const std::string tool = tool_path();
    if (tool.empty() || ::access(tool.c_str(), X_OK) != 0)
    {
        GTEST_SKIP() << "dzipc_topic_cat 不在 " << tool << ", 跳过真二进制用例";
    }

    const std::vector<std::pair<const char*, bool>> cases{
        {"", false},
        {"-w", true},
        {"--watch_handshake", true},
        {"-w true", true},
        {"-w false", false},                      /* FLAG 会在这里红 */
        {"--watch_handshake=false", false},       /* FLAG 会在这里红 */
        {"-w -f 4", true},                        /* BOOL 会在这里红(吞掉 -f ⇒ 判假) */
        {"-f 4 -w", true},
        {"-w=false", false},
    };

    for (const auto& c : cases)
    {
        SCOPED_TRACE(std::string("real binary, args: '") + c.first + "'");
        const std::string out = run_tool(c.first);
        ASSERT_FALSE(out.empty()) << "工具没有产生任何输出, 用例无效";
        EXPECT_EQ(note_present(out), c.second)
            << (c.second ? "期望打印 note(watch 为真)但没有" : "期望没有 note(watch 为假)但打印了");
    }
}

}   // namespace
