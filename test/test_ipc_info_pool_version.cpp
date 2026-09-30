/* [W07] 跨进程版本/布局协商的**混合版本**证据（D-4 约束 2: 拒绝而非静默错解）。
 *
 * 两件事必须证明:
 *   ① 同一段名上若已有**旧布局/旧版本/旧容量**的段, 新进程必须**显式拒绝**并报明确错误,
 *      ⛔不得按旧 size 静默使用 —— 旧段的 entries 数组比本版本短, 按 kMaxEntries 遍历
 *      就是读越界(libipc 的 shm acquire 还会按调用方 size 对既有段 ftruncate)。
 *   ② 布局一变就换段名后缀(kShmName v1 → v2) ⇒ 新代码**不触碰**旧代段; 滚动升级期间
 *      新旧二进制各自只看自己那一代, 不可能互相 resize/错解。
 *
 * ── 为什么每个用例都 fork ──────────────────────────────────────────────────
 * a) `IpcInfoPool::instance()` 是**进程级单例**, 一旦构造就固定指向当时映射的段;
 *    要"先造陈旧段再让池去 attach", 必须在池被构造**之前**完成 —— 全新子进程最干净。
 * b) 本文件的父进程**从不**调用 `IpcInfoPool::instance()`, 因此即使 gtest 乱序/过滤,
 *    每个子进程仍是从"池未构造"的干净状态开始。
 * c) 陈旧段装在**生产段名**上(必须如此才测得到), 所以每个子进程结束前都 unlink 干净,
 *    并且本文件不应与 test_ipc_info_pool*.cpp 并行跑。
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "dzIPC/ipc_info_pool.h"
#include "libipc/shm.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::info_pool;

/* 旧代(kMaxEntries=512)的段字节数 = 56 + 512×296。硬编码在这里是**刻意的**:
 * 它代表"线上还在跑的旧二进制", 不是本版本的编译期常量。 */
constexpr std::size_t kOldLayoutRegionBytes = 151608;
constexpr char kOldShmName[] = "dz_ipc_info_pool_v1";

struct ChildRun
{
    bool exited{false};
    int code{-1};
    int signal{0};
    std::string text;
    std::string err;
};

void emit(int fd, const std::string& line)
{
    const std::string s = line + "\n";
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(s.size()))
    {
        const ssize_t w = ::write(fd, s.data() + off, s.size() - static_cast<std::size_t>(off));
        if (w <= 0) return;
        off += w;
    }
}

std::string read_all(int fd)
{
    std::string s;
    char buf[2048];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) s.append(buf, static_cast<std::size_t>(n));
    return s;
}

