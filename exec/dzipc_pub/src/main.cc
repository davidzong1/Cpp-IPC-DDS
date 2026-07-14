#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "argparser.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/dzipc.h"
#include "dzIPC/ipc_info_pool.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"

namespace {
volatile std::sig_atomic_t g_running = 1;

void handle_sigint(int)
{
    std::fprintf(stderr, "\nReceived SIGINT, shutting down...\n");
    g_running = 0;
}

// =====================================================================
// .msg file schema parser
// =====================================================================

struct MsgField
{
    std::string name;
    uint8_t type{0};        // FieldType / MsgType byte
    std::string nested_type; // name of nested message type (empty if primitive)
};

struct MsgSchema
{
    std::string type_name;  // PascalCase class name, e.g. "StdVector"
    std::vector<MsgField> fields;
};

/// Convert snake_case to PascalCase: "std_vector" → "StdVector"
std::string snake_to_pascal(const std::string& snake)
{
    std::string result;
    bool capitalize = true;
    for (char c : snake)
    {
        if (c == '_')
        {
            capitalize = true;
        }
        else if (capitalize)
        {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize = false;
        }
        else
        {
            result += c;
        }
    }
    return result;
}

/// Map msg-type-string (e.g. "int32", "float64[]", "string") → FieldType byte
uint8_t msg_field_type_byte(const std::string& type_str, bool& is_nested)
{
    is_nested = false;
    // Check for array suffix
    bool is_array = false;
    std::string base = type_str;
    if (base.size() > 2 && base[base.size() - 2] == '[' && base[base.size() - 1] == ']')
    {
        is_array = true;
        base = base.substr(0, base.size() - 2);
    }

    // Primitive types
    if (base == "bool")  return is_array ? 13u : 1u;
    if (base == "int8")  return is_array ? 14u : 2u;
    if (base == "uint8")  return is_array ? 15u : 3u;
    if (base == "int16")  return is_array ? 16u : 4u;
    if (base == "uint16") return is_array ? 17u : 5u;
    if (base == "int32")  return is_array ? 18u : 6u;
    if (base == "uint32") return is_array ? 19u : 7u;
    if (base == "int64")  return is_array ? 20u : 8u;
    if (base == "uint64") return is_array ? 21u : 9u;
    if (base == "float32") return is_array ? 22u : 10u;
    if (base == "float64") return is_array ? 23u : 11u;
    if (base == "string")  return is_array ? 24u : 12u;

    // Nested type
    is_nested = true;
    return is_array ? 26u : 25u;
}

/// Parse a single .msg file and return its schema
MsgSchema parse_msg_file(const std::string& file_path)
{
    MsgSchema schema;
    // Derive type_name from filename: "path/std_vector.msg" → "StdVector"
    auto slash = file_path.find_last_of("/\\");
    std::string basename = (slash != std::string::npos) ? file_path.substr(slash + 1) : file_path;
    auto dot = basename.find_last_of('.');
    if (dot != std::string::npos) basename = basename.substr(0, dot);
    schema.type_name = snake_to_pascal(basename);

    std::ifstream file(file_path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "Warning: cannot open .msg file '%s'\n", file_path.c_str());
        return schema;
    }

    std::string line;
    while (std::getline(file, line))
    {
        // Trim leading/trailing whitespace
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start >= line.size()) continue;  // empty line
        if (line[start] == '#') continue;    // comment

        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;

        std::string content = line.substr(start, end - start);

        // Split at first whitespace: "<type> <name>"
        auto space = content.find_first_of(" \t");
        if (space == std::string::npos) continue;  // malformed line

        std::string type_str = content.substr(0, space);
        std::string field_name = content.substr(space + 1);

        // Trim field_name
        size_t ns = 0;
        while (ns < field_name.size() && (field_name[ns] == ' ' || field_name[ns] == '\t')) ++ns;
        field_name = field_name.substr(ns);

        if (field_name.empty() || type_str.empty()) continue;

        MsgField f;
        f.name = field_name;
        bool is_nested = false;
        f.type = msg_field_type_byte(type_str, is_nested);
        if (is_nested)
        {
            // Extract base type name for nested types (strip [] suffix if present)
            if (type_str.size() > 2 && type_str[type_str.size() - 2] == '[' && type_str[type_str.size() - 1] == ']')
                f.nested_type = type_str.substr(0, type_str.size() - 2);
            else
                f.nested_type = type_str;
        }
        schema.fields.push_back(f);
    }
    return schema;
}

