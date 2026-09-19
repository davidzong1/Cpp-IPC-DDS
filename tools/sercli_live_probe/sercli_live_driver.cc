/* sercli_live_driver —— ser-cli 自动选路(IPC_SOCKET 在 ser-cli 上的语义)的跨进程活体驱动。
 *
 * 两种构造方式(都真的走 Auto, 没有 shm/socket 硬编码):
 *   --via direct  (默认): 直接构造 `autopath::auto_ser_ipc` / `auto_cli_ipc`。
 *                          这就是 IPC_SOCKET 在 ser-cli 工厂里被派发到的**同一个类**
 *                          (src/dzIPC/server_ipc.cc:131-136 与 :213 起), 额外好处是
 *                          能读 status() 拿到 decision/fallback。
 *   --via factory        : 走公共工厂 `ServerIPCPtrMake(..., IPC_SOCKET, ...)`
 *                          (dzipc.h:49), 证明**产品公共 API 的 Auto 路径**本身可用。
 *                          代价: 公共包装只暴露 transport_current(), 于是
 *                          decision=/fallback= 只能打 na。
 *
 * 输出纪律: stdout 全部为单行、ASCII、无颜色 —— 供外部脚本逐行对账。
 *
 * 用法:
 *   sercli_live_driver --role server --topic T --domain D --auto [--hold-ms H]
 *   sercli_live_driver --role client --topic T --domain D --auto --count N
 *                      [--period-ms M] [--hold-ms H] [--timeout-ms X]
 * 退出码: client 当且仅当 N 次 RPC **全部 ok** 时返回 0, 否则返回 1(丢失即非 0)。
 *         server 正常收到 SIGTERM/SIGINT 返回 0。
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>   // ::getpid()

#include "dzIPC/auto_ser_cli_ipc.h"
#include "dzIPC/common/path_switch.h"
#include "dzIPC/dzipc.h"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(
        std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
        std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

std::atomic<uint64_t> g_cb_calls{0};

/* 服务端回显: 每项 +1。计数值是"无重复副作用"的跨进程对账量
 * (与本进程外的 client ok 数逐数字比对)。 */
void echo_plus_one(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    g_cb_calls.fetch_add(1, std::memory_order_relaxed);
    auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    res->response = req->request;
    for (auto& v : res->response) v += 1.0;
}

const char* kind_name(dzIPC::path::Kind k)
{
    switch (k)
    {
    case dzIPC::path::Kind::Socket: return "Socket";
    case dzIPC::path::Kind::Shm: return "Shm";
    default: return "None";
    }
}

/* 一行 PATH 尾巴。direct 模式给全量判定字段; factory 模式只有 kind, 其余打 na ——
 * 打 na 而不是省略, 免得外部脚本把"没这一列"误读成"值为空"。 */
std::string path_suffix(const char* kind, const char* decision, const char* fallback, const char* state)
{
    std::string s = " PATH kind=";
    s += kind;
    s += " decision=";
    s += decision;
    s += " fallback=";
    s += fallback;
    s += " state=";
    s += state;
    return s;
}

