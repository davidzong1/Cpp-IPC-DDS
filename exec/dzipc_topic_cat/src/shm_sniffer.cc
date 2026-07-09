// dzipc_topic_cat: a "ros2 topic echo"-style passive sniffer for cpp-ipc
// channels. Attaches to an existing channel by name and prints each message
// without disturbing the publisher (no receiver registration).

#include "shm_sniffer.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace dzIPC {
struct shm_sniffer_options
{
    std::string name;
    std::string pref;
    ipc::sniffer::topology topo = ipc::sniffer::topology::route;
    bool hex = true;
    bool ascii = true;
    std::size_t max = 0;   // 0 = unlimited
    std::uint64_t timeout_ms = ipc::invalid_value;
    std::size_t width = 16;   // bytes per row in hex view
    bool quiet_meta = false;
};

shm_sniffer::shm_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id)
    : sniffer_base(topic_name, domain_id, ser_or_topic, msg_id)
{
    create_sniffer(topic_name, domain_id, ser_or_topic, msg_id);
}

shm_sniffer::~shm_sniffer()
{
    stop_.store(true, std::memory_order_release);
    if (recv_thread_.joinable())
    {
        recv_thread_.join();
    }
}

void shm_sniffer::create_sniffer(const std::string& topic_name, int domain_id, bool ser_or_topic, uint32_t msg_id)
{
    stop_.store(false, std::memory_order_release);
    topic_name_ = topic_name;
    domain_id_ = domain_id;
    this->ser_or_topic_ = ser_or_topic;
    this->msg_id_ = msg_id;
    std::string control_name =
        ser_or_topic_ ? "dz_ipc_" + topic_name + "_ser_control" : "dz_ipc_" + topic_name + "_topic_control";
    if (!control_plane_.open(control_name))
    {
        std::fprintf(stderr, "error: failed to open control plane '%s'\n", control_name.c_str());
        std::exit(1);
    }
    generation_ = control_plane_.generation();
    if (!open_channels(topic_name, domain_id, ser_or_topic))
    {
        std::exit(1);
    }
    ready.store(true, std::memory_order_release);
    recv_thread_ = std::thread(
        [this]()
        {
            while (!stop_.load(std::memory_order_acquire))
            {
                reopen_if_generation_changed();
                sniffer_info got = recv_inner(50);
                // Only refresh the cache when we actually received something, so a
                // timeout in this iteration doesn't clobber a payload that the
                // consumer hasn't picked up yet.
                if (got.request.size() > 0 || got.response.size() > 0)
                {
                    auto msg_cache_cache = std::make_unique<sniffer_info>(std::move(got));
                    std::lock_guard<std::mutex> lock(msg_mutex);
                    msg_cache = std::move(msg_cache_cache);
                }
                std::this_thread::yield();
            }
        });
}

bool shm_sniffer::open_channels(const std::string& topic_name, int, bool ser_or_topic)
{
    // The shm publishers/servers do NOT open a route/server with the raw topic
    // name — they mangle it the same way the regular sub/cli does. The sniffer
    // must apply the exact same scheme to attach to the right SHM region.
    //   topic mode  : "dz_ipc_<topic>_topic"            -> ipc::route
    //   service mode: "dz_ipc_<topic>_ser_r" + "_ser_w" -> ipc::server
    shm_sniffer_options opt;
    opt.pref = "";
    if (ser_or_topic_)
    {
        opt.topo = ipc::sniffer::topology::server;
        opt.name = "dz_ipc_" + topic_name + "_ser_r";
    }
    else
    {
        opt.topo = ipc::sniffer::topology::route;
        opt.name = "dz_ipc_" + topic_name + "_topic";
    }
    req_ = std::make_unique<ipc::sniffer>();
    bool ok = opt.pref.empty() ? req_->open(opt.name.c_str(), opt.topo)
                               : req_->open(ipc::prefix{opt.pref.c_str()}, opt.name.c_str(), opt.topo);
    if (!ok)
    {
        std::fprintf(stderr, "error: failed to open channel '%s' (prefix='%s', topology=%s)\n", opt.name.c_str(),
                     opt.pref.c_str(),
                     opt.topo == ipc::sniffer::topology::server  ? "server"
                     : opt.topo == ipc::sniffer::topology::route ? "route"
                                                                 : "channel");

        return false;
    }
    if (ser_or_topic)
    {
        std::string res_name = "dz_ipc_" + topic_name + "_ser_w";
        res_ = std::make_unique<ipc::sniffer>();
        bool ok = opt.pref.empty()
                      ? res_->open(res_name.c_str(), ipc::sniffer::topology::server)
                      : res_->open(ipc::prefix{opt.pref.c_str()}, res_name.c_str(), ipc::sniffer::topology::server);
        if (!ok)
        {
            std::fprintf(stderr, "error: failed to open channel '%s' (prefix='%s', topology=%s)\n", res_name.c_str(),
                         opt.pref.c_str(), "server");
            return false;
        }
    }
    else
    {
        res_.reset();
    }
    return true;
}

void shm_sniffer::reopen_if_generation_changed()
{
    using dzIPC::control_plane_shm::TopicState;
    if (!control_plane_.valid() || control_plane_.state() != TopicState::Ready)
    {
        return;
    }
    const std::uint32_t current_generation = control_plane_.generation();
    if (current_generation == 0 || current_generation == generation_)
    {
        return;
    }
    if (open_channels(topic_name_, domain_id_, ser_or_topic_))
    {
        generation_ = current_generation;
        std::lock_guard<std::mutex> lock(msg_mutex);
        msg_cache.reset();
    }
}

sniffer_info shm_sniffer::try_recv() noexcept
{
    if (!ready.load(std::memory_order_acquire))
        return sniffer_info{ipc::buffer{}, ipc::buffer{}};
    std::shared_ptr<sniffer_info> info = nullptr;
    {
        std::lock_guard<std::mutex> lock(msg_mutex);
        info = std::move(msg_cache);
    }
    if (info)
    {
        return std::move(*info);
    }
    return sniffer_info{ipc::buffer{}, ipc::buffer{}};
}

sniffer_info shm_sniffer::recv_inner(std::uint64_t timeout_ms) noexcept
{
    if (ser_or_topic_)
    {
        ipc::sniffer::meta m1, m2;
        ipc::buffer req_cache = std::move(req_->recv(timeout_ms, &m1));
        ipc::buffer res_cache = std::move(res_->recv(timeout_ms, &m2));
        return sniffer_info{std::move(req_cache), std::move(res_cache)};
    }
    else
    {
        ipc::sniffer::meta m1;
        return sniffer_info{req_->recv(timeout_ms, &m1), ipc::buffer{}};
    }
}
}   // namespace dzIPC
