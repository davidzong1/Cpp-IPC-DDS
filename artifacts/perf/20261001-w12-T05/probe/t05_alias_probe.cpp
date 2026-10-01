/* T05 / W12 §7.3.4 判据探针 —— 段级复位 vs 同进程并发首借（与
 * test/test_chunk_capacity_backpressure.cpp 的 C9 用例**同逻辑**，但独立成型，
 * 便于换库矩阵逐臂记录 same_id_pairs / same_data_ptr / bad_rounds / orphan reset 次数）。
 *
 * 设计要点（与仓内用例逐条对齐，⛔ 不得放宽）：
 *   · 专属前缀（非空）—— 空前缀的 9216 段是全机共享的，会引入邻居噪声；
 *   · 每轮 fork 一个**种子进程**：借 3 块后 `_exit`（不归还）⇒ 段留存且空闲链**非素净**
 *     ⇒ 复位路径才会进入（全新段会被 `pool_.invalid()` 早退挡掉）；
 *   · 每轮再 fork 一个**被测进程**：8 个话题各一个生产者，栅栏同步后**同时**首发
 *     `loan(8000)`（档 9216）⇒ 竞争"首次 attach 判定"；
 *   · 清段**只在开跑前一次**（不是每轮清 —— 每轮清会让段永远"全新"，复位路径不执行）；
 *   · 判据：两个不同话题拿到相同 `loan_t::id` 或相同 `data` 指针 ⇒ bad round。
 *
 * 自指纹：把**实际加载的 libipc 库的真实路径与 SHA-256** 打进输出（读 /proc/self/maps），
 * 使读数与库的绑定不依赖外部脚本的转述。 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "libipc/ipc.h"

namespace {

constexpr int kTopics = 8;
constexpr std::size_t kLoanSize = 8000; /* ⇒ 档 9216 */

/* 以下三个仅在父进程使用；子进程的写随 _exit 丢弃 ⇒ 必须由父进程累加。 */
int g_ids_total = 0;
int g_ptrs_total = 0;