[[noreturn]] void die(const std::string& why)
{
    std::fprintf(stderr, "ERROR %s\n", why.c_str());
    std::exit(2);
}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(uint64_t ms)
{
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* ------------------------------------------------------------------ 参数 */
struct Args
{
    std::string role{"client"};
    std::string topic;
    int domain{0};
    bool auto_mode{false};
    int count{0};
    uint64_t period_ms{100};
    uint64_t hold_ms{0};
    uint64_t timeout_ms{1500};
    bool force_no_evidence{false};
    bool verbose{false};
    std::string via{"direct"};
    /* UF-004: 让库**不要**接管退出(SIGINT/SIGTERM 走应用自己的处置,
     * 见 dzipc.h 的 DisableShutdownMonitor())。裸布尔: 只写开关名, 没有取值形式
     * ⇒ 不会吞掉下一个 token; 写成 --no-shutdown-monitor=... 会按"未知参数"报错。 */
    bool no_shutdown_monitor{false};
};

bool take_value(int argc, char** argv, int& i, const char* name, std::string& out)
{
    if (std::strcmp(argv[i], name) == 0)
    {
        if (i + 1 >= argc) die(std::string(name) + " needs a value");
        out = argv[++i];
        return true;
    }
    /* 也接受 --name=value */
    const size_t n = std::strlen(name);
    if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
    {
        out = argv[i] + n + 1;
        return true;
    }
    return false;
}

Args parse_args(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        std::string v;
        if (take_value(argc, argv, i, "--role", v)) a.role = v;
        else if (take_value(argc, argv, i, "--topic", v)) a.topic = v;
        else if (take_value(argc, argv, i, "--domain", v)) a.domain = std::atoi(v.c_str());
        else if (take_value(argc, argv, i, "--count", v)) a.count = std::atoi(v.c_str());
        else if (take_value(argc, argv, i, "--period-ms", v)) a.period_ms = std::strtoull(v.c_str(), nullptr, 10);
        else if (take_value(argc, argv, i, "--hold-ms", v)) a.hold_ms = std::strtoull(v.c_str(), nullptr, 10);
        else if (take_value(argc, argv, i, "--timeout-ms", v)) a.timeout_ms = std::strtoull(v.c_str(), nullptr, 10);
        else if (take_value(argc, argv, i, "--via", v)) a.via = v;
        else if (std::strcmp(argv[i], "--auto") == 0) a.auto_mode = true;
        else if (std::strcmp(argv[i], "--force-no-evidence") == 0) a.force_no_evidence = true;
        else if (std::strcmp(argv[i], "--verbose") == 0) a.verbose = true;
        /* ⛔ 裸布尔: 用 strcmp 单判, 不走 take_value(那会吞掉下一个 token) */
        else if (std::strcmp(argv[i], "--no-shutdown-monitor") == 0) a.no_shutdown_monitor = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            std::printf(
                "sercli_live_driver --role server|client --topic T --domain D --auto [--count N] [--period-ms M]\n"
                "                  [--hold-ms H] [--timeout-ms X] [--via direct|factory] [--force-no-evidence]\n"
                "                  [--no-shutdown-monitor]\n");
            std::exit(0);
        }
        else
        {
            die(std::string("unknown argument '") + argv[i] + "' (try --help)");
        }
    }
    if (a.topic.empty()) die("--topic is required");
    if (a.role != "server" && a.role != "client") die("--role must be server|client");
    if (!a.auto_mode) die("--auto is required (this driver only exercises Auto ser-cli)");
    if (a.via != "direct" && a.via != "factory") die("--via must be direct|factory");
    if (a.role == "client" && a.count <= 0) die("--role client needs --count N with N>0");
    if (a.via == "factory" && a.force_no_evidence)
        die("--force-no-evidence is only available with --via direct (factory exposes no Options)");
    return a;
}

/* ------------------------------------------------- 统一的两套后端小包装 */
/* server 侧 */
struct ServerHandle
{
    dzIPC::ServerIPCPtr factory;
    std::unique_ptr<dzIPC::autopath::auto_ser_ipc> direct;

    void init()
    {
        if (factory) factory->InitChannel();
        if (direct) direct->InitChannel();
    }
    dzIPC::path::Kind kind() const
    {
        if (direct) return direct->transport_current();
        return factory->transport_current();
    }
    bool handshake_completed() const
    {
        if (direct) return direct->handshake_completed();
        return factory->handshake_completed();
    }
    const dzIPC::path::Status* status() const { return direct ? &direct->status() : nullptr; }
};

struct ClientHandle
{
    dzIPC::ClientIPCPtr factory;
    std::unique_ptr<dzIPC::autopath::auto_cli_ipc> direct;

    void init()
    {
        if (factory) factory->InitChannel();
        if (direct) direct->InitChannel();
    }
    dzIPC::path::Kind kind() const
    {
        if (direct) return direct->transport_current();
        return factory->transport_current();
    }
    bool handshake_completed() const
    {
        if (direct) return direct->handshake_completed();
        return factory->handshake_completed();
    }
    bool send(std::shared_ptr<dzIPC::ServiceData>& sd, uint64_t tmo)
    {
        if (direct) return direct->send_request(sd, tmo);
        return factory->send_request(sd, tmo);
    }
    const dzIPC::path::Status* status() const { return direct ? &direct->status() : nullptr; }
};