/// Search msg_dir recursively for a .msg file whose PascalCase name matches type_name
std::string find_msg_file(const std::string& msg_dir, const std::string& type_name)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(msg_dir, ec))
    {
        std::fprintf(stderr, "Warning: '%s' is not a directory (ec=%s)\n",
                     msg_dir.c_str(), ec.message().c_str());
        return {};
    }

    for (auto it = fs::recursive_directory_iterator(msg_dir, ec);
         it != fs::recursive_directory_iterator(); ++it)
    {
        if (ec)
        {
            std::fprintf(stderr, "Warning: error scanning '%s': %s\n",
                         msg_dir.c_str(), ec.message().c_str());
            ec.clear();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string path = it->path().string();
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".msg") != 0) continue;

        std::string basename = it->path().stem().string();
        if (snake_to_pascal(basename) == type_name)
        {
            return path;
        }
    }
    return {};
}

// =====================================================================
// Build a GenericMessage populated with default values for each field
// =====================================================================

void build_default_message(dzIPC::GenericMessage& msg, const MsgSchema& schema)
{
    for (const auto& f : schema.fields)
    {
        switch (f.type)
        {
        case 1:  msg.set_bool(f.name, false); break;
        case 2:  msg.set_int8(f.name, 0); break;
        case 3:  msg.set_uint8(f.name, 0); break;
        case 4:  msg.set_int16(f.name, 0); break;
        case 5:  msg.set_uint16(f.name, 0); break;
        case 6:  msg.set_int32(f.name, 0); break;
        case 7:  msg.set_uint32(f.name, 0); break;
        case 8:  msg.set_int64(f.name, 0); break;
        case 9:  msg.set_uint64(f.name, 0); break;
        case 10: msg.set_float32(f.name, 0.0f); break;
        case 11: msg.set_float64(f.name, 0.0); break;
        case 12: msg.set_string(f.name, ""); break;
        case 13: msg.set_bool_array(f.name, {}); break;
        case 14: msg.set_int8_array(f.name, {}); break;
        case 15: msg.set_uint8_array(f.name, {}); break;
        case 16: msg.set_int16_array(f.name, {}); break;
        case 17: msg.set_uint16_array(f.name, {}); break;
        case 18: msg.set_int32_array(f.name, {}); break;
        case 19: msg.set_uint32_array(f.name, {}); break;
        case 20: msg.set_int64_array(f.name, {}); break;
        case 21: msg.set_uint64_array(f.name, {}); break;
        case 22: msg.set_float32_array(f.name, {}); break;
        case 23: msg.set_float64_array(f.name, {}); break;
        case 24: msg.set_string_array(f.name, {}); break;
        case 25: {  // MSG_NESTED
            dzIPC::GenericMessage nested;
            msg.set_nested(f.name, nested);
            break;
        }
        case 26: {  // MSG_NESTED_ARRAY
            msg.set_nested_array(f.name, {});
            break;
        }
        default: break;
        }
    }
}

// =====================================================================
// Field override logic (same as before, but with added array support)
// =====================================================================

const char* field_type_name(uint8_t type)
{
    switch (type)
    {
    case 1: return "bool";
    case 2: return "int8";
    case 3: return "uint8";
    case 4: return "int16";
    case 5: return "uint16";
    case 6: return "int32";
    case 7: return "uint32";
    case 8: return "int64";
    case 9: return "uint64";
    case 10: return "float32";
    case 11: return "float64";
    case 12: return "string";
    case 13: return "bool[]";
    case 14: return "int8[]";
    case 15: return "uint8[]";
    case 16: return "int16[]";
    case 17: return "uint16[]";
    case 18: return "int32[]";
    case 19: return "uint32[]";
    case 20: return "int64[]";
    case 21: return "uint64[]";
    case 22: return "float32[]";
    case 23: return "float64[]";
    case 24: return "string[]";
    case 25: return "nested";
    case 26: return "nested[]";
    default: return "unknown";
    }
}

