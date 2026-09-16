/* base2_probe —— ser-cli 握手通道(base+2)的**独立进程、只读**观测器。
 *
 * 与 dzipc_topic_cat 的 `-w` 的分工(这是本文件存在的全部理由):
 *   `-w` 的观测挂在 sniffer 的 **socket 腿**上(main.cc:228-236 自己印的 note 写明),
 *   而 Auto ser-cli 一旦切到 SHM, 池轮询线程会**重建** sniffer 成 SHM 腿 ——
 *   那一刻 `-w` 的观测随旧 sniffer 一起消失。于是"切到 SHM 之后握手通道上还在发生
 *   什么"用 `-w` 是**看不到**的(且不报错, 只是不显示)。
 *   本探针是**独立 OS 进程**, 与 sniffer 的传输重建解耦, 因此切换全程连续可观测。
 *
 * 安全性(沿用 handshake_probe.h 的既有结论, 此处不重复推导):
 *   - base+2 是 **IP 组播**通道, 投递语义是"每个匹配的 socket 各得一份副本",
 *     多一个监听者**不偷包**(该结论在 handshake_probe.h 里已有有/无观察者对照实测);
 *   - 本进程**从不 send**(只用 create/connect/receive_nowait/close);
 *   - 解码**复用产品自己的字节布局**(handshake_probe.h), 不做第二套解读;
 *   - 对 SHM 段只做 `opendir` 名单列举, **绝不 open/create**, 不产生任何段。
 *
 * 输出(stdout, 全 ASCII 单行, 供逐行对账):
 *   PROBE ready pid=.. port=.. topic=.. domain=.. hz=..
 *   HS t=<ms> server=<Name>(<v>) client=<Name>(<v>) frames=<n> undec=<n>      仅状态变化时
 *   BEAT t=<ms> server_frames=.. client_frames=..                              每 1000ms
 *   SHMSEG +<name> / SHMSEG -<name>                                            仅名单变化时
 *   PROBE SUMMARY frames=.. undecodable=.. transitions=.. shm_events=..
 */
#include <algorithm>
#include <atomic>
#include <cctype>   // std::isalnum
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

#include "handshake_probe.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Args
{
    std::string topic;
    int domain{0};
    int hz{1000};
    uint64_t hold_ms{0};
    bool shm_watch{true};
    bool quiet{false};
};

bool take_value(int argc, char** argv, int& i, const char* name, std::string& out)
{
    if (std::strcmp(argv[i], name) == 0)
    {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "ERROR %s needs a value\n", name);
            std::exit(2);
        }
        out = argv[++i];
        return true;
    }
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
        if (take_value(argc, argv, i, "--topic", v)) a.topic = v;
        else if (take_value(argc, argv, i, "--domain", v)) a.domain = std::atoi(v.c_str());
        else if (take_value(argc, argv, i, "--hz", v)) a.hz = std::atoi(v.c_str());
        else if (take_value(argc, argv, i, "--hold-ms", v)) a.hold_ms = std::strtoull(v.c_str(), nullptr, 10);
        else if (std::strcmp(argv[i], "--no-shm-watch") == 0) a.shm_watch = false;
        else if (std::strcmp(argv[i], "--quiet") == 0) a.quiet = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            std::printf("base2_probe --topic T --domain D [--hz 1000] [--hold-ms H] [--no-shm-watch] [--quiet]\n");
            std::exit(0);
        }
        else
        {
            std::fprintf(stderr, "ERROR unknown argument '%s'\n", argv[i]);
            std::exit(2);
        }
    }
    if (a.topic.empty())
    {
        std::fprintf(stderr, "ERROR --topic is required\n");
        std::exit(2);
    }
    if (a.hz < 1 || a.hz > 100000) a.hz = 1000;
    return a;
}

/* /dev/shm 名单(只读)。用来独立于 topic_cat / sniffer 观察 SHM 腿的**出现与消失**。
 * 只 opendir + readdir, 不 open 任何段 —— 不产生、不修改、不删除。 */
