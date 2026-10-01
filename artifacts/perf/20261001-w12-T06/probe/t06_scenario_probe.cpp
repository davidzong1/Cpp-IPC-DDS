/* T06 / W12 §8 W09 独立复核 —— **四类场景布景探针**（自建量具，非官方工装）。
 *
 * 为什么需要它：官方常驻工装 `test_chunk_capacity_backpressure` 把 §8 的四类场景
 * 揉在**同一个 1200 轮用例**（C9）里，只能给出一个总判定，无法**逐类隔离布景**
 * 与逐类取证。本探针把四类场景各自**独立布景**，每类给出可核对的读数：
 *
 *   fresh  全新段（段文件不存在）：首次 attach + 借 1 块 ⇒ 判「不触发 orphan reset」。
 *   live   段有**活**持有者：另一进程持有 1 块且**始终存活**，本进程首次 attach 该段
 *          ⇒ 判「不复位」。判据不止日志：若发生错误复位，复位链从头开始 ⇒ 本进程
 *          借到的 id 必然是持有者的 id ⇒ 持有者的 64 字节会被改写（MUTATED）
 *          ⇒ 本场景对错误复位是**确定性**敏感的（不是概率性的）。
 *   dead   段内持有者已**死**：种子进程借 3 块后 `_exit`（不归还）⇒ 段留存且空闲链
 *          非素净（pre_chain=37）；被测进程 attach 后**能不能借满 40 块**是「允许复位」
 *          的**行为判据**（不复位则最多只能借到 37）。
 *   race   reclaim 期间同进程另一线程 acquire：每轮「种子进程 → 8 话题并发首借 worker」
 *          （与 C9 同构）；同时把 worker 进程自己的 `orphan segment reset` 行数逐轮
 *          记账 ⇒ 可直接观察「探活进入但复核放弃」（opendir 次数 > reset 次数）。
 *
 * 段状态的读取**不用产品 API**：段是 tmpfs 文件，chunk_info_t 的首成员 id_pool<>
 * 的 next_[capacity] 占文件头 capacity 字节、cursor_ 紧随其后（见 C8 用例注释与
 * docs/shm_chunk_pool_occupancy_plan.md §3 步骤②）。
 *
 * ⛔ 本探针不链接产品库之外的东西，不修改任何产品/工装文件；只读产品头文件与库。
 * ⛔ helper 子进程一律把 stdout 重定向到 /dev/null（`_exit` 不刷新 stdio）⇒
 *    `orphan segment reset` 行**只可能**来自本场景的被测进程，逐进程归属明确。
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libipc/ipc.h"

namespace {

constexpr std::size_t kLoanSize = 8000;   /* ⇒ chunk 档 9216 */
constexpr std::size_t kChunkClass = 9216;
constexpr std::size_t kCap = 40;          /* ipc::large_msg_cache */

std::string g_log_dir = "artifacts/perf/20261001-w12-T06/scenarios";

std::string seg_path(std::string const &prefix)
{
    return "/dev/shm/" + prefix + "__IPC_SHM__CHUNK_INFO__" + std::to_string(kChunkClass) +
           "__C" + std::to_string(kCap);
}

/* ── 段状态直读（不调用产品 API）────────────────────────────────────────── */
struct SegState
{
    bool exists = false;
    std::size_t size = 0;
    unsigned cursor = 0;
    std::size_t chain = 0;      /* 从 cursor_ 沿 next_ 走到的步数 = 空闲块数 */
    bool chain_broken = false;
    bool all_zero = false;
    int first_nonzero = -1;
};

SegState seg_state(std::string const &prefix)
{
    SegState s;
    std::ifstream f(seg_path(prefix), std::ios::binary);
    if (!f) return s;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    s.exists = true;
    s.size = b.size();
    if (b.size() < kCap + 1) return s;
    s.cursor = b[kCap];
    unsigned cur = s.cursor;
    std::size_t steps = 0;
    while (cur < kCap && steps <= kCap) { cur = b[cur]; ++steps; }
    s.chain = steps;
    s.chain_broken = (steps > kCap);
    s.all_zero = true;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (b[i] != 0) { s.all_zero = false; s.first_nonzero = (int)i; break; }
    return s;
}

