#pragma once
#include "dzIPC/net/outbox.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "libipc/ipc.h"
#include "runtime_fixture.h"
#include <csignal>
#include <cstring>
#include <sys/wait.h>

namespace shared_net_test
{
struct ChildGuard
{
    pid_t pid;
    explicit ChildGuard(pid_t p) : pid(p)
    {
    }
    ~ChildGuard()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
            {
            }
        }
    }
};
inline Bytes flat_blob(std::size_t size, std::uint64_t sequence = 1)
{
    Bytes data(size);
    dzflat::SegHeader h{
        dzflat::kMagic, 0xabcdef01, 32, 8, static_cast<std::uint32_t>(size), 1, 0, 71, 0};
    std::memcpy(data.data(), &h, sizeof(h));
    for (std::size_t i = sizeof(h); i < size; ++i)
        data[i] = static_cast<unsigned char>((i * 31 + sequence) % 251);
    return data;
}
inline OutboxHeader outbox_header(std::uint64_t sequence = 1)
{
    OutboxHeader h;
    h.session_id = 1;
    h.gateway_epoch = 2;
    h.publisher_id[0] = 1;
    h.sequence = sequence;
    std::memcpy(h.route.scope.data(), "DZS2", 4);
    h.route.scope[7] = 1;
    h.route.msg_id = 71;
    h.encoding = Encoding::DzFlat;
    h.schema_hash = 0xabcdef01;
    return h;
}
struct OutboxFixture
{
    WelcomeBody welcome;
    SendBudget budget{{32 * 1024 * 1024, 512}, {32 * 1024 * 1024, 512}};
    std::shared_ptr<SendAccount> account;
    std::unique_ptr<OutboxSender> sender;
    std::unique_ptr<OutboxReceiver> receiver;
    explicit OutboxFixture(CreditCounters initial = {32 * 1024 * 1024, 512})
    {
        welcome.locality = local::random_identity();
        welcome.tx_name = outbox_name(welcome.locality, 2, 1);
        welcome.granted_bytes = initial.bytes;
        welcome.granted_records = initial.records;
        account = budget.open(initial);
        sender = std::make_unique<OutboxSender>(welcome, 1, 2);
    }
    void attach()
    {
        receiver = std::make_unique<OutboxReceiver>(welcome, 1, 2, sender->event_fd(), account);
        sender->ready();
    }
    void update()
    {
        sender->progress(receiver->progress());
        sender->grant(account->granted());
    }
    ~OutboxFixture()
    {
        receiver.reset();
        sender.reset();
        account->close();
        ipc::route::clear_storage(ipc::prefix{welcome.tx_name.c_str()}, welcome.tx_name.c_str());
    }
};
} // namespace shared_net_test
#ifdef DZIPC_GATEWAY_BIN
namespace shared_net_test
{
struct GatewayProcess : ChildGuard
{
    explicit GatewayProcess(const GatewayConfig &c) : ChildGuard(-1)
    {
        std::vector<std::string> args{DZIPC_GATEWAY_BIN,  "serve",
                                      "--listen-ip",      c.listen_ip,
                                      "--interface",      c.interface,
                                      "--control",        c.control_path,
                                      "--data-base-port", std::to_string(c.data_base_port),
                                      "--control-port",   std::to_string(c.control_port),
                                      "--discovery-port", std::to_string(c.discovery_port)};
        std::vector<char *> argv;
        for (auto &s : args)
            argv.push_back(s.data());
        argv.push_back(nullptr);
        pid = fork();
        if (!pid)
        {
            execv(argv[0], argv.data());
            _exit(127);
        }
        if (pid < 0)
            throw std::runtime_error("网关测试进程创建失败");
    }
    void stop_now()
    {
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
            {
            }
            pid = -1;
        }
    }
};
} // namespace shared_net_test
#endif