/// Apply a single field override. Format: "name:type:value"
bool apply_field_override(dzIPC::GenericMessage& msg, const std::string& spec)
{
    auto colon1 = spec.find(':');
    if (colon1 == std::string::npos)
    {
        std::fprintf(stderr, "Invalid field spec '%s' (expected name:type:value)\n", spec.c_str());
        return false;
    }
    auto colon2 = spec.find(':', colon1 + 1);
    if (colon2 == std::string::npos)
    {
        std::fprintf(stderr, "Invalid field spec '%s' (expected name:type:value)\n", spec.c_str());
        return false;
    }
    std::string field_name = spec.substr(0, colon1);
    std::string type_str = spec.substr(colon1 + 1, colon2 - colon1 - 1);
    std::string value_str = spec.substr(colon2 + 1);

    try
    {
        if (type_str == "bool")
            msg.set_bool(field_name, (value_str == "true" || value_str == "1"));
        else if (type_str == "int8")
            msg.set_int8(field_name, static_cast<int8_t>(std::stoi(value_str)));
        else if (type_str == "uint8")
            msg.set_uint8(field_name, static_cast<uint8_t>(std::stoul(value_str)));
        else if (type_str == "int16")
            msg.set_int16(field_name, static_cast<int16_t>(std::stoi(value_str)));
        else if (type_str == "uint16")
            msg.set_uint16(field_name, static_cast<uint16_t>(std::stoul(value_str)));
        else if (type_str == "int32")
            msg.set_int32(field_name, std::stoi(value_str));
        else if (type_str == "uint32")
            msg.set_uint32(field_name, static_cast<uint32_t>(std::stoul(value_str)));
        else if (type_str == "int64")
            msg.set_int64(field_name, std::stoll(value_str));
        else if (type_str == "uint64")
            msg.set_uint64(field_name, std::stoull(value_str));
        else if (type_str == "float32")
            msg.set_float32(field_name, std::stof(value_str));
        else if (type_str == "float64")
            msg.set_float64(field_name, std::stod(value_str));
        else if (type_str == "string")
            msg.set_string(field_name, value_str);
        else
        {
            std::fprintf(stderr, "Unsupported field type '%s' for CLI override\n", type_str.c_str());
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Error setting field '%s': %s\n", field_name.c_str(), e.what());
        return false;
    }
    return true;
}

}   // anonymous namespace

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handle_sigint);

    // ---- Parse CLI ----
    ArgParser parser("dzipc_pub",
                     "Publish messages to a dzIPC topic. Schema is derived from the .msg file\n"
                     "matching the subscriber's declared type in IpcInfoPool.  If no subscriber\n"
                     "exists for the topic, the program exits immediately.");

    parser.add_argument("--topic", "-t", "Topic name to publish to", ArgParser::Type::STRING, true);
    parser.add_argument("--field", "-f", "Field override: name:type:value (repeatable)", ArgParser::Type::STRING, false, "");
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
    parser.add_argument("--msg_id", "", "Message type ID (default: auto from subscriber type)", ArgParser::Type::INT, false,
                        "-1");
    parser.add_argument("--freq", "", "Publish frequency in Hz (default: 1)", ArgParser::Type::INT, false, "1");
    parser.add_argument("--count", "-c", "Number of messages to publish (0 = infinite, default: 0)",
                        ArgParser::Type::INT, false, "0");

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
    std::string msg_dir = parser.get<std::string>("--msg-dir");
    std::string mode = parser.get<std::string>("--mode");
    int domain_id_arg = parser.get<int>("--domain");
    int msg_id_arg = parser.get<int>("--msg_id");
    int freq_hz = parser.get<int>("--freq");
    int count = parser.get<int>("--count");

    if (freq_hz <= 0)
    {
        std::fprintf(stderr, "Error: --freq must be > 0\n");
        return 1;
    }

    // Collect repeated --field arguments manually (ArgParser keeps only last value)
    std::vector<std::string> field_overrides;
    for (int i = 1; i < argc; ++i)
    {
        std::string tok = argv[i];
        if ((tok == "--field" || tok == "-f") && i + 1 < argc)
            field_overrides.push_back(argv[++i]);
        else if (tok.find("--field=") == 0)
            field_overrides.push_back(tok.substr(8));
        else if (tok.find("-f=") == 0)
            field_overrides.push_back(tok.substr(3));
    }

    // ---- Step 1: Find subscriber entries in IpcInfoPool ----
    auto& pool = dzIPC::info_pool::IpcInfoPool::instance();
    auto entries = pool.snapshot(true);

    std::vector<dzIPC::info_pool::EntrySnapshot> subscriber_entries;
    for (const auto& entry : entries)
    {
        if (entry.topic_name != topic_name) continue;
        if (!entry.alive || !entry.in_use) continue;
        if (entry.kind == dzIPC::info_pool::EntryKind::ShmSub
            || entry.kind == dzIPC::info_pool::EntryKind::SocketSub)
        {
            subscriber_entries.push_back(entry);
        }
    }

    if (subscriber_entries.empty())
    {
        std::fprintf(stderr, "No active subscribers found for topic '%s'.\n", topic_name.c_str());

        // Show all available topics
        std::fprintf(stderr, "Available topics (with subscribers):\n");
        for (const auto& entry : entries)
        {
            if (!entry.alive || !entry.in_use) continue;
            if (entry.kind == dzIPC::info_pool::EntryKind::ShmSub
                || entry.kind == dzIPC::info_pool::EntryKind::SocketSub)
            {
                std::fprintf(stderr, "  - %-32s  kind=%-12s  type=%-24s  domain=%d\n",
                             entry.topic_name.c_str(),
                             dzIPC::info_pool::to_string(entry.kind),
                             entry.type_name.c_str(),
                             entry.domain_id);
            }
        }
        std::fprintf(stderr, "\nPublishing aborted — no subscriber to validate message schema against.\n");
        return 1;
    }

    // Use the first subscriber entry
    const auto& sub_entry = subscriber_entries[0];
    std::string type_name = sub_entry.type_name;

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
        std::fprintf(stderr, "Error: subscriber entry has empty type_name. Cannot determine message schema.\n");
        return 1;
    }

    // Determine transport: is subscriber using shm or socket?
    bool use_shm;
    std::string effective_mode = mode;
    if (mode == "auto")
    {
        use_shm = (sub_entry.kind == dzIPC::info_pool::EntryKind::ShmSub);
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
    int domain_id = (domain_id_arg >= 0) ? domain_id_arg : sub_entry.domain_id;
    uint32_t msg_id = (msg_id_arg >= 0) ? static_cast<uint32_t>(msg_id_arg) : 0;

    // ---- Step 2: Find and parse the .msg file ----
    std::string msg_file = find_msg_file(msg_dir, type_name);
    if (msg_file.empty())
    {
        std::fprintf(stderr, "Error: cannot find .msg file for type '%s' in '%s'\n",
                     type_name.c_str(), msg_dir.c_str());
        std::fprintf(stderr, "  Make sure --msg-dir points to the project's msg/ directory.\n");
        return 1;
    }

    std::fprintf(stderr, "Found .msg file: %s\n", msg_file.c_str());

    MsgSchema schema = parse_msg_file(msg_file);
    if (schema.fields.empty())
    {
        std::fprintf(stderr, "Warning: .msg file '%s' has no fields.\n", msg_file.c_str());
    }

    // ---- Step 3: Display schema from .msg file ----
    std::fprintf(stderr, "\n=== Message schema: %s (%zu fields) ===\n",
                 schema.type_name.c_str(), schema.fields.size());
    for (const auto& f : schema.fields)
    {
        std::fprintf(stderr, "  %-24s : %s", f.name.c_str(), field_type_name(f.type));
        if (!f.nested_type.empty())
            std::fprintf(stderr, " (%s)", f.nested_type.c_str());
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "=========================================\n\n");

    // ---- Step 4: Build GenericMessage with default values ----
    dzIPC::GenericMessage msg;
    build_default_message(msg, schema);

    // ---- Step 5: Apply field overrides ----
    for (const auto& spec : field_overrides)
    {
        std::fprintf(stderr, "Applying field override: %s\n", spec.c_str());
        if (!apply_field_override(msg, spec))
        {
            std::fprintf(stderr, "  (continuing anyway)\n");
        }
    }

    // ---- Step 6: Create publisher and publish ----
    dzIPC::IPCType ipc_type = use_shm ? dzIPC::IPC_SHM : dzIPC::IPC_SOCKET;

    auto topic_base = std::make_shared<dzIPC::GenericMessage>(msg);
    topic_base->set_msg_id(msg_id);
    auto topic_data = std::make_shared<dzIPC::TopicData>(topic_base, msg_id);

    auto pub = dzIPC::PublisherIPCPtrMake(topic_data, topic_name, static_cast<size_t>(domain_id), ipc_type, false);

    std::fprintf(stderr, "Publishing to '%s' via %s at %d Hz (msg_id=%u, domain=%d). Ctrl+C to stop.\n\n",
                 topic_name.c_str(), effective_mode.c_str(), freq_hz, msg_id, domain_id);

    int64_t msg_count = 0;
    uint64_t period_us = 1'000'000 / freq_hz;

    while (g_running && (count == 0 || msg_count < count))
    {
        auto start = std::chrono::steady_clock::now();

        auto pub_msg = std::make_shared<dzIPC::GenericMessage>(msg);
        pub_msg->set_msg_id(msg_id);
        bool ok = pub->publish(pub_msg->msgcast<IpcMsgBase>());

        if (ok)
        {
            msg_count++;
            std::fprintf(stderr, "[#%lld] published\n", (long long)msg_count);
        }
        else
        {
            std::fprintf(stderr, "Warning: publish failed for message #%lld\n", (long long)(msg_count + 1));
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed_us =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        if (elapsed_us < period_us)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(period_us - elapsed_us));
        }

        // Periodically re-check that subscribers still exist
        if (msg_count % 10 == 0)
        {
            auto current_entries = pool.snapshot(false);
            bool has_sub = false;
            for (const auto& e : current_entries)
            {
                if (e.topic_name == topic_name && e.alive && e.in_use
                    && (e.kind == dzIPC::info_pool::EntryKind::ShmSub
                        || e.kind == dzIPC::info_pool::EntryKind::SocketSub))
                {
                    has_sub = true;
                    break;
                }
            }
            if (!has_sub)
            {
                std::fprintf(stderr, "\n\033[33mNo subscribers remaining for topic '%s' — stopping.\033[0m\n",
                             topic_name.c_str());
                break;
            }
        }
    }

    std::fprintf(stderr, "Published %lld messages. Done.\n", (long long)msg_count);
    return 0;
}