void print_seg(char const *tag, std::string const &prefix, SegState const &s)
{
    std::printf("T06_SEG tag=%s prefix=%s exists=%d size=%zu cursor=%u chain=%zu broken=%d "
                "all_zero=%d first_nonzero=%d\n",
                tag, prefix.c_str(), s.exists ? 1 : 0, s.size, s.cursor, s.chain,
                s.chain_broken ? 1 : 0, s.all_zero ? 1 : 0, s.first_nonzero);
    std::fflush(nullptr);
}

/* ── 自指纹：实际加载的库路径 + SHA-256（不依赖外部脚本转述）─────────────── */
std::string loaded_lib_path()
{
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line))
    {
        auto pos = line.find('/');
        if (pos == std::string::npos) continue;
        std::string p = line.substr(pos);
        if (p.find("libipc.so") == std::string::npos) continue;
        char buf[4096];
        ssize_t n = ::readlink(p.c_str(), buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; return std::string(buf); }
        return p;
    }
    return std::string{};
}

std::string sha256_of(std::string const &path)
{
    std::string cmd = "sha256sum '" + path + "' 2>/dev/null";
    FILE *p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) return std::string{};
    char buf[256] = {0};
    if (::fgets(buf, sizeof(buf), p) == nullptr) { ::pclose(p); return std::string{}; }
    ::pclose(p);
    std::string s{buf};
    auto sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

/* ── 子进程工具 ─────────────────────────────────────────────────────────── */
bool redirect_stdout(std::string const &path)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return false;
    ::dup2(fd, 1);
    ::close(fd);
    return true;
}

/* 逐级 mkdir（量具自检：日志目录不存在 ⇒ 子进程 stdout 重定向静默失败 ⇒ 读数丢失）。 */
void ensure_dir(std::string const &dir)
{
    std::string acc;
    for (std::size_t i = 0; i < dir.size(); ++i)
    {
        acc += dir[i];
        if (dir[i] == '/' || i + 1 == dir.size())
        {
            if (acc.size() > 1 && acc != "/") ::mkdir(acc.c_str(), 0755);
        }
    }
}

/* 在被重定向到 `log` 的子进程里执行 fn；返回子进程退出码。
 * ⛔ fn 内部必须 fflush；本函数在 _exit 前会再 fflush 一次。 */
int run_child(std::string const &log, std::function<int()> fn)
{
    const ::pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        if (!log.empty() && !redirect_stdout(log))
        {
            std::fprintf(stderr, "T06_MEAS_ERR child_stdout_redirect_failed log=%s\n", log.c_str());
            ::_exit(120);
        }
        int rc = 1;
        try { rc = fn(); } catch (...) { rc = 1; }
        std::fflush(nullptr);
        ::_exit(rc);
    }
    int st = 0;
    if (::waitpid(pid, &st, 0) != pid) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

bool file_exists(std::string const &p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

std::string read_text(std::string const &p)
{
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

/* 统计文件里 "orphan segment reset" 出现次数（只算 [from, end) 区段）。 */
std::size_t count_reset_lines(std::string const &path, std::size_t from = 0)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    f.seekg(0, std::ios::end);
    std::size_t n = (std::size_t)f.tellg();
    if (n <= from) return 0;
    f.seekg((std::streamoff)from);
    std::string s((std::size_t)(n - from), '\0');
    f.read(&s[0], (std::streamsize)s.size());
    std::size_t cnt = 0, pos = 0;
    const std::string needle = "orphan segment reset";
    while ((pos = s.find(needle, pos)) != std::string::npos) { ++cnt; pos += needle.size(); }
    return cnt;
}

std::size_t file_size(std::string const &path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (std::size_t)st.st_size;
}

std::string topic(std::string const &tag, int r, int i)
{
    return "t06_" + tag + "_" + std::to_string(r) + "_" + std::to_string(i);
}

struct RoutePair
{
    std::unique_ptr<ipc::route> rx, tx;
    bool open(std::string const &prefix, std::string const &t, unsigned tm = 3000)
    {
        rx.reset(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::receiver});
        tx.reset(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::sender});
        return tx->wait_for_recv(1, tm);
    }
};