std::vector<std::string> list_shm_for_topic(const std::string& topic)
{
    std::vector<std::string> out;
    /* ⛔ 不要自己去复刻段名拼接规则, 两条原因都是实测的:
     *   ① 主题型段名走 sanitize_topic_name()(name_operator.cc:13-23: 保留
     *      alnum/_/-/. , 其余逐字节变 '_'), 而**服务型**段名走
     *      shm_service_prefix()(:31-36) —— 那里注释明写"历史上没有对 topic 名做
     *      sanitize", 于是同一 topic 在两类段名里长得**不一样**;
     *   ② 段名拼接权在产品手里, 探针复刻一份必然会随产品漂移。
     * 所以只取 topic 的**最后一段**(不含 '/'), 做"子串包含"匹配 —— 它在两种形态
     * 里都原样出现。认不出宁可漏报, 绝不猜。 */
    std::string needle = topic;
    const size_t slash = needle.rfind('/');
    if (slash != std::string::npos) needle = needle.substr(slash + 1);
    if (needle.empty()) return out;
    DIR* d = ::opendir("/dev/shm");
    if (d == nullptr) return out;
    while (struct dirent* e = ::readdir(d))
    {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (name.find(needle) != std::string::npos) out.push_back(name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

}   // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const Args a = parse_args(argc, argv);

    dzIPC::handshake_probe probe;
    if (!probe.open(a.topic, a.domain))
    {
        /* 开失败不静默: 明确打一行 FAIL 并以非 0 退出, 免得外部脚本把"没观测"
         * 读成"没流量"。 */
        std::printf("PROBE FAIL open topic=%s domain=%d\n", a.topic.c_str(), a.domain);
        std::fflush(stdout);
        return 3;
    }
    const auto snap0 = probe.snapshot();
    std::printf("PROBE ready pid=%d port=%u topic=%s domain=%d hz=%d shm_watch=%d\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(snap0.port), a.topic.c_str(), a.domain, a.hz, a.shm_watch ? 1 : 0);
    std::fflush(stdout);

    const int64_t t0 = now_ms();
    std::string last_state;
    uint64_t transitions = 0;
    uint64_t frames_at_last_beat = 0;
    int64_t next_beat = t0 + 1000;
    std::vector<std::string> last_shm;
    bool shm_first = true;
    uint64_t shm_events = 0;
    const int64_t poll_ns = 1000000000LL / a.hz;

    while (!g_stop)
    {
        probe.poll();
        const auto s = probe.snapshot();

        auto side = [](const dzIPC::handshake_observation& o) -> std::string
        {
            if (!o.seen) return std::string("(none)");
            std::string v = dzIPC::path_state_name(o.path_state);
            v += "(";
            v += std::to_string(static_cast<int>(o.path_state));
            v += ")";
            if (!o.has_path_state) v += "[old_frame]";
            return v;
        };
        const std::string cur = side(s.server) + "|" + side(s.client);
        if (cur != last_state)
        {
            last_state = cur;
            ++transitions;
            if (!a.quiet)
            {
                std::printf("HS t=%lld server=%s client=%s frames=%llu undec=%llu\n",
                            static_cast<long long>(now_ms() - t0), side(s.server).c_str(), side(s.client).c_str(),
                            static_cast<unsigned long long>(s.frames),
                            static_cast<unsigned long long>(s.undecodable));
                std::fflush(stdout);
            }
        }

        const int64_t now = now_ms();
        if (now >= next_beat)
        {
            next_beat = now + 1000;
            /* 心跳是"探针活着但没流量"与"探针死了"的分界 —— 没有它, 一段安静的
             * 日志无法区分这两种完全不同的情况。 */
            std::printf("BEAT t=%lld server_frames=%llu client_frames=%llu frames=%llu undec=%llu\n",
                        static_cast<long long>(now - t0), static_cast<unsigned long long>(s.server.frames),
                        static_cast<unsigned long long>(s.client.frames),
                        static_cast<unsigned long long>(s.frames),
                        static_cast<unsigned long long>(s.undecodable));
            std::fflush(stdout);
            frames_at_last_beat = s.frames;
        }
        (void)frames_at_last_beat;

        if (a.shm_watch)
        {
            const auto cur_shm = list_shm_for_topic(a.topic);
            if (shm_first || cur_shm != last_shm)
            {
                if (!shm_first)
                {
                    for (const auto& n : cur_shm)
                    {
                        if (std::find(last_shm.begin(), last_shm.end(), n) == last_shm.end())
                        {
                            std::printf("SHMSEG +%s t=%lld\n", n.c_str(), static_cast<long long>(now - t0));
                            ++shm_events;
                        }
                    }
                    for (const auto& n : last_shm)
                    {
                        if (std::find(cur_shm.begin(), cur_shm.end(), n) == cur_shm.end())
                        {
                            std::printf("SHMSEG -%s t=%lld\n", n.c_str(), static_cast<long long>(now - t0));
                            ++shm_events;
                        }
                    }
                }
                else
                {
                    for (const auto& n : cur_shm)
                    {
                        std::printf("SHMSEG +%s t=0 [preexisting]\n", n.c_str());
                    }
                }
                std::fflush(stdout);
                last_shm = cur_shm;
                shm_first = false;
            }
        }

        if (a.hold_ms && static_cast<uint64_t>(now - t0) >= a.hold_ms) break;
        std::this_thread::sleep_for(std::chrono::nanoseconds(poll_ns));
    }

    const auto s = probe.snapshot();
    std::printf("PROBE SUMMARY frames=%llu undecodable=%llu transitions=%llu shm_events=%llu "
                "server_frames=%llu client_frames=%llu\n",
                static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.undecodable),
                static_cast<unsigned long long>(transitions), static_cast<unsigned long long>(shm_events),
                static_cast<unsigned long long>(s.server.frames),
                static_cast<unsigned long long>(s.client.frames));
    std::fflush(stdout);
    probe.close();
    return 0;
}
