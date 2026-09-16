#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <sstream>
#include <thread>
#include "argparser.h"
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "info.h"
#include "msg_type_identify.h"
#include "sniffer.h"
#include "transport_select.h"
constexpr std::uint64_t RECV_FREQ = 20;   //Hz

enum class StateMachine : int {
    UNCONNECTED = 0,
    CONNECTED = 1,
};

/* --watch_handshake: 是否在 ser-cli 握手通道(base+2)上开一条**只读**监听。
 * 默认 false —— 默认行为与历史逐字一致。见 handshake_probe.h。 */
bool watch_handshake{false};

namespace {
volatile std::sig_atomic_t g_running = 1;

std::mutex topic_in_use_mutex;
std::condition_variable topic_in_use_cv;
std::mutex sniffer_use_mutex;
/* 当前挂着的传输(false=socket, true=shm)。写者(池轮询线程)与读者(显示线程)现在是
 * 跨线程的, 且切换重建会**多次**改写它 —— 旧的裸 bool 在这里是数据竞争, 标签可能停留
 * 在旧值上(屏幕上写着 SHM、其实是 socket 的帧, 或者反过来)。 */
std::atomic<bool> link_type{false};
/* 已选中的池条目, 与 sniffer 同生命周期; 仅在持有 sniffer_use_mutex 时读写。 */
dzipc_topic_cat::Selection cur_sel;
/* 每次**重建** sniffer 自增(不是每次重选)。显示线程据此丢弃上一传输的缓存帧。 */
std::atomic<uint64_t> sniffer_epoch{0};

void handle_sigint(int)
{
    std::cerr << "Process exiting..." << std::endl;
    g_running = 0;
    topic_in_use_cv.notify_all();
}

void clear_sniffer(std::unique_ptr<dzIPC::sniffer>& sniffer, std::unique_ptr<dzIPC::TopicData>& MsgManager_topic,
                   std::unique_ptr<dzIPC::ServiceData>& MsgManager_service)
{
    std::unique_lock<std::mutex> lock(sniffer_use_mutex);
    MsgManager_service.reset();
    MsgManager_topic.reset();
    sniffer.reset();
    cur_sel = dzipc_topic_cat::Selection{};
}

/* 建新 sniffer 到临时变量, 再在锁内交换 —— 持锁期间不做 SHM/UDP 建链这种可能阻塞
 * (乃至 std::exit)的活; 旧 sniffer 在锁外析构, 其接收线程的 join 不占锁。 */
void build_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, bool link, uint32_t msg_id,
                   const dzipc_topic_cat::Selection& sel, std::unique_ptr<dzIPC::sniffer>& sniffer,
                   std::unique_ptr<dzIPC::TopicData>& MsgManager_topic,
                   std::unique_ptr<dzIPC::ServiceData>& MsgManager_service)
{
    std::unique_ptr<dzIPC::sniffer> new_sniffer;
    std::unique_ptr<dzIPC::TopicData> new_topic;
    std::unique_ptr<dzIPC::ServiceData> new_service;
    new_sniffer.reset(new dzIPC::sniffer(topic_name, domain_id, ser_or_topic, link, msg_id, watch_handshake));
    if (ser_or_topic)
        new_service.reset(
            new dzIPC::ServiceData(std::make_shared<IpcMsgBase>(), std::make_shared<IpcMsgBase>(), msg_id));
    else
        new_topic.reset(new dzIPC::TopicData(std::make_shared<IpcMsgBase>(), msg_id));

    {
        std::unique_lock<std::mutex> lock(sniffer_use_mutex);
        MsgManager_service = std::move(new_service);
        MsgManager_topic = std::move(new_topic);
        sniffer = std::move(new_sniffer);
        link_type.store(link, std::memory_order_release);
        cur_sel = sel;
        sniffer_epoch.fetch_add(1, std::memory_order_release);
    }
}
}   // namespace