/* 读 /proc/self/maps 找实际加载的 libipc.so.*，返回 (路径, sha256) */
std::string loaded_lib_path()
{
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("/");
        if (pos == std::string::npos) continue;
        std::string p = line.substr(pos);
        if (p.find("libipc.so") == std::string::npos) continue;
        if (p.find(".so") == std::string::npos) continue;
        /* 解析符号链接到真实体（libipc.so.3 → libipc.so.1.3.0） */
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

int one_round(int seed, std::string const &prefix, int *out_ids, int *out_ptrs, int *out_held)
{
    *out_ids = *out_ptrs = *out_held = 0;
    std::vector<std::unique_ptr<ipc::route>> rx, tx;
    for (int i = 0; i < kTopics; ++i) {
        const std::string t = "w12t05_" + std::to_string(seed) + "_t" + std::to_string(i);
        rx.emplace_back(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::receiver});
        tx.emplace_back(new ipc::route{ipc::prefix{prefix.c_str()}, t.c_str(), ipc::sender});
    }
    for (int i = 0; i < kTopics; ++i)
        if (!tx[(std::size_t)i]->wait_for_recv(1, 3000)) return 3; /* 3 = 前提不成立 ⇒ skip */

    std::atomic<int> go{0};
    std::mutex m;
    struct Owned { int t; ipc::loan_t lo; };
    std::vector<Owned> held;
    std::vector<std::thread> th;
    for (int i = 0; i < kTopics; ++i) {
        th.emplace_back([&, i] {
            while (go.load(std::memory_order_acquire) == 0) {}
            auto lo = tx[(std::size_t)i]->loan(kLoanSize);
            if (lo.valid()) {
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
        for (std::size_t j = i + 1; j < held.size(); ++j) {
            if (held[i].t == held[j].t) continue;
            if (held[i].lo.id == held[j].lo.id) ++ids;
            if (held[i].lo.data == held[j].lo.data) ++ptrs;
        }
    for (auto &h : held) tx[(std::size_t)h.t]->discard_loan(h.lo);
    /* ⛔ 计数由**父进程**在收到管道回报后累加（子进程累加会随 _exit 丢失）。 */
    *out_ids = ids;
    *out_ptrs = ptrs;
    *out_held = (int)held.size();
    std::fflush(nullptr);
    return (ids || ptrs) ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv)
{
    int rounds = 1200;
    std::string prefix = "w12t05alias";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) prefix = argv[++i];
    }
    /* ⛔ 每轮 2 次 fork × 1200 轮 ⇒ 必须放开 core dump：若发生段级复位引发的别名写崩，
     * 崩溃本身是要被看见的证据，不得因 core 写盘而拖慢或掩盖。设为 0 只影响"是否落盘"，
     * 不影响 WIFSIGNALED 的判定。 */
    /* ⛔ 只关闭 core dump（不改任何其它 rlimit）。 */
    {
        struct rlimit rl;
        if (::getrlimit(RLIMIT_CORE, &rl) == 0) { rl.rlim_cur = 0; (void)::setrlimit(RLIMIT_CORE, &rl); }
    }
    std::string const libp = loaded_lib_path();
    std::string const libsha = sha256_of(libp);
    std::printf("T05_FINGERPRINT loaded_lib=%s loaded_lib_sha256=%s pid=%d\n",
                libp.c_str(), libsha.c_str(), (int)::getpid());
    std::printf("T05_CONFIG rounds=%d topics=%d loan_size=%zu chunk_class=9216 prefix='%s' seed_procs=per-round\n",
                rounds, kTopics, kLoanSize, prefix.c_str());
    std::fflush(nullptr);

    /* ⛔ 只清一次（不是每轮清）：每轮的"非素净"由种子进程现造。 */
    ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, "w12t05_seed");

    int bad = 0, skip = 0, done = 0;
    int crash_round = -1;
    int hit_rounds = 0, crash_rounds = 0, last_hit_round = -1;
    for (int r = 0; r < rounds && bad == 0; ++r) {
        /* ① 种子进程：借 3 块后 _exit（不归还）⇒ 段留存且空闲链非素净。 */
        const ::pid_t sp = ::fork();
        if (sp == 0) {
            ipc::route stx{ipc::prefix{prefix.c_str()}, "w12t05_seed", ipc::sender};
            ipc::route srx{ipc::prefix{prefix.c_str()}, "w12t05_seed", ipc::receiver};
            if (!stx.wait_for_recv(1, 3000)) ::_exit(0);
            std::vector<ipc::loan_t> keep;
            for (int i = 0; i < 3; ++i) {
                auto lo = stx.loan(kLoanSize);
                if (!lo.valid()) break;
                keep.push_back(lo);
            }
            std::fflush(nullptr);
            ::_exit(0);
        }
        int sst = 0;
        (void)::waitpid(sp, &sst, 0);

        /* ② 被测进程：8 话题并发首借。子进程通过管道把"本轮的 id/data 冲突计数"回传，
         * 使 outside 能区分两种失败形态：**别名命中**（同 id / 同 data 指针）与
         * **异常退出**（信号/写崩）。⛔ 两者都是 bad round，但必须分类如实报告。 */
        int pipefd[2] = {-1, -1};
        if (::pipe(pipefd) != 0) { std::printf("T05_ABNORMAL round=%d pipe_failed\n", r); bad++; break; }
        const ::pid_t pid = ::fork();
        if (pid == 0) {
            ::close(pipefd[0]);
            int ids = 0, ptrs = 0, held = 0;
            int const rc = one_round(r, prefix, &ids, &ptrs, &held);
            int const rep[3] = {rc, ids * 100 + ptrs, held};   /* ids/ptrs 各自 <100 足够 */
            (void)!::write(pipefd[1], rep, sizeof(rep));
            ::close(pipefd[1]);
            ::_exit(rc);
        }
        ::close(pipefd[1]);
        int rep[3] = {-1, -1, -1};
        ssize_t const rn = ::read(pipefd[0], rep, sizeof(rep));
        ::close(pipefd[0]);
        int st = 0;
        (void)::waitpid(pid, &st, 0);
        if (!WIFEXITED(st)) {
            std::printf("T05_ABNORMAL round=%d signal=%d classified=crash\n", r,
                        WIFSIGNALED(st) ? WTERMSIG(st) : -1);
            std::fflush(nullptr);
            bad++;
            crash_round = r;
            ++crash_rounds;
            ++done;
            break;
        }
        const int c = WEXITSTATUS(st);
        if (rn == (ssize_t)sizeof(rep) && rep[1] > 0) {
            /* ⛔ 子进程的累计量不回传 ⇒ 命中计数必须在这里由**父进程**累加，
             * 否则 T05_RESULT 的 same_id_pairs / same_data_ptr 会假报 0。 */
            g_ids_total += rep[1] / 100;
            g_ptrs_total += rep[1] % 100;
            ++hit_rounds;
            last_hit_round = r;
            std::printf("T05_ALIAS_HIT round=%d rc=%d same_id_pairs=%d same_data_ptr=%d held=%d prefix=%s\n",
                        r, rep[0], rep[1] / 100, rep[1] % 100, rep[2], prefix.c_str());
            std::fflush(nullptr);
        }
        if (c == 3) ++skip; else if (c == 1) ++bad;
        ++done;
    }
    std::printf("T05_RESULT rounds=%d executed=%d topics=%d bad_rounds=%d skipped_rounds=%d "
                "same_id_pairs=%d same_data_ptr=%d alias_hit_rounds=%d crash_rounds=%d "
                "first_alias_round=%d crash_round=%d\n",
                rounds, done, kTopics, bad, skip, g_ids_total, g_ptrs_total, hit_rounds,
                crash_rounds, last_hit_round, crash_round);
    std::fflush(nullptr);
    return bad ? 1 : 0;
}