/* ── 场景 fresh：全新段不触发 orphan reset ──────────────────────────────── */
int scen_fresh(std::string const &prefix, int rounds)
{
    std::string const log = g_log_dir + "/fresh.log";
    int ok = 0, reset_rounds = 0;
    for (int r = 0; r < rounds; ++r)
    {
        ::unlink(seg_path(prefix).c_str());
        bool const pre_absent = !file_exists(seg_path(prefix));
        const std::size_t sz0 = file_size(log);
        int const rc = run_child(log, [&]() -> int
        {
            RoutePair p;
            if (!p.open(prefix, topic("fresh", r, 0))) { std::printf("T06_FRESH round=%d open_failed\n", r); return 3; }
            auto lo = p.tx->loan(kLoanSize);
            if (!lo.valid()) { std::printf("T06_FRESH round=%d loan_invalid\n", r); return 2; }
            SegState const s1 = seg_state(prefix);
            std::printf("T06_FRESH round=%d prefix=%s pre_absent=%d id=%d chain_after_borrow=%zu cursor=%u\n",
                        r, prefix.c_str(), pre_absent ? 1 : 0, (int)lo.id, s1.chain, s1.cursor);
            p.tx->discard_loan(lo);
            SegState const s2 = seg_state(prefix);
            std::printf("T06_FRESH round=%d chain_after_discard=%zu cursor=%u\n", r, s2.chain, s2.cursor);
            std::fflush(nullptr);
            return 0;
        });
        std::size_t const rs = count_reset_lines(log, sz0);
        bool const measured = (file_size(log) > sz0) && (rc != 120);
        if (!measured)
        {
            std::printf("T06_MEAS_ERR scenario=fresh round=%d rc=%d log=%s reason=no_child_output\n", r,
                        rc, log.c_str());
            return 1;
        }
        reset_rounds += (rs != 0) ? 1 : 0;
        if (rc == 0) ++ok;
        std::printf("T06_FRESH round=%d rc=%d pre_absent=%d reset_lines=%zu\n", r, rc,
                    pre_absent ? 1 : 0, rs);
        std::fflush(nullptr);
    }
    std::printf("T06_FRESH_RESULT rounds=%d ok=%d rounds_with_reset=%d reset_lines_total=%zu "
                "log=%s\n",
                rounds, ok, reset_rounds, count_reset_lines(log), log.c_str());
    std::fflush(nullptr);
    return (ok == rounds && reset_rounds == 0) ? 0 : 1;
}