namespace {
/* 把握手观测渲染成一个显示块。
 * ⛔ opened==false(没开 / 开失败)时返回空串 —— 屏幕上一个字符都不多, 这就是
 *    "默认行为兼容"在输出侧的落点。 */
std::string render_handshake(const dzIPC::handshake_snapshot& hs)
{
    if (!hs.opened)
    {
        return std::string();
    }
    auto one = [](const char* who, const dzIPC::handshake_observation& o)
    {
        std::ostringstream os;
        os << "  " << who << ": ";
        if (!o.seen)
        {
            os << "(no frame yet)";
            return os.str();
        }
        const int64_t age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count()
                               - o.last_ts_ns / 1'000'000;
        os << "path_state=\033[36m" << dzIPC::path_state_name(o.path_state) << "\033[0m("
           << static_cast<int>(o.path_state) << ")"
           << (o.has_path_state ? "" : " \033[33m[旧帧: 无 path_state 字段]\033[0m") << "  run_status="
           << (o.run_status ? "true" : "false") << "  frames=" << o.frames << "  last=" << age_ms << "ms ago";
        return os.str();
    };
    std::ostringstream os;
    os << "------------------------------------------------------\n"
       << "Handshake watch (\033[32mREAD-ONLY\033[0m, port base+2): frames=" << hs.frames
       << " undecodable=" << hs.undecodable << "\n"
       << one("server(host)", hs.server) << "\n"
       << one("client(cli) ", hs.client) << "\n";
    return os.str();
}
}   // namespace