std::string dec_of(const dzIPC::path::Status* s)
{
    return s ? std::string(dzIPC::path::to_string(s->decision())) : std::string("na");
}
std::string fb_of(const dzIPC::path::Status* s)
{
    return s ? std::string(dzIPC::path::to_string(s->fallback())) : std::string("na");
}
std::string st_of(const dzIPC::path::Status* s)
{
    return s ? std::string(dzIPC::path::to_string(s->state())) : std::string("na");
}

/* 打一行 PATH(仅在内容变化时, 免得刷屏淹掉 RPC 行)。返回本行是否打过。 */
template <typename H>
bool emit_path_if_changed(H& h, std::string& last, const char* tag)
{
    const std::string kind = kind_name(h.kind());
    const auto* s = h.status();
    const std::string dec = dec_of(s), fb = fb_of(s), st = st_of(s);
    const std::string cur = kind + "|" + dec + "|" + fb + "|" + st;
    if (cur == last) return false;
    last = cur;
    std::printf("%s%s\n", tag, path_suffix(kind.c_str(), dec.c_str(), fb.c_str(), st.c_str()).c_str());
    std::fflush(stdout);
    return true;
}

/* ------------------------------------------------------------------ server */
int run_server(const Args& a)
{
    ServerHandle h;
    if (a.via == "factory")
    {
        h.factory = dzIPC::ServerIPCPtrMake(a.topic, make_sd(), echo_plus_one, static_cast<size_t>(a.domain),
                                            dzIPC::IPC_SOCKET, a.verbose);
    }
    else
    {
        dzIPC::autopath::Options opts;
        opts.force_no_evidence = a.force_no_evidence;
        h.direct = std::make_unique<dzIPC::autopath::auto_ser_ipc>(a.topic, make_sd(), echo_plus_one,
                                                                   static_cast<size_t>(a.domain), opts, a.verbose);
    }
    h.init();
    std::printf("SERVER ready pid=%d topic=%s domain=%d via=%s\n", static_cast<int>(::getpid()), a.topic.c_str(),
                a.domain, a.via.c_str());
    std::fflush(stdout);

    const int64_t t0 = now_ms();
    std::string last;
    uint64_t last_cb = 0;
    while (!g_stop)
    {
        emit_path_if_changed(h, last, "PATH");
        const uint64_t cb = g_cb_calls.load(std::memory_order_relaxed);
        if (cb != last_cb)
        {
            last_cb = cb;
            const auto* s = h.status();
            std::printf("CB n=%llu%s\n", static_cast<unsigned long long>(cb),
                        path_suffix(kind_name(h.kind()), dec_of(s).c_str(), fb_of(s).c_str(), st_of(s).c_str())
                            .c_str());
            std::fflush(stdout);
        }
        if (a.hold_ms && static_cast<uint64_t>(now_ms() - t0) >= a.hold_ms) break;
        sleep_ms(20);
    }
    std::printf("SERVER SUMMARY cb=%llu\n", static_cast<unsigned long long>(g_cb_calls.load()));
    std::fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------ client */
int run_client(const Args& a)
{
    ClientHandle h;
    if (a.via == "factory")
    {
        h.factory = dzIPC::ClientIPCPtrMake(a.topic, make_sd(), static_cast<size_t>(a.domain), dzIPC::IPC_SOCKET,
                                            a.verbose);
    }
    else
    {
        dzIPC::autopath::Options opts;
        opts.force_no_evidence = a.force_no_evidence;
        h.direct = std::make_unique<dzIPC::autopath::auto_cli_ipc>(a.topic, make_sd(),
                                                                   static_cast<size_t>(a.domain), opts, a.verbose);
    }
    h.init();
    std::printf("CLIENT ready pid=%d topic=%s domain=%d via=%s count=%d\n", static_cast<int>(::getpid()),
                a.topic.c_str(), a.domain, a.via.c_str(), a.count);
    std::fflush(stdout);

    /* 等握手(有上界, 免得没有服务端时无限挂住)。 */
    const int64_t hs_deadline = now_ms() + 6000;
    std::string last;
    while (!g_stop && now_ms() < hs_deadline && !h.handshake_completed())
    {
        emit_path_if_changed(h, last, "PATH");
        sleep_ms(10);
    }
    std::printf("HANDSHAKE completed=%d\n", h.handshake_completed() ? 1 : 0);
    std::fflush(stdout);

    int ok = 0, failed = 0;
    for (int i = 0; i < a.count && !g_stop; ++i)
    {
        auto sd = make_sd();
        sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = {1.0, 2.0, 3.0};
        const int64_t deadline = now_ms() + static_cast<int64_t>(a.timeout_ms);
        bool good = false;
        std::string res_str = "none";
        while (now_ms() < deadline)
        {
            if (h.send(sd, a.timeout_ms))
            {
                const auto r = sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response;
                good = (r.size() == 3 && r[0] == 2.0 && r[1] == 3.0 && r[2] == 4.0);
                if (good)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f", r[0], r[1], r[2]);
                    res_str = buf;
                }
                break;
            }
            sleep_ms(2);
        }
        if (good) ++ok; else ++failed;
        const auto* s = h.status();
        std::printf("RPC seq=%d ok=%d res=%s%s\n", i, good ? 1 : 0, res_str.c_str(),
                    path_suffix(kind_name(h.kind()), dec_of(s).c_str(), fb_of(s).c_str(), st_of(s).c_str()).c_str());
        std::fflush(stdout);
        sleep_ms(a.period_ms);
    }

    /* --hold-ms: RPC 做完后**不立刻退出**, 给外部探针/topic_cat 留观测窗口。
     * 客户端一退, 握手通道就静了 —— 那样"切后仍可观测"这件事根本没法查。 */
    if (a.hold_ms)
    {
        const int64_t hold_deadline = now_ms() + static_cast<int64_t>(a.hold_ms);
        while (!g_stop && now_ms() < hold_deadline)
        {
            emit_path_if_changed(h, last, "PATH");
            sleep_ms(20);
        }
    }

    const auto* s = h.status();
    std::printf("FINAL kind=%s decision=%s fallback=%s state=%s\n", kind_name(h.kind()), dec_of(s).c_str(),
                fb_of(s).c_str(), st_of(s).c_str());
    if (s)
    {
        std::printf("FINAL counters switch_attempts=%llu switch_successes=%llu switch_fallbacks=%llu "
                    "dup_delivery_detected=%llu requests_in_switch_window=%llu evidence_kind=%d\n",
                    static_cast<unsigned long long>(s->switch_attempts.load()),
                    static_cast<unsigned long long>(s->switch_successes.load()),
                    static_cast<unsigned long long>(s->switch_fallbacks.load()),
                    static_cast<unsigned long long>(s->dup_delivery_detected.load()),
                    static_cast<unsigned long long>(s->requests_in_switch_window.load()),
                    s->evidence.kind.load());
    }
    std::printf("SUMMARY sent=%d ok=%d failed=%d\n", a.count, ok, failed);
    std::fflush(stdout);
    return (ok == a.count) ? 0 : 1;
}

}   // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const Args a = parse_args(argc, argv);
    /* UF-004 opt-out: 必须在**任何 IPC 构造之前**(run_server/run_client 里才有构造,
     * 见 :268/:313 一带), 也就是在应用装好自己的处理器(:406-407)之后 —— 这正是
     * "应用自带优雅退出、不想被库接管"的场景。
     * ⛔ 返回值必须**打出来**: false = 太晚(监控已在跑), 那一轮走的其实是默认退出语义,
     *    静默下去会让外部脚本把默认路径当成 opt-out 生效(队内纪律: 静默失效即缺陷)。 */
    if (a.no_shutdown_monitor)
    {
        const bool ok = dzIPC::DisableShutdownMonitor();
        std::printf("OPTOUT no_shutdown_monitor=1 ret=%d\n", ok ? 1 : 0);
        std::fflush(stdout);
        if (!ok)
            std::fprintf(stderr, "WARN --no-shutdown-monitor 太晚: 退出监控已启动, 本次仍走默认退出语义\n");
    }
    return a.role == "server" ? run_server(a) : run_client(a);
}
