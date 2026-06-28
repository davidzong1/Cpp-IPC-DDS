#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>
#include "argparser.h"
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "info.h"
#include "msg_type_identify.h"
#include "sniffer.h"
constexpr std::uint64_t RECV_FREQ = 20;   //Hz

enum class StateMachine : int {
    UNCONNECTED = 0,
    CONNECTED = 1,
};

namespace {
volatile std::sig_atomic_t g_running = 1;

std::mutex topic_in_use_mutex;
std::condition_variable topic_in_use_cv;
std::mutex sniffer_use_mutex;
std::string kind_cache{"unknown"};

void handle_sigint(int)
{
    std::cerr << "Process exiting..." << std::endl;
    g_running = 0;
    topic_in_use_cv.notify_all();
}
}   // namespace

bool link_type{false};   // false for socket, true for shm

bool check_topic_state(dzIPC::info_pool::IpcInfoPool& pool, std::string& topic_name, bool ser_or_topic, uint32_t msg_id,
                       std::unique_ptr<dzIPC::sniffer>& sniffer, std::unique_ptr<dzIPC::TopicData>& MsgManager_topic,
                       std::unique_ptr<dzIPC::ServiceData>& MsgManager_service, std::atomic<StateMachine>& state)
{
    auto entries = pool.snapshot(true);
    for (const auto& entry : entries)
    {
        if (entry.topic_name == topic_name)
        {
            if (!entry.alive || !entry.in_use)
            {
                state.store(StateMachine::UNCONNECTED, std::memory_order_release);
                {
                    std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                    MsgManager_service.reset();
                    sniffer.reset();
                }
                return false;
            }
            if ((kind_cache != std::string(dzIPC::info_pool::get_type_from_kind(entry.kind))) && kind_cache != "unknown")
            {
                kind_cache = std::string(dzIPC::info_pool::get_type_from_kind(entry.kind));
                state.store(StateMachine::UNCONNECTED, std::memory_order_release);
                {
                    std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                    MsgManager_service.reset();
                    sniffer.reset();
                }
                return false;
            }
            if (state.load(std::memory_order_acquire) == StateMachine::CONNECTED)
            {
                return false;
            }
            if (ser_or_topic)
            {
                if (entry.kind == dzIPC::info_pool::EntryKind::SocketServer
                    || entry.kind == dzIPC::info_pool::EntryKind::SocketClient
                    || entry.kind == dzIPC::info_pool::EntryKind::ShmServer
                    || entry.kind == dzIPC::info_pool::EntryKind::ShmClient)
                {
                    printf("\x1b[2J\x1b[HFound service '%s' with msg_id %u! Connecting...\n", topic_name.c_str(),
                           msg_id);
                    int domain_id = entry.domain_id;
                    link_type = (entry.kind == dzIPC::info_pool::EntryKind::ShmServer
                                 || entry.kind == dzIPC::info_pool::EntryKind::ShmClient)
                                    ? true
                                    : false;
                    {
                        std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                        MsgManager_service.reset(new dzIPC::ServiceData(std::make_shared<IpcMsgBase>(),
                                                                        std::make_shared<IpcMsgBase>(), msg_id));
                        sniffer.reset(new dzIPC::sniffer(topic_name, domain_id, ser_or_topic, link_type, msg_id));
                    }
                    state.store(StateMachine::CONNECTED, std::memory_order_release);
                    kind_cache = std::string(dzIPC::info_pool::get_type_from_kind(entry.kind));
                    return true;
                }
            }
            else
            {
                if (entry.kind == dzIPC::info_pool::EntryKind::SocketPub
                    || entry.kind == dzIPC::info_pool::EntryKind::ShmPub
                    || entry.kind == dzIPC::info_pool::EntryKind::SocketSub
                    || entry.kind == dzIPC::info_pool::EntryKind::ShmSub)
                {
                    printf("\x1b[2J\x1b[HFound topic '%s' with msg_id %u! Connecting...\n", topic_name.c_str(), msg_id);
                    int domain_id = entry.domain_id;
                    link_type = (entry.kind == dzIPC::info_pool::EntryKind::ShmPub
                                 || entry.kind == dzIPC::info_pool::EntryKind::ShmSub)
                                    ? true
                                    : false;
                    {
                        std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                        MsgManager_topic.reset(new dzIPC::TopicData(std::make_shared<IpcMsgBase>(), msg_id));
                        sniffer.reset(new dzIPC::sniffer(topic_name, domain_id, ser_or_topic, link_type, msg_id));
                    }
                    state.store(StateMachine::CONNECTED, std::memory_order_release);
                    kind_cache = std::string(dzIPC::info_pool::get_type_from_kind(entry.kind));
                    return true;
                }
            }
        }
    }
    {
        std::unique_lock<std::mutex> lock(sniffer_use_mutex);
        MsgManager_service.reset();
        MsgManager_topic.reset();
        sniffer.reset();
        kind_cache = "unknown";
    }
    state.store(StateMachine::UNCONNECTED, std::memory_order_release);
    return false;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handle_sigint);
    ArgParser parser("dzipc_topic_cat", "Topic cat for dzIPC");
    parser.add_argument("--topic", "-t", "Topic name", ArgParser::Type::STRING, true);
    parser.add_argument("--ser_or_topic", "-s", "Service(true) or Publish(false) flag", ArgParser::Type::BOOL, true);
    parser.add_argument("--msg_id", "-m", "Message ID to filter (optional)", ArgParser::Type::INT, false, "0");
    parser.add_argument("--freq", "-f", "Receive frequency in Hz (default: 20)", ArgParser::Type::INT, false, "0");
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
    std::thread pool__thread(
        [&pool, &topic_name, &ser_or_topic, &msg_id, &sniffer, &MsgManager_topic, &MsgManager_service, &state]()
        {
            while (g_running)
            {
                if (check_topic_state(pool, topic_name, ser_or_topic, msg_id, sniffer, MsgManager_topic,
                                      MsgManager_service, state))
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
                    std::unique_lock<std::mutex> lock(sniffer_use_mutex);
                    // message可能在连接丢失时被重置为nullptr，因此需要在使用前检查片段
                    if (!sniffer)
                    {
                        break;
                    }
                    info = std::move(sniffer->try_recv());
                }
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
                       << (link_type ? "SHM" : "SOCKET") << "\033[0m)" << std::setw(20) << "Msg ID: " << msg_id
                       << "\n\n";
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
                       << (link_type ? "SHM" : "SOCKET") << "\033[0m)" << std::setw(20) << "Msg ID: " << msg_id
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