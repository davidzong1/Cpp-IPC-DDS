#include "shared_net/runtime_fixture.h"
#include "gtest/gtest.h"
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
using namespace shared_net_test;
namespace
{
struct Process
{
    pid_t pid = -1;
    explicit Process(const GatewayConfig &c)
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
        if (pid == 0)
        {
            execv(argv[0], argv.data());
            _exit(127);
        }
        if (pid < 0)
            throw std::runtime_error("测试子进程创建失败");
    }
    void kill_and_wait()
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
    ~Process()
    {
        kill_and_wait();
    }
};
} // namespace
TEST(SharedNetGatewayLock, ProcessExclusionStoppedOwnerAndStaleRestart)
{
    Directory dir;
    auto c = configuration(dir);
    Process owner(c);
    ASSERT_TRUE(until([&] { return std::filesystem::exists(dir.control()); }));
    RawClient first(dir.control());
    const auto epoch = first.header.gateway_epoch;
    EXPECT_THROW(local::Listener contender(dir.control()), std::system_error);
    ASSERT_EQ(kill(owner.pid, SIGSTOP), 0);
    int state = 0;
    ASSERT_EQ(waitpid(owner.pid, &state, WUNTRACED), owner.pid);
    ASSERT_TRUE(WIFSTOPPED(state));
    EXPECT_THROW(local::Listener contender(dir.control()), std::system_error);
    owner.kill_and_wait();
    ASSERT_TRUE(std::filesystem::exists(dir.control()));
    Process restarted(c);
    ASSERT_TRUE(until([&] {
        try
        {
            RawClient next(dir.control());
            return next.header.gateway_epoch != epoch;
        }
        catch (...)
        {
            return false;
        }
    }));
    ASSERT_EQ(kill(restarted.pid, SIGTERM), 0);
    ASSERT_TRUE(until([&] { return !std::filesystem::exists(dir.control()); }));
    ASSERT_EQ(waitpid(restarted.pid, &state, 0), restarted.pid);
    restarted.pid = -1;
    EXPECT_TRUE(WIFEXITED(state));
    EXPECT_EQ(WEXITSTATUS(state), 0);
}
TEST(SharedNetGatewayLock, DirectoryLockSocketModesAndUnsafePaths)
{
    Directory dir;
    {
        local::Listener listener(dir.control());
        struct stat st
        {
        };
        ASSERT_EQ(stat(dir.control().c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 0777, 0600);
        ASSERT_EQ(stat((dir.path + "/gateway.lock").c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 0777, 0600);
    }
    EXPECT_FALSE(std::filesystem::exists(dir.control()));
    ASSERT_EQ(chmod(dir.path.c_str(), 0755), 0);
    EXPECT_THROW(local::Listener listener(dir.control()), std::runtime_error);
    ASSERT_EQ(chmod(dir.path.c_str(), 0700), 0);
    ASSERT_EQ(chmod((dir.path + "/gateway.lock").c_str(), 0644), 0);
    EXPECT_THROW(local::Listener listener(dir.control()), std::runtime_error);
    ASSERT_EQ(unlink((dir.path + "/gateway.lock").c_str()), 0);
    ASSERT_EQ(symlink("/dev/null", (dir.path + "/gateway.lock").c_str()), 0);
    EXPECT_THROW(local::Listener listener(dir.control()), std::system_error);
    Directory parent;
    ASSERT_EQ(symlink(dir.path.c_str(), (parent.path + "/alias").c_str()), 0);
    EXPECT_THROW(local::Listener listener(parent.path + "/alias/control.sock"), std::system_error);
}
TEST(SharedNetGatewayLock, RejectSocketSymlinkAndDoNotDeleteReplacement)
{
    Directory dir;
    ASSERT_EQ(symlink("/dev/null", dir.control().c_str()), 0);
    EXPECT_THROW(local::Listener listener(dir.control()), std::runtime_error);
    ASSERT_EQ(unlink(dir.control().c_str()), 0);
    {
        local::Listener listener(dir.control());
        ASSERT_EQ(unlink(dir.control().c_str()), 0);
        local::Fd file(open(dir.control().c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600));
        ASSERT_TRUE(file);
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(dir.control()));
}
TEST(SharedNetGatewayLock, FailedUdpStartupReleasesLockAndSocket)
{
    Directory first, second;
    auto c = configuration(first);
    GatewayRuntime gateway(c);
    c.control_path = second.control();
    EXPECT_THROW(GatewayRuntime other(c), std::system_error);
    EXPECT_FALSE(std::filesystem::exists(second.control()));
    EXPECT_NO_THROW(local::Listener listener(second.control()));
}