bool check_topic_state(dzIPC::info_pool::IpcInfoPool& pool, std::string& topic_name, bool ser_or_topic, uint32_t msg_id,
                       std::unique_ptr<dzIPC::sniffer>& sniffer, std::unique_ptr<dzIPC::TopicData>& MsgManager_topic,
                       std::unique_ptr<dzIPC::ServiceData>& MsgManager_service, std::atomic<StateMachine>& state,
                       dzipc_topic_cat::TransportPreference pref)
{
    /* 扫描**全部**匹配条目再决定, 不再"取首条即收工": Auto ser-cli 切到 SHM 后池里
     * socket/shm 四条并存(socket 腿 stop_data_plane() 不注销池登记, 且注册在先),
     * 取首条会确定性地选中那条**已经不发数据**的 socket 腿。选型规则见 transport_select.h。 */
    const auto entries = pool.snapshot(true);
    const auto sel = dzipc_topic_cat::select_sniffer_entry(entries, topic_name, ser_or_topic, pref);
    const bool connected = state.load(std::memory_order_acquire) == StateMachine::CONNECTED;

    if (!sel.found)
    {
        /* 没有可用条目(含显式 --transport 下该传输不存在): 断开, 回到等待态。
         * 注意与旧实现的差别: 旧代码"首个匹配条目死了"就断, 现在只有**一条可用都没有**
         * 才断 —— 首条死、次条活的情况以前会误断。 */
        if (connected)
        {
            clear_sniffer(sniffer, MsgManager_topic, MsgManager_service);
        }
        state.store(StateMachine::UNCONNECTED, std::memory_order_release);
        return false;
    }

    if (connected)
    {
        /* 通道没变就不动(同一传输内换了个进程/slot 不必重建, 免得白清显示缓存);
         * 通道变了(典型: Auto 从 socket 切到 SHM)必须重建 —— 否则切换前建好的
         * socket_sniffer 会盯着已经停掉的 UDP 端口空转, 且永不自愈。 */
        if (cur_sel.same_channel(sel))
        {
            return false;
        }
        printf("\x1b[2J\x1b[HTransport changed to %s (slot %d). Reconnecting...\n", sel.shm ? "SHM" : "SOCKET",
               static_cast<int>(sel.slot));
    }
    else
    {
        printf("\x1b[2J\x1b[HFound %s '%s' with msg_id %u! Connecting as %s...\n", ser_or_topic ? "service" : "topic",
               topic_name.c_str(), msg_id, sel.shm ? "SHM" : "SOCKET");
    }

    build_sniffer(topic_name, sel.domain_id, ser_or_topic, sel.shm, msg_id, sel, sniffer, MsgManager_topic,
                  MsgManager_service);
    state.store(StateMachine::CONNECTED, std::memory_order_release);
    return true;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handle_sigint);
    ArgParser parser("dzipc_topic_cat", "Topic cat for dzIPC");
    parser.add_argument("--topic", "-t", "Topic name", ArgParser::Type::STRING, true);
    parser.add_argument("--ser_or_topic", "-s", "Service(true) or Publish(false) flag", ArgParser::Type::BOOL, true);
    parser.add_argument("--msg_id", "-m", "Message ID to filter (optional)", ArgParser::Type::INT, false, "0");
    parser.add_argument("--freq", "-f", "Receive frequency in Hz (default: 20)", ArgParser::Type::INT, false, "0");
    /* ⛔ 必须是 OPT_BOOL(裸写即真, 且支持显式 true/false), 不能用 BOOL/FLAG:
     * - BOOL 无条件消费下一个 token ⇒ `-w -f 4` 把 `-f` 吞成值 ⇒ 观测**静默关闭**,
     *   且孤立的 `4` 落进位置参数被丢弃 ⇒ 频率一起丢。两个参数同时失效, 屏幕只表现为
     *   "没有握手流量" —— 与真的没有流量无法区分。
     * - 裸 FLAG 不消费 token(这一点对), 但 `--watch_handshake=false` 也会变成真:
     *   显式关被读成开。OPT_BOOL 是"裸写即真 + 显式值优先"的并集, 两个毛病都没有。
     * 另: 本仓既有的纯开关 dzipc_pub --once 仍是 FLAG, 语义未动。 */
    parser.add_argument("--watch_handshake", "-w",
                        "ser-cli only: also listen READ-ONLY on the handshake port (base+2) and show each side's "
                        "path_state. Never sends; off by default (bare -w means on; -w false / =false turns it off)",
                        ArgParser::Type::OPT_BOOL);
    parser.add_argument("--transport", "",
                        "Transport to sniff: auto (default) | shm | socket. auto scans every pool entry and prefers "
                        "the SHM leg, falling back to socket; shm/socket wait for that transport only",
                        ArgParser::Type::STRING, false, "auto");
    try
    {
        parser.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    std::string topic_name = parser.get<std::string>("--topic");
    bool ser_or_topic = parser.get<bool>("--ser_or_topic");
    watch_handshake = parser.get<bool>("--watch_handshake");
    dzipc_topic_cat::TransportPreference pref = dzipc_topic_cat::TransportPreference::Auto;
    if (!dzipc_topic_cat::parse_transport_preference(parser.get<std::string>("--transport"), pref))
    {
        std::fprintf(stderr, "Error: --transport must be one of auto|shm|socket (got '%s')\n",
                     parser.get<std::string>("--transport").c_str());
        return 1;
    }
    if (watch_handshake && !ser_or_topic)
    {
        /* 握手通道是 ser-cli 独有的(socket_pub_sub_ipc 对 base+2 的引用计数恒为 0),
         * 所以 topic 模式下 -w 是**空操作**。显式说一声: 静默忽略正是本仓反复出现的
         * 失效形态, 而它的表现("屏幕上什么都没有")与"确实没有握手流量"无法区分。 */
        std::fprintf(stderr,
                     "\033[33mnote: --watch_handshake applies to ser-cli only; with --ser_or_topic false there is no "
                     "handshake channel to watch, so it has no effect\033[0m\n");
    }
    else if (watch_handshake && pref != dzipc_topic_cat::TransportPreference::Socket)
    {
        /* 握手观测只挂在 socket 腿上(见 socket_sniffer.cc); 默认 auto 在 Auto ser-cli
         * 切到 SHM 之后会**重建**成 SHM 腿, 那时这条观测就不再可用(不报错, 只是不显示)。
         * 想持续盯握手通道就 --transport socket。 */
        std::fprintf(stderr,
                     "\033[33mnote: --watch_handshake attaches on the socket leg only; with --transport %s it is "
                     "unavailable once the session moves to SHM (use --transport socket to keep it)\033[0m\n",
                     dzipc_topic_cat::to_string(pref));
    }
    std::atomic<StateMachine> state{StateMachine::UNCONNECTED};
    std::unique_ptr<dzIPC::TopicData> MsgManager_topic;
    std::unique_ptr<dzIPC::ServiceData> MsgManager_service;
    std::unique_ptr<dzIPC::sniffer> sniffer;
    auto& pool = dzIPC::info_pool::IpcInfoPool::instance();
    uint64_t beat_pahse;//ms
    if (parser.get<int>("--freq") > 0)
    {
        beat_pahse = 1'000 / parser.get<int>("--freq");
    }
    else
    {
        beat_pahse = 1'000 / RECV_FREQ;
    }
    uint32_t msg_id = static_cast<uint32_t>(parser.get<int>("--msg_id"));
    dzIPC::sniffer_info info;
    std::stringstream ss;
    std::string req_str_cache, res_str_cache;
    req_str_cache = "";
    res_str_cache = "";
    uint64_t seen_epoch = sniffer_epoch.load(std::memory_order_acquire);
    std::thread pool__thread(
        [&pool, &topic_name, &ser_or_topic, &msg_id, &sniffer, &MsgManager_topic, &MsgManager_service, &state, pref]()
        {
            while (g_running)
            {
                if (check_topic_state(pool, topic_name, ser_or_topic, msg_id, sniffer, MsgManager_topic,
                                      MsgManager_service, state, pref))
                {
                    topic_in_use_cv.notify_all();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    while (g_running)
    {
        if (state == StateMachine::UNCONNECTED)
        {
            fprintf(stderr, "%s\n\033[33mWaiting for topic '%s' to be published...\n\033[0m", ss.str().c_str(),
                    topic_name.c_str());
            std::unique_lock<std::mutex> lock(topic_in_use_mutex);
            topic_in_use_cv.wait(lock,
                                 [&state]
                                 {
                                     return state.load(std::memory_order_acquire) == StateMachine::CONNECTED
                                            || !g_running;
                                 });
        }
        else
        {
            while (g_running && state.load(std::memory_order_acquire) == StateMachine::CONNECTED)
            {
                auto start = std::chrono::steady_clock::now();
                {
                    /* 换了传输就丢掉上一传输的缓存: 否则标签已经是 SHM, 屏幕上还挂着
                     * 切换前 socket 期的最后一帧 —— 正是让人误判"还在抓"的假象。 */
                    const uint64_t epoch = sniffer_epoch.load(std::memory_order_acquire);
                    if (epoch != seen_epoch)
                    {
                        seen_epoch = epoch;
                        req_str_cache.clear();
                        res_str_cache.clear();
                    }
                    std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                    // message可能在连接丢失时被重置为nullptr，因此需要在使用前检查片段
                    if (!sniffer)
                    {
                        break;
                    }
                    info = std::move(sniffer->try_recv());
                }
                const bool is_shm = link_type.load(std::memory_order_acquire);
                if (ser_or_topic)
                {
                    std::string req_str = dzIPC::msg_to_string(info.request);
                    std::string res_str = dzIPC::msg_to_string(info.response);
                    if (req_str.empty())
                    {
                        req_str = req_str_cache;
                    }
                    else
                    {
                        req_str_cache = req_str;
                    }
                    if (res_str.empty())
                    {
                        res_str = res_str_cache;
                    }
                    else
                    {
                        res_str_cache = res_str;
                    }
                    ss.str("");
                    ss.clear();
                    ss << "\x1b[2J\x1b[H=====================================================\n\n";
                    ss << "Topic: \033[32m" << topic_name << "\033[0m" << std::setw(20) << "Type: Service(\033[34m"
                       << (is_shm ? "SHM" : "SOCKET") << "\033[0m)" << std::setw(20) << "Msg ID: " << msg_id
                       << "\n\n";
                    ss << render_handshake(info.hs);
                    ss << "------------------------------------------------------\n"
                       << "Request:\n"
                       << req_str << "\nResponse:\n"
                       << res_str << std::endl;
                    fprintf(stdout, "%s", ss.str().c_str());
                    fflush(stdout);
                }
                else
                {
                    std::string topic_str = dzIPC::msg_to_string(info.request);
                    if (topic_str.empty())
                    {
                        topic_str = req_str_cache;
                    }
                    else
                    {
                        req_str_cache = topic_str;
                    }
                    ss.str("");
                    ss.clear();
                    ss << "\x1b[2J\x1b[H=====================================================\n\n";
                    ss << "Topic: \033[32m" << topic_name << "\033[0m" << std::setw(20) << "Type: Topic(\033[34m"
                       << (is_shm ? "SHM" : "SOCKET") << "\033[0m)" << std::setw(20) << "Msg ID: " << msg_id
                       << "\n\n";
                    ss << "------------------------------------------------------\n"
                       << "Message:\n"
                       << topic_str << std::endl;
                    fprintf(stdout, "%s", ss.str().c_str());
                    fflush(stdout);
                }
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(beat_pahse > duration ? beat_pahse - duration : 0));
            }
        }
    }
    pool__thread.join();
    return 0;
}
