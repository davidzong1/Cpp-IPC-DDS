#include "dzIPC/ipc_info_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options
{
    bool one_shot{true};
    bool show_all_kinds{true};
    bool verbose{false};
    bool clear_pool{false};
    int interval_ms{500};
    std::string filter_kind;  /* shm_pub / socket_sub / ... */
    std::string filter_topic; /* substring */
};

void print_help(const char* prog)
{
    std::cout << "usage: " << prog << " [options]\n"
              << "  -w, --watch            持续刷新（类似 top）\n"
              << "  -i, --interval <ms>    刷新间隔，默认 500\n"
              << "  -k, --kind <kind>      只显示指定 kind，例如 shm_pub / socket_sub\n"
              << "  -t, --topic <str>      按 topic 子串过滤\n"
              << "  -v, --verbose          显示 extra/timestamps 等详细字段\n"
              << "      --reset            清空共享内存池（慎用，需所有 dzIPC 进程已退出）\n"
              << "  -h, --help             本帮助\n";
}

int parse_args(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            print_help(argv[0]);
            return 1;
        }
        else if (a == "-w" || a == "--watch")
        {
            opt.one_shot = 0;
        }
        else if ((a == "-i" || a == "--interval") && i + 1 < argc)
        {
            opt.interval_ms = std::max(100, std::atoi(argv[++i]));
        }
        else if ((a == "-k" || a == "--kind") && i + 1 < argc)
        {
            opt.filter_kind = argv[++i];
            opt.show_all_kinds = false;
        }
        else if ((a == "-t" || a == "--topic") && i + 1 < argc)
        {
            opt.filter_topic = argv[++i];
        }
        else if (a == "-v" || a == "--verbose")
        {
            opt.verbose = true;
        }
        else if (a == "--reset")
        {
            opt.clear_pool = true;
        }
        else
        {
            std::cerr << "unknown arg: " << a << "\n";
            print_help(argv[0]);
            return 0;
        }
    }
    return 2;
}

std::string format_age(int64_t register_ts_ns)
{
    if (register_ts_ns == 0)
        return "-";
    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    int64_t age_s = (now_ns - register_ts_ns) / 1'000'000'000LL;
    if (age_s < 0)
        age_s = 0;
    char buf[32];
    if (age_s < 60)
        std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(age_s));
    else if (age_s < 3'600)
        std::snprintf(buf, sizeof(buf), "%lldm%02llds", static_cast<long long>(age_s / 60),
                      static_cast<long long>(age_s % 60));
    else
        std::snprintf(buf, sizeof(buf), "%lldh%02lldm", static_cast<long long>(age_s / 3'600),
                      static_cast<long long>((age_s / 60) % 60));
    return buf;
}

void print_snapshot(const std::vector<dzIPC::info_pool::EntrySnapshot>& entries, const Options& opt)
{
    /* 过滤 */
    std::vector<const dzIPC::info_pool::EntrySnapshot*> view;
    view.reserve(entries.size());
    for (const auto& e : entries)
    {
        if (!opt.filter_kind.empty() && opt.filter_kind != dzIPC::info_pool::to_string(e.kind))
            continue;
        if (!opt.filter_topic.empty() && e.topic_name.find(opt.filter_topic) == std::string::npos)
            continue;
        view.push_back(&e);
    }

    std::sort(view.begin(), view.end(),
              [](const auto* a, const auto* b)
              {
                  if (a->kind != b->kind)
                      return a->kind < b->kind;
                  if (a->topic_name != b->topic_name)
                      return a->topic_name < b->topic_name;
                  return a->pid < b->pid;
              });
    printf("%-4s%-15s%-8s%-8s%-32s%-32s%-50s\n", "ID", "KIND", "PID", "AGE", "TOPIC", "TYPE", "EXTRA");
    printf("%s\n", std::string(100, '-').c_str());

    for (const auto* e : view)
    {
        if (e->alive && e->in_use)
        {
            printf("%-4d%-15s%-8d%-8s%-32s%-32s%-50s\n", e->slot, dzIPC::info_pool::to_string(e->kind), e->pid,
                   format_age(e->register_ts_ns).c_str(), e->topic_name.c_str(),
                   (e->type_name.empty() ? "-" : e->type_name.c_str()), (e->extra.empty() ? "-" : e->extra.c_str()));
        }
    }

    printf("\nTotal: %zu %s\n", view.size(), (view.size() == 1 ? "entry" : "entries"));
}

}   // namespace

int main(int argc, char** argv)
{
    Options opt;
    int parse_result = parse_args(argc, argv, opt);
    if (parse_result != 2)
    {
        if (parse_result == 1)
            return 0;
        else
            return 1;
    }

    if (opt.clear_pool)
    {
        dzIPC::info_pool::IpcInfoPool::reset_storage();
        std::cout << "info pool cleared." << std::endl;
        return 0;
    }

    auto& pool = dzIPC::info_pool::IpcInfoPool::instance();
    if (opt.one_shot)
    {
        auto entries = pool.snapshot(true);
        print_snapshot(entries, opt);
        return 0;
    }

    /* watch 模式 */
    while (true)
    {
        std::cout << "\x1b[2J\x1b[H"; /* clear screen + cursor home */
        auto entries = pool.snapshot(true);
        print_snapshot(entries, opt);
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));
    }
    return 0;
}