/* ── 场景 live：段有活持有者 ⇒ 不复位 ──────────────────────────────────── */
int scen_live(std::string const &prefix, int rounds)
{
    std::string const log = g_log_dir + "/live.log";
    int subject_ok = 0, subject_reset_rounds = 0, holder_ok = 0, alias_rounds = 0;
    for (int r = 0; r < rounds; ++r)
    {
        ::unlink(seg_path(prefix).c_str());
        std::string const hinfo = g_log_dir + "/live_holder_info_" + std::to_string(r) + ".txt";
        std::string const hres = g_log_dir + "/live_holder_result_" + std::to_string(r) + ".txt";
        std::string const sres = g_log_dir + "/live_subject_result_" + std::to_string(r) + ".txt";
        ::unlink(hinfo.c_str());
        ::unlink(hres.c_str());
        ::unlink(sres.c_str());

        int ctrl[2];
        if (::pipe(ctrl) != 0) return 1;

        /* ① 活持有者：借 1 块 + 写 64B 0xA5，然后**一直活着**等父进程放行。 */
        const ::pid_t hp = ::fork();
        if (hp == 0)
        {
            redirect_stdout("/dev/null");
            ::close(ctrl[1]);
            {
                RoutePair p;
                if (!p.open(prefix, topic("liveh", r, 0))) { ::close(ctrl[0]); ::_exit(4); }
                auto lo = p.tx->loan(kLoanSize);
                if (!lo.valid()) { ::close(ctrl[0]); ::_exit(5); }
                std::memset(lo.data, 0xA5, 64);
                {
                    std::ofstream o(hinfo);
                    o << "holder_pid=" << (int)::getpid() << " chunk_id_h=" << (int)lo.id << "\n";
                }
                /* 等父进程关掉读端（EOF）⇒ 在此之前持有者始终存活、贷款始终在飞。 */
                char c = 0;
                (void)!::read(ctrl[0], &c, 1);
                ::close(ctrl[0]);
                /* 复核自己的 64 字节是否被别人的借样改写（别名/错误复位的确定性证据）。 */
                unsigned char buf[64];
                std::memcpy(buf, lo.data, sizeof(buf));
                int mutated = 0;
                for (unsigned char v : buf) if (v != 0xA5) { mutated = 1; break; }
                {
                    std::ofstream o(hres);
                    o << "holder_pid=" << (int)::getpid() << " chunk_id_h=" << (int)lo.id
                      << " verdict=" << (mutated ? "MUTATED" : "UNCHANGED") << "\n";
                }
                p.tx->discard_loan(lo);
            }
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(ctrl[0]);
        /* 等持有者把 info 落盘（段已存在、1 块在飞）。 */
        for (int i = 0; i < 3000 && !file_exists(hinfo); ++i) ::usleep(1000);

        SegState const before = seg_state(prefix);
        print_seg("live_before_subject", prefix, before);

        const std::size_t sz0 = file_size(log);
        int const rc = run_child(log, [&]() -> int
        {
            RoutePair p;
            if (!p.open(prefix, topic("live", r, 0))) { std::printf("T06_LIVE round=%d open_failed\n", r); return 3; }
            SegState const s_in = seg_state(prefix);
            auto lo = p.tx->loan(kLoanSize);
            if (!lo.valid()) { std::printf("T06_LIVE round=%d loan_invalid\n", r); return 2; }
            std::memset(lo.data, 0x5A, 64);
            SegState const s_mid = seg_state(prefix);
            std::printf("T06_LIVE round=%d subject_id=%d chain_on_attach=%zu cursor_on_attach=%u "
                        "chain_after_borrow=%zu\n",
                        r, (int)lo.id, s_in.chain, s_in.cursor, s_mid.chain);
            {
                std::ofstream o(sres);
                o << "subject_pid=" << (int)::getpid() << " chunk_id_s=" << (int)lo.id << "\n";
            }
            p.tx->discard_loan(lo);
            SegState const s_end = seg_state(prefix);
            std::printf("T06_LIVE round=%d chain_after_discard=%zu\n", r, s_end.chain);
            std::fflush(nullptr);
            return 0;
        });
        std::size_t const rs = count_reset_lines(log, sz0);
        if ((file_size(log) <= sz0) || rc == 120)
        {
            std::printf("T06_MEAS_ERR scenario=live round=%d rc=%d log=%s reason=no_child_output\n", r,
                        rc, log.c_str());
            ::close(ctrl[1]);
            (void)::waitpid(hp, nullptr, 0);
            return 1;
        }

        /* 放行持有者 ⇒ 它复核自己被写过的 64 字节。 */
        ::close(ctrl[1]);
        int hst = 0;
        (void)::waitpid(hp, &hst, 0);

        std::string const hi = read_text(hinfo);
        std::string const hr = read_text(hres);
        std::string const sj = read_text(sres);
        int const holder_verdict_ok = hr.find("verdict=UNCHANGED") != std::string::npos;
        /* 跨进程比较 data 指针无意义（各自地址空间）⇒ 只比 **chunk id**（同一池内的块号）。
         * 主判据 = 持有者的 64 字节有没有被别人的借样改写（确定性），chunk id 相等是辅证。 */
        /* ⛔ 键名必须唯一：用 "pid=" 里的 "id=" 会误匹配（实测把 pid 当成 chunk id）。 */
        auto parse_key = [](std::string const &s, char const *key) -> int
        {
            std::size_t p = s.find(key);
            return (p == std::string::npos) ? -1 : std::atoi(s.c_str() + p + std::strlen(key));
        };
        int const holder_id = parse_key(hi, "chunk_id_h=");
        int const subject_chunk_id = parse_key(sj, "chunk_id_s=");
        int const same = (subject_chunk_id >= 0 && subject_chunk_id == holder_id) ? 1 : 0;
        alias_rounds += same;

        std::printf("T06_LIVE round=%d subject_rc=%d subject_reset_lines=%zu holder_id=%d "
                    "subject_id=%d same_id=%d holder_verdict=%s holder_exit=%d\n",
                    r, rc, rs, holder_id, subject_chunk_id, same,
                    holder_verdict_ok ? "UNCHANGED" : "MUTATED", WIFEXITED(hst) ? WEXITSTATUS(hst) : -1);
        std::fflush(nullptr);
        if (rc == 0) ++subject_ok;
        if (rs != 0) ++subject_reset_rounds;
        if (holder_verdict_ok) ++holder_ok;
        /* 逐轮文件**保留**（审计留痕）；下一轮开始时才 unlink 同名文件。 */
    }
    std::printf("T06_LIVE_RESULT rounds=%d subject_ok=%d subject_rounds_with_reset=%d "
                "holder_unchanged=%d same_id_rounds=%d log=%s\n",
                rounds, subject_ok, subject_reset_rounds, holder_ok, alias_rounds, log.c_str());
    std::fflush(nullptr);
    return (subject_ok == rounds && subject_reset_rounds == 0 && holder_ok == rounds &&
            alias_rounds == 0) ? 0 : 1;
}

/* ── 场景 dead：段内持有者已死 ⇒ 允许复位 ──────────────────────────────── */
int scen_dead(std::string const &prefix, int rounds, bool expect_reset)
{
    std::string const log = g_log_dir + "/dead.log";
    int ok = 0, borrowed_ok = 0, reset_rounds = 0;
    std::size_t pre_chain_seen = 0;
    for (int r = 0; r < rounds; ++r)
    {
        ::unlink(seg_path(prefix).c_str());
        /* ① 种子进程：借 3 块后 _exit（不归还）⇒ 段留存且空闲链非素净。 */
        int const src = run_child("/dev/null", [&]() -> int
        {
            RoutePair p;
            if (!p.open(prefix, topic("seedsrc", r, 0))) return 3;
            std::vector<ipc::loan_t> held;
            for (int i = 0; i < 3; ++i)
            {
                auto lo = p.tx->loan(kLoanSize);
                if (!lo.valid()) break;
                held.push_back(lo);
            }
            std::fflush(nullptr);
            return (int)held.size();   /* 不 discard ⇒ 段内空闲链保持非素净 */
        });
        SegState const pre = seg_state(prefix);
        print_seg("dead_pre_subject", prefix, pre);
        pre_chain_seen = pre.chain;

        const std::size_t sz0 = file_size(log);
        int const rc = run_child(log, [&]() -> int
        {
            RoutePair p;
            if (!p.open(prefix, topic("dead", r, 0))) { std::printf("T06_DEAD round=%d open_failed\n", r); return 3; }
            std::vector<ipc::loan_t> held;
            for (std::size_t i = 0; i < kCap + 8; ++i)
            {
                auto lo = p.tx->loan(kLoanSize);
                if (!lo.valid()) break;
                held.push_back(lo);
            }
            SegState const s = seg_state(prefix);
            std::printf("T06_DEAD round=%d prefix=%s borrowed=%zu chain_while_full=%zu cursor=%u\n",
                        r, prefix.c_str(), held.size(), s.chain, s.cursor);
            for (auto const &lo : held) p.tx->discard_loan(lo);
            std::fflush(nullptr);
            return (int)held.size();
        });
        std::size_t const rs = count_reset_lines(log, sz0);
        if ((file_size(log) <= sz0) || rc == 120)
        {
            std::printf("T06_MEAS_ERR scenario=dead round=%d rc=%d log=%s reason=no_child_output\n", r,
                        rc, log.c_str());
            return 1;
        }
        reset_rounds += (rs != 0) ? 1 : 0;
        int const borrowed = rc;
        if (borrowed == (int)kCap) ++borrowed_ok;
        std::printf("T06_DEAD round=%d seeder_borrowed=%d pre_chain=%zu subject_borrowed=%d "
                    "subject_reset_lines=%zu\n",
                    r, src, pre.chain, borrowed, rs);
        std::fflush(nullptr);
        if (expect_reset ? (borrowed == (int)kCap && rs != 0) : (borrowed == (int)(kCap - 3)))
            ++ok;
    }
    std::printf("T06_DEAD_RESULT rounds=%d rounds_as_expected=%d borrowed_full=%d "
                "rounds_with_reset=%d pre_chain_last=%zu expect_reset=%d log=%s\n",
                rounds, ok, borrowed_ok, reset_rounds, pre_chain_seen, expect_reset ? 1 : 0,
                log.c_str());
    std::fflush(nullptr);
    return (ok == rounds) ? 0 : 1;
}

/* ── 场景 race：reclaim 期间同进程另一线程 acquire ──────────────────────── */
int one_round(int seed, std::string const &prefix, int topics, int *out_ids, int *out_ptrs,
              int *out_held)
{
    *out_ids = *out_ptrs = *out_held = 0;
    std::vector<std::unique_ptr<ipc::route>> rx, tx;
    for (int i = 0; i < topics; ++i)
    {
        const std::string t = topic("race", seed, i);
        rx.emplace_back(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::receiver});
        tx.emplace_back(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::sender});
    }
    for (int i = 0; i < topics; ++i)
        if (!tx[(std::size_t)i]->wait_for_recv(1, 3000)) return 3;

    std::atomic<int> go{0};
    std::mutex m;
    struct Owned { int t; ipc::loan_t lo; };
    std::vector<Owned> held;
    std::vector<std::thread> th;
    for (int i = 0; i < topics; ++i)
    {
        th.emplace_back([&, i] {
            while (go.load(std::memory_order_acquire) == 0) {}
            auto lo = tx[(std::size_t)i]->loan(kLoanSize);
            if (lo.valid())
            {
                std::memset(lo.data, 'A' + i, 64);
                std::lock_guard<std::mutex> g(m);
                held.push_back(Owned{i, lo});
            }
        });
    }
    go.store(1, std::memory_order_release);
    for (auto &x : th) x.join();

    int ids = 0, ptrs = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
        for (std::size_t j = i + 1; j < held.size(); ++j)
        {
            if (held[i].t == held[j].t) continue;
            if (held[i].lo.id == held[j].lo.id) ++ids;
            if (held[i].lo.data == held[j].lo.data) ++ptrs;
        }
    for (auto &h : held) tx[(std::size_t)h.t]->discard_loan(h.lo);
    *out_ids = ids;
    *out_ptrs = ptrs;
    *out_held = (int)held.size();
    /* ⛔ 每轮**必打一行**：否则"无命中"与"被测进程根本没跑/写不出日志"在量具上不可区分
     * （量具自检：race 场景按本轮新增输出行判定"测量有效"）。 */
    std::printf("T06_RACE_ROUND seed=%d topics=%d held=%zu same_id_pairs=%d same_data_ptr=%d\n",
                seed, topics, held.size(), ids, ptrs);
    std::fflush(nullptr);
    return (ids || ptrs) ? 1 : 0;
}

