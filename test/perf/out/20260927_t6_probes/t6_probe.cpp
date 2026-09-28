// t6 probe harness skeleton (ipc-test-perf). Kept in .t6_probe/ (workspace scratch), never in test/.
#include <dzIPC/threepools/socket_recv_worker.h>
#include <dzIPC/threepools/recv_worker.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <thread>

namespace {
std::size_t thread_count() {
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    std::size_t n = 0;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n;
    closedir(d);
    return n;
}
void sleep_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
long arg_long(int argc, char** argv, const char* key, long def) {
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], key, klen) && argv[i][klen] == '=')   // --key=VALUE
            return std::strtol(argv[i] + klen + 1, nullptr, 10);
        if (!std::strcmp(argv[i], key) && i + 1 < argc)                  // --key VALUE
            return std::strtol(argv[i + 1], nullptr, 10);
    }
    return def;
}
}  // namespace

int main(int argc, char** argv) {
    const long workers  = arg_long(argc, argv, "--workers", 0);
    const long hold_ms  = arg_long(argc, argv, "--hold-ms", 2000);
    const long sample_ms= arg_long(argc, argv, "--sample-ms", 200);
    std::printf("probe=t6_skeleton pid=%d\n", (int)getpid());
    std::printf("backend=%s available=%d\n",
                dzIPC::threepools::SocketRecvWorkerPool::backend_name(),
                (int)dzIPC::threepools::SocketRecvWorkerPool::backend_available());
    std::printf("threads_t0=%zu\n", thread_count());
    auto& pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
    const bool started = pool.start((std::size_t)workers);
    std::printf("pool_started=%d worker_count=%zu running=%d threads_after_start=%zu\n",
                (int)started, pool.worker_count(), (int)pool.running(), thread_count());
    for (long t = 0; t < hold_ms; t += sample_ms) {
        const auto st = pool.stats();
        std::printf("t=%ldms threads=%zu routes=%zu idle_exits=%llu restarts=%llu "
                    "wakeups=%llu timeouts=%llu\n", t, thread_count(), st.route_count,
                    (unsigned long long)st.idle_exits, (unsigned long long)st.thread_restarts,
                    (unsigned long long)st.wait_wakeups, (unsigned long long)st.wait_timeouts);
        std::fflush(stdout);
        sleep_ms(sample_ms);
    }
    pool.stop();
    sleep_ms(300);
    std::printf("threads_after_stop=%zu\n", thread_count());
    return 0;
}