/* fork 子进程执行 body; 同时把子进程的 stderr 收进管道(用来断言"报了明确错误")。 */
ChildRun run_child_capture_stderr(const std::function<void(int)>& body)
{
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    if (::pipe(out) != 0 || ::pipe(err) != 0) return {};
    const ::pid_t pid = ::fork();
    if (pid < 0) return {};
    if (pid == 0)
    {
        ::close(out[0]);
        ::close(err[0]);
        ::dup2(err[1], STDERR_FILENO);
        ::close(err[1]);
        int rc = 1;
        try
        {
            body(out[1]);
            rc = 0;
        }
        catch (...)
        {
            rc = 9;
        }
        ::_exit(rc);
    }
    ::close(out[1]);
    ::close(err[1]);

    ChildRun r;
    r.text = read_all(out[0]);
    r.err = read_all(err[0]);
    ::close(out[0]);
    ::close(err[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        r.exited = true;
        r.code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        r.signal = WTERMSIG(status);
    }
    return r;
}

/* 造一个"别的布局"的段: 先按旧 size 建段, 再按**公开的段头偏移契约**写 magic/version/
 * max_entries(init_state@0 置 kInitReady=2, 否则待测进程会当成"未初始化")。 */
bool install_stale_segment(const char* name, std::size_t bytes, std::uint32_t magic,
                           std::uint32_t version, std::uint32_t max_entries)
{
    ipc::shm::remove(name);
    ipc::shm::handle h;
    if (!h.acquire(name, bytes, ipc::shm::create)) return false;
    auto* p = static_cast<std::uint8_t*>(h.get());
    if (p == nullptr) return false;
    const std::uint32_t head[4] = {2u, magic, version, max_entries};
    std::memcpy(p, head, sizeof(head));
    /* ⛔ release_no_unlink 而非普通 release: 后者在引用计数归零时会 unlink 段名,
     * 待测进程就 attach 不到了。这里要的是"文件还在、没人映射"。 */
    h.release_no_unlink();
    return true;
}

struct FileProbe
{
    bool exists{false};
    long long size{0};
    std::uint8_t prefix[16]{};
};

FileProbe probe_shm_file(const char* name)
{
    FileProbe p;
    char path[256];
    std::snprintf(path, sizeof(path), "/dev/shm/%s", name);
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return p;
    struct stat st{};
    if (::fstat(fd, &st) == 0) p.size = static_cast<long long>(st.st_size);
    const ssize_t n = ::read(fd, p.prefix, sizeof(p.prefix));
    ::close(fd);
    p.exists = (n == static_cast<ssize_t>(sizeof(p.prefix)));
    return p;
}

std::string kv(const std::string& text, const std::string& key)
{
    const std::string pat = key + "=";
    const std::size_t pos = text.find(pat);
    if (pos == std::string::npos) return {};
    const std::size_t b = pos + pat.size();
    const std::size_t e = text.find_first_of(" \n", b);
    return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

void cleanup_segments()
{
    ipc::shm::remove(kOldShmName);
    ipc::shm::remove("dz_ipc_info_pool_v2");
}

}   // namespace

/* ① 同段名 + 旧 layout_ver + 旧 max_entries ⇒ 必须拒绝(register_entry 返回 -1)且报明确
 * 错误, ⛔不是"按旧 size 静默用"、也不是段错误。 */
TEST(IpcInfoPoolVersion, StaleOldLayoutOnCurrentNameIsRejectedNotMisread)
{
    const ChildRun r = run_child_capture_stderr([](int out) {
        cleanup_segments();
        if (!install_stale_segment("dz_ipc_info_pool_v2", kOldLayoutRegionBytes, kLayoutMagic, 1u, 512u))
        {
            emit(out, "installed=0");
            return;
        }
        const int32_t slot = IpcInfoPool::instance().register_entry(
            {EntryKind::SocketSub, "w07_stale", "", "", 0, ""});
        emit(out, "installed=1 slot=" + std::to_string(slot));
        cleanup_segments();
    });

    ASSERT_TRUE(r.exited) << "子进程异常退出 signal=" << r.signal << " err=" << r.err;
    ASSERT_EQ(r.code, 0) << r.err;
    ASSERT_EQ(kv(r.text, "installed"), "1") << r.text;
    EXPECT_LT(std::strtol(kv(r.text, "slot").c_str(), nullptr, 10), 0)
        << "旧布局段被静默使用了 —— 必须拒绝: " << r.text;
    /* "可识别": 拒绝要带**具体**原因, 而不是笼统的池未就绪。 */
    EXPECT_NE(r.err.find("拒绝使用既有段"), std::string::npos) << "缺显式拒绝诊断: " << r.err;
    EXPECT_NE(r.err.find("layout_ver"), std::string::npos) << "诊断未指明版本不符: " << r.err;
    EXPECT_NE(r.err.find("max_entries"), std::string::npos) << "诊断未指明容量不符: " << r.err;
}

/* ② 旧代段名(v1)与新代(v2)物理隔离: 新代码不触碰旧段; 新池在 v2 上正常工作。 */
TEST(IpcInfoPoolVersion, OldGenerationSegmentIsNotTouchedByNewLayout)
{
    const ChildRun r = run_child_capture_stderr([](int out) {
        cleanup_segments();
        /* 旧代段: 旧 size + 旧版本号 + 旧容量, 前缀当哨兵。 */
        if (!install_stale_segment(kOldShmName, kOldLayoutRegionBytes, kLayoutMagic, 1u, 512u))
        {
            emit(out, "installed=0");
            return;
        }
        const FileProbe before = probe_shm_file(kOldShmName);

        /* 新进程只用 v2。 */
        const int32_t slot = IpcInfoPool::instance().register_entry(
            {EntryKind::SocketSub, "w07_iso", "", "", 0, ""});

        const FileProbe after = probe_shm_file(kOldShmName);
        const FileProbe v2 = probe_shm_file("dz_ipc_info_pool_v2");

        emit(out, "installed=1 slot=" + std::to_string(slot) + " before_size=" +
                      std::to_string(before.size) + " after_size=" + std::to_string(after.size) +
                      " prefix_same=" +
                      std::to_string(before.exists && after.exists &&
                                     std::memcmp(before.prefix, after.prefix, sizeof(before.prefix)) == 0)
                          + " v2_exists=" + std::to_string(v2.exists ? 1 : 0));
        cleanup_segments();
    });

    ASSERT_TRUE(r.exited) << "子进程异常退出 signal=" << r.signal << " err=" << r.err;
    ASSERT_EQ(r.code, 0) << r.err;
    ASSERT_EQ(kv(r.text, "installed"), "1") << r.text;
    EXPECT_GE(std::strtol(kv(r.text, "slot").c_str(), nullptr, 10), 0)
        << "新代(v2)池应正常工作: " << r.text;
    EXPECT_EQ(kv(r.text, "v2_exists"), "1") << "新代段未创建: " << r.text;
    EXPECT_EQ(kv(r.text, "prefix_same"), "1") << "旧代段前缀被改动 —— 新旧代未隔离: " << r.text;
    EXPECT_EQ(kv(r.text, "before_size"), kv(r.text, "after_size"))
        << "旧代段大小被改动(被 ftruncate resize) —— 新旧代未隔离: " << r.text;
    /* 旧段文件大小 = libipc 的 calc_size(region) = region + sizeof(info_t)(4)
     * (region 已按 4 对齐), 即"谁也没动过它"。 */
    EXPECT_EQ(std::strtoll(kv(r.text, "after_size").c_str(), nullptr, 10),
              static_cast<long long>(kOldLayoutRegionBytes) + 4)
        << "旧代段应是未被动过的原大小: " << r.text;
}