int scen_race(std::string const &prefix, int rounds, int topics)
{
    std::string const log = g_log_dir + "/race_worker_topics" + std::to_string(topics) + ".log";
    ::unlink(log.c_str());
    int bad = 0, skip = 0, executed = 0, crash_rounds = 0;
    int ids_total = 0, ptrs_total = 0, hit_rounds = 0, last_hit = -1, crash_round = -1;
    std::size_t reset_total = 0, reset_rounds = 0, noreset_rounds = 0;
    for (int r = 0; r < rounds && bad == 0; ++r)
    {
        /* ① 种子：造"已被用过"的段（借 3 后 _exit）。 */
        (void)run_child("/dev/null", [&]() -> int
        {
            RoutePair p;
            if (!p.open(prefix, topic("raceseed", r, 0))) return 3;
            std::vector<ipc::loan_t> held;
            for (int i = 0; i < 3; ++i)
            {
                auto lo = p.tx->loan(kLoanSize);
                if (!lo.valid()) break;
                held.push_back(lo);
            }
            std::fflush(nullptr);
            ::_exit(0);
        });
        /* ② 被测 worker：全新进程（首次 attach 判定每轮各来一次）。 */
        const std::size_t sz0 = file_size(log);
        int pipefd[2] = {-1, -1};
        if (::pipe(pipefd) != 0) { std::printf("T06_RACE_ABNORMAL round=%d pipe_failed\n", r); ++bad; break; }
        const ::pid_t pid = ::fork();
        if (pid == 0)
        {
            redirect_stdout(log);
            ::close(pipefd[0]);
            int ids = 0, ptrs = 0, held = 0;
            int const rc = one_round(r, prefix, topics, &ids, &ptrs, &held);
            int const rep[3] = {rc, ids * 100 + ptrs, held};
            (void)!::write(pipefd[1], rep, sizeof(rep));
            ::close(pipefd[1]);
            std::fflush(nullptr);
            ::_exit(rc);
        }
        ::close(pipefd[1]);
        int rep[3] = {-1, -1, -1};
        ssize_t const rn = ::read(pipefd[0], rep, sizeof(rep));
        ::close(pipefd[0]);
        int st = 0;
        (void)::waitpid(pid, &st, 0);
        std::size_t const rs = count_reset_lines(log, sz0);
        if (file_size(log) <= sz0)
        {
            std::printf("T06_MEAS_ERR scenario=race round=%d log=%s reason=no_worker_output\n", r,
                        log.c_str());
            ++bad;
            break;
        }
        ++executed;
        reset_total += rs;
        if (rs != 0) ++reset_rounds; else ++noreset_rounds;
        if (!WIFEXITED(st))
        {
            std::printf("T06_RACE_ABNORMAL round=%d signal=%d classified=crash\n", r,
                        WIFSIGNALED(st) ? WTERMSIG(st) : -1);
            ++bad; ++crash_rounds; crash_round = r; break;
        }
        const int c = WEXITSTATUS(st);
        if (rn == (ssize_t)sizeof(rep) && rep[1] > 0)
        {
            ids_total += rep[1] / 100;
            ptrs_total += rep[1] % 100;
            ++hit_rounds;
            last_hit = r;
            std::printf("T06_RACE_ALIAS_HIT round=%d rc=%d same_id_pairs=%d same_data_ptr=%d held=%d "
                        "worker_reset_lines=%zu\n",
                        r, rep[0], rep[1] / 100, rep[1] % 100, rep[2], rs);
            std::fflush(nullptr);
        }
        if (c == 3) ++skip; else if (c == 1) ++bad;
        if ((r % 200) == 0)
        {
            std::printf("T06_RACE_PROGRESS round=%d executed=%d bad=%d resets=%zu noreset=%zu\n", r,
                        executed, bad, reset_total, noreset_rounds);
            std::fflush(nullptr);
        }
    }
    std::printf("T06_RACE_RESULT rounds=%d executed=%d topics=%d bad_rounds=%d skipped_rounds=%d "
                "same_id_pairs=%d same_data_ptr=%d alias_hit_rounds=%d crash_rounds=%d "
                "first_alias_round=%d crash_round=%d worker_reset_total=%zu "
                "worker_rounds_with_reset=%zu worker_rounds_without_reset=%zu log=%s\n",
                rounds, executed, topics, bad, skip, ids_total, ptrs_total, hit_rounds,
                crash_rounds, last_hit, crash_round, reset_total, reset_rounds, noreset_rounds,
                log.c_str());
    std::fflush(nullptr);
    return bad ? 1 : 0;
}

