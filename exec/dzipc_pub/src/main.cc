#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "argparser.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/dzipc.h"
#include "dzIPC/ipc_info_pool.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "msg_builder.h"
#include "msg_schema.h"
#include "msg_type_identify.h"

namespace {
volatile std::sig_atomic_t g_running = 1;

void handle_sigint(int)
{
    std::fprintf(stderr, "\nReceived SIGINT, shutting down...\n");
    g_running = 0;
}

bool is_subscriber_kind(dzIPC::info_pool::EntryKind kind)
{
    return kind == dzIPC::info_pool::EntryKind::ShmSub
           || kind == dzIPC::info_pool::EntryKind::SocketSub;
}

std::vector<dzIPC::info_pool::EntrySnapshot> find_subscribers(
    const std::vector<dzIPC::info_pool::EntrySnapshot>& entries, const std::string& topic_name)
{
    std::vector<dzIPC::info_pool::EntrySnapshot> subs;
    for (const auto& entry : entries)
    {
        if (entry.topic_name != topic_name) continue;
        if (!entry.alive || !entry.in_use) continue;
        if (is_subscriber_kind(entry.kind)) subs.push_back(entry);
    }
    return subs;
}

/// Read extra field overrides from a file: one spec per line, '#' comments allowed.
bool read_override_file(const std::string& path, std::vector<std::string>& specs)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "Error: cannot open field file '%s'\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(file, line))
    {
        std::string trimmed = dzipc_pub::detail::trim(line);
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        if (trimmed.empty() || trimmed[0] == '#') continue;
        specs.push_back(trimmed);
    }
    return true;
}

}   // anonymous namespace

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handle_sigint);

    // ---- Parse CLI ----
    ArgParser parser("dzipc_pub",
                     "Publish messages to a dzIPC topic (rostopic-pub style). The message schema\n"
                     "is derived from the .msg file matching the subscriber's declared type in\n"
                     "IpcInfoPool, or from --type when given. Field values can be overridden with\n"
                     "-f name=value (arrays: -f arr=[1,2,3]; nested: -f pose.x=1.5).");

    parser.add_argument("--topic", "-t", "Topic name to publish to", ArgParser::Type::STRING, true);
    parser.add_argument("--type", "-T", "Message type name (default: auto-detect from subscriber)",
                        ArgParser::Type::STRING, false, "");
    parser.add_argument("--field", "-f", "Field override: name=value or name:type:value (repeatable)",
                        ArgParser::Type::STRING, false, "");
    parser.add_argument("--file", "", "Read field overrides from file (one spec per line)",
                        ArgParser::Type::STRING, false, "");
    parser.add_argument("--msg-dir", "", "Path to the msg/ directory containing .msg files", ArgParser::Type::STRING, false,
#ifndef DZIPC_MSG_DIR
                        "msg");
#else
                        DZIPC_MSG_DIR);