void usage()
{
    std::printf("usage: t06_scenario_probe fresh|live|dead <prefix> <rounds> [expect_reset 0|1]\n"
                "       t06_scenario_probe race <prefix> <rounds> <topics>\n"
                "env: T06_LOG_DIR (default %s)\n", g_log_dir.c_str());
}

}   // namespace

int main(int argc, char **argv)
{
    if (const char *d = ::getenv("T06_LOG_DIR")) g_log_dir = d;
    ensure_dir(g_log_dir);
    if (argc < 4) { usage(); return 2; }
    const std::string scen = argv[1];
    const std::string prefix = argv[2];
    const int a3 = std::atoi(argv[3]);

    std::string const libp = loaded_lib_path();
    std::printf("T06_PROBE scenario=%s prefix=%s rounds=%d loaded_lib=%s loaded_lib_sha256=%s "
                "probe_pid=%d log_dir=%s\n",
                scen.c_str(), prefix.c_str(), a3, libp.c_str(), sha256_of(libp).c_str(),
                (int)::getpid(), g_log_dir.c_str());
    SegState const s0 = seg_state(prefix);
    print_seg("probe_start", prefix, s0);
    std::fflush(nullptr);

    if (scen == "fresh") return scen_fresh(prefix, a3);
    if (scen == "live") return scen_live(prefix, a3);
    if (scen == "dead")
    {
        bool const expect_reset = (argc > 4) ? (std::atoi(argv[4]) != 0) : true;
        return scen_dead(prefix, a3, expect_reset);
    }
    if (scen == "race")
    {
        int const topics = (argc > 4) ? std::atoi(argv[4]) : 8;
        ::unlink(seg_path(prefix).c_str());
        return scen_race(prefix, a3, topics);
    }
    usage();
    return 2;
}