#endif
    parser.add_argument("--mode", "-m", "Transport: auto, shm, socket (default: auto)", ArgParser::Type::STRING, false,
                        "auto");
    parser.add_argument("--domain", "-d", "Domain ID (default: auto-detect from subscriber)", ArgParser::Type::INT, false,
                        "-1");
    parser.add_argument("--msg_id", "", "Message type ID; must match the subscriber's msg_id (default: 0)",
                        ArgParser::Type::INT, false, "-1");
    parser.add_argument("--rate", "-r", "Publish rate in Hz, fractions allowed (default: 1.0)",
                        ArgParser::Type::DOUBLE, false, "1.0");
    parser.add_argument("--count", "-c", "Number of messages to publish (0 = infinite, default: 0)",
                        ArgParser::Type::INT, false, "0");
    parser.add_argument("--once", "-1", "Publish one message and exit (same as --count 1)",
                        ArgParser::Type::FLAG);
    parser.add_argument("--wait", "-w", "Seconds to wait for a subscriber connection before publishing (default: 5)",
                        ArgParser::Type::DOUBLE, false, "5.0");

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
    std::string type_arg = parser.get<std::string>("--type");
    std::string msg_dir = parser.get<std::string>("--msg-dir");
    std::string mode = parser.get<std::string>("--mode");
    int domain_id_arg = parser.get<int>("--domain");
    int msg_id_arg = parser.get<int>("--msg_id");
    double rate_hz = parser.get<double>("--rate");
    int count = parser.get<int>("--count");
    double wait_sec = parser.get<double>("--wait");
    if (parser.get<bool>("--once"))
    {
        count = 1;
    }

    if (rate_hz <= 0.0)
    {
        std::fprintf(stderr, "Error: --rate must be > 0\n");
        return 1;
    }
    if (count < 0)
    {
        std::fprintf(stderr, "Error: --count must be >= 0\n");
        return 1;
    }

    // Collect repeated --field arguments manually (ArgParser keeps only last value)
    std::vector<std::string> field_overrides;
    for (int i = 1; i < argc; ++i)
    {
        std::string tok = argv[i];
        if ((tok == "--field" || tok == "-f") && i + 1 < argc)
            field_overrides.push_back(argv[++i]);
        else if (tok.rfind("--field=", 0) == 0)
            field_overrides.push_back(tok.substr(8));
        else if (tok.rfind("-f=", 0) == 0)
            field_overrides.push_back(tok.substr(3));
    }
    std::string field_file = parser.get<std::string>("--file");
    if (!field_file.empty() && !read_override_file(field_file, field_overrides))
    {
        return 1;
    }

    // ---- Step 1: Find subscriber entries in IpcInfoPool ----
    auto& pool = dzIPC::info_pool::IpcInfoPool::instance();
    auto entries = pool.snapshot(true);
    auto subscriber_entries = find_subscribers(entries, topic_name);

    std::string type_name = type_arg;
    bool have_discovered_sub = !subscriber_entries.empty();

    if (have_discovered_sub)
    {
        const auto& sub_entry = subscriber_entries[0];
        std::fprintf(stderr, "Found %zu subscriber(s) for topic '%s':\n",
                     subscriber_entries.size(), topic_name.c_str());
        for (const auto& e : subscriber_entries)
        {
            std::fprintf(stderr, "  - kind=%s  type=%s  domain=%d  pid=%d\n",
                         dzIPC::info_pool::to_string(e.kind),
                         e.type_name.c_str(), e.domain_id, e.pid);
        }
        if (type_name.empty())
        {
            type_name = sub_entry.type_name;
        }
        else if (type_name != sub_entry.type_name)
        {
            std::fprintf(stderr, "Warning: --type '%s' differs from subscriber's declared type '%s'\n",
                         type_name.c_str(), sub_entry.type_name.c_str());
        }
    }
    else if (type_name.empty())
    {
        std::fprintf(stderr, "No active subscribers found for topic '%s'.\n", topic_name.c_str());
        std::fprintf(stderr, "Available topics (with subscribers):\n");
        for (const auto& entry : entries)
        {
            if (!entry.alive || !entry.in_use) continue;
            if (is_subscriber_kind(entry.kind))
            {
                std::fprintf(stderr, "  - %-32s  kind=%-12s  type=%-24s  domain=%d\n",
                             entry.topic_name.c_str(),
                             dzIPC::info_pool::to_string(entry.kind),
                             entry.type_name.c_str(),
                             entry.domain_id);
            }
        }
        std::fprintf(stderr,
                     "\nPublishing aborted — pass --type <TypeName> to publish without an active subscriber.\n");
        return 1;
    }
    else
    {
        std::fprintf(stderr,
                     "No active subscribers for topic '%s'; publishing anyway with --type '%s'.\n",
                     topic_name.c_str(), type_name.c_str());
    }

    if (type_name.empty() || type_name == "GenericMessage")
    {
        std::fprintf(stderr,
                     "Error: subscriber's declared type ('%s') does not identify a .msg schema.\n"
                     "       Pass --type <TypeName> explicitly.\n",
                     type_name.c_str());
        return 1;
    }

    // Determine transport
    bool use_shm;
    std::string effective_mode = mode;
    if (mode == "auto")
    {
        use_shm = have_discovered_sub
                      ? (subscriber_entries[0].kind == dzIPC::info_pool::EntryKind::ShmSub)
                      : true;   // no subscriber to imitate — default to shm
        effective_mode = use_shm ? "shm" : "socket";
    }
    else if (mode == "shm")
    {
        use_shm = true;
    }
    else if (mode == "socket")
    {
        use_shm = false;
    }
    else
    {
        std::fprintf(stderr, "Error: --mode must be 'auto', 'shm', or 'socket'\n");
        return 1;
    }

    // Determine domain_id and msg_id
    int domain_id = (domain_id_arg >= 0)
                        ? domain_id_arg
                        : (have_discovered_sub ? subscriber_entries[0].domain_id : 0);
    uint32_t msg_id = (msg_id_arg >= 0) ? static_cast<uint32_t>(msg_id_arg) : 0;
    if (msg_id_arg < 0)
    {
        std::fprintf(stderr,
                     "Note: --msg_id not given, using 0. Subscribers drop messages whose msg_id\n"
                     "      differs from their own — pass --msg_id if the subscriber uses a non-zero id.\n");
    }

    // ---- Step 2: Load and display the schema ----
    dzipc_pub::SchemaRegistry registry(msg_dir);
    const dzipc_pub::MsgSchema* schema = registry.load(type_name);
    if (schema == nullptr)
    {
        std::fprintf(stderr, "Error: cannot find .msg file for type '%s' in '%s'\n",
                     type_name.c_str(), msg_dir.c_str());
        std::fprintf(stderr, "  Make sure --msg-dir points to the project's msg/ directory.\n");
        return 1;
    }
    if (schema->fields.empty())
    {
        std::fprintf(stderr, "Warning: .msg schema for '%s' has no fields.\n", type_name.c_str());
    }

    std::fprintf(stderr, "\n=== Message schema: %s (%zu fields) ===\n",
                 schema->type_name.c_str(), schema->fields.size());
    for (const auto& f : schema->fields)
    {
        std::fprintf(stderr, "  %-24s : %s", f.name.c_str(), dzipc_pub::field_type_name(f.type));
        if (!f.nested_type.empty())
            std::fprintf(stderr, " (%s)", f.nested_type.c_str());
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "=========================================\n\n");

    // ---- Step 3: Build defaults and apply field overrides ----
    dzIPC::GenericMessage msg;
    if (!dzipc_pub::build_default_message(msg, *schema, registry))
    {
        std::fprintf(stderr, "Warning: some nested schemas were missing; their fields default to empty.\n");
    }

    for (const auto& spec : field_overrides)
    {
        std::string err = dzipc_pub::apply_field_override(msg, *schema, registry, spec);
        if (!err.empty())
        {
            std::fprintf(stderr, "Error in field override '%s': %s\n", spec.c_str(), err.c_str());
            return 1;
        }
        std::fprintf(stderr, "Applied field override: %s\n", spec.c_str());
    }

    // ---- Step 4: Create publisher and open the channel ----
    dzIPC::IPCType ipc_type = use_shm ? dzIPC::IPC_SHM : dzIPC::IPC_SOCKET;

    auto topic_base = std::make_shared<dzIPC::GenericMessage>(msg);
    topic_base->set_msg_id(msg_id);
    auto topic_data = std::make_shared<dzIPC::TopicData>(topic_base, msg_id);

    auto pub = dzIPC::PublisherIPCPtrMake(topic_data, topic_name, static_cast<size_t>(domain_id), ipc_type, false);
    pub->InitChannel();

    // Wait for the subscriber handshake so the first message is not lost.
    // (socket publishers block inside InitChannel until connected and never
    // report has_subscribed, so the wait only applies to shm.)
    if (use_shm && wait_sec > 0.0)
    {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(wait_sec));
        while (g_running && !pub->has_subscribed() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!pub->has_subscribed())
        {
            std::fprintf(stderr, "Warning: no subscriber connected within %.1fs, publishing anyway.\n", wait_sec);
        }
    }

    // ---- Step 5: Show exactly what will be sent ----
    {
        auto preview = std::make_shared<dzIPC::GenericMessage>(msg);
        preview->set_msg_id(msg_id);
        ipc::buffer wire = preview->serialize();
        std::fprintf(stderr, "--- Outgoing message ---\n%s------------------------\n",
                     dzIPC::msg_to_string(wire).c_str());
    }

    std::fprintf(stderr, "Publishing to '%s' via %s at %.3g Hz (msg_id=%u, domain=%d)%s. Ctrl+C to stop.\n\n",
                 topic_name.c_str(), effective_mode.c_str(), rate_hz, msg_id, domain_id,
                 count > 0 ? "" : ", infinite");

    // ---- Step 6: Publish loop ----
    int64_t msg_count = 0;
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / rate_hz));
    auto next_tick = std::chrono::steady_clock::now();
    auto last_alive_check = std::chrono::steady_clock::now();

    while (g_running && (count == 0 || msg_count < count))
    {
        auto pub_msg = std::make_shared<dzIPC::GenericMessage>(msg);
        pub_msg->set_msg_id(msg_id);
        bool ok = pub->publish(pub_msg->msgcast<IpcMsgBase>());

        if (ok)
        {
            msg_count++;
            std::fprintf(stderr, "[#%lld] published\n", static_cast<long long>(msg_count));
        }
        else
        {
            std::fprintf(stderr, "Warning: publish failed for message #%lld\n",
                         static_cast<long long>(msg_count + 1));
        }

        if (count != 0 && msg_count >= count) break;

        // Periodically re-check that at least one subscriber is still alive.
        // Only meaningful when the schema was validated against a discovered
        // subscriber; with an explicit --type we keep publishing regardless.
        auto now = std::chrono::steady_clock::now();
        if (have_discovered_sub && now - last_alive_check > std::chrono::seconds(2))
        {
            last_alive_check = now;
            if (find_subscribers(pool.snapshot(false), topic_name).empty())
            {
                std::fprintf(stderr, "\n\033[33mNo subscribers remaining for topic '%s' — stopping.\033[0m\n",
                             topic_name.c_str());
                break;
            }
        }

        next_tick += period;
        std::this_thread::sleep_until(next_tick);
    }

    std::fprintf(stderr, "Published %lld messages. Done.\n", static_cast<long long>(msg_count));
    return 0;
}
