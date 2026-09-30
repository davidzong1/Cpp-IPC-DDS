/* t46 同臂取证：`ipc::loan()` 原因出口的**五个失败出口逐条可判** + 两个目标 ID 的
 * 0→非 0 演示 + **三类互不冒充**核对。
 *
 * 三类（⛔ 不得互相冒充，这是本任务的核心判据）：
 *   · chunk_exhausted      —— 传输层: A/TLV 的 send / no_member_send 池空(降级交付)
 *   · chunk_alloc_failed   —— 传输层: loan 池空(B 借样被拒, 未交付)
 *   · borrow_failed_*      —— 应用层: loan()/publish_loaned() 失败(按原因细分)
 * 本探针让两个族**同时在场**，分别读各自读数。
 *
 * ⛔ 关于生产写入点的如实边界（写进交付 §5）：
 *   `borrow_failed_pool_exhausted` 的**生产**写入点必须在应用调用点
 *   （`include/dzIPC/shm_pub_sub_ipc.h::loan<Flat>()`，属 SHM接入负责人的写入面）。
 *   本探针的 ARM-B 扮演那个调用点：它**逐句执行**该补丁将要执行的逻辑
 *   （读原因 ⇒ 按原因记 ID），以证明"出口一旦被使用，ID 就能从 0 变非 0"，
 *   并同时给出可直接应用的 2 行补丁（`40_proposed_callsite.patch`）。
 * 用法: loan_reason_arms <A|B|C|D|E|F>
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dzIPC/measure/counters.h"
#include "libipc/ipc.h"

using dzIPC::measure::CounterId;
using dzIPC::measure::CounterRegistry;

namespace {
unsigned long long g(CounterId id)
{
    return static_cast<unsigned long long>(CounterRegistry::instance().get(id));
}
void dump(const char* tag)
{
    std::printf("%s io_pool=%llu io_oversized=%llu io_norx=%llu io_publish=%llu io_unknown=%llu | "
                "cap chunk_ex=%llu chunk_af=%llu\n", tag,
                g(CounterId::borrow_failed_pool_exhausted), g(CounterId::borrow_failed_oversized),
                g(CounterId::borrow_failed_no_receiver), g(CounterId::borrow_failed_publish),
                g(CounterId::borrow_failed_reason_unknown),
                g(CounterId::chunk_exhausted), g(CounterId::chunk_alloc_failed));
    std::fflush(stdout);
}
struct Rig {
    std::string name;
    ipc::route tx, rx;
    explicit Rig(const char* n) : name(n), tx(nullptr), rx(nullptr)
    {
        ipc::route::clear_storage(n);
        /* route 不可拷贝: 用 placement 语义的分步初始化。 */
        tx = ipc::route(n, ipc::sender);
        rx = ipc::route(n, ipc::receiver);
    }
    bool handshake() { return tx.wait_for_recv(1, 2000); }
};
}  // namespace

int main(int argc, char** argv)
{
    const char* arm = (argc > 1) ? argv[1] : "A";
    std::printf("arm=%s\n", arm);

    /* ---------- ARM-A：五出口逐条可判（原因出口的核心价值） ---------- */
    if (std::strcmp(arm, "A") == 0)
    {
        dump("BEFORE-A");
        const char* cases[] = {"E1_invalid_handle", "E2_not_ready", "E3_no_receiver",
                               "E4_pool_exhausted", "E5_size_too_large", "E6_storage_unavailable"};
        for (const char* c : cases)
        {
            ipc::loan_status st = ipc::loan_status::ok;
            ipc::loan_t lo;
            std::string nm = std::string("lra_") + c;
            if (std::strcmp(c, "E1_invalid_handle") == 0)
            {
                ipc::route unopened;              /* handle == nullptr */
                lo = unopened.loan(4096, st);
            }
            else if (std::strcmp(c, "E2_not_ready") == 0)
            {
                /* `ready_sending()==false` 的可达构造：**同一条 route 的单生产端 flag
                 * 已被另一个发送端占用**（`queue.h:143` 的 `sender_flag_ ||
                 * connect_sender()`）。这正是 counters.h 注释里点名的"单生产端 flag
                 * 占用"那一态 —— 在只有 bool 的年代它与"池耗尽"无法区分。 */
                ipc::route::clear_storage(nm.c_str());
                ipc::route tx1{nm.c_str(), ipc::sender};    /* 先占 flag */
                ipc::route rx{nm.c_str(), ipc::receiver};
                (void)tx1.wait_for_recv(1, 2000);
                ipc::route tx2{nm.c_str(), ipc::sender};    /* flag 已被占 */
                lo = tx2.loan(4096, st);
            }
            else if (std::strcmp(c, "E3_no_receiver") == 0)
            {
                ipc::route::clear_storage(nm.c_str());
                ipc::route tx{nm.c_str(), ipc::sender};
                lo = tx.loan(4096, st);           /* 已连接, 但无接收方 */
            }
            else if (std::strcmp(c, "E4_pool_exhausted") == 0)
            {
                ipc::route::clear_storage(nm.c_str());
                ipc::route tx{nm.c_str(), ipc::sender};
                ipc::route rx{nm.c_str(), ipc::receiver};
                if (!tx.wait_for_recv(1, 2000)) { std::printf("  %s handshake_failed\n", c); continue; }
                std::vector<ipc::loan_t> held;
                for (int i = 0; i < 100; ++i)
                {
                    ipc::loan_status s2 = ipc::loan_status::ok;
                    auto l = tx.loan(64 * 1024, s2);
                    if (!l.valid()) { st = s2; break; }
                    held.push_back(l);
                }
                for (auto& h : held) tx.discard_loan(h);
                std::printf("  %-22s status=%-20s (held=%zu)\n", c, ipc::loan_status_name(st), held.size());
                continue;
            }
            else if (std::strcmp(c, "E5_size_too_large") == 0)
            {
                ipc::route::clear_storage(nm.c_str());
                ipc::route tx{nm.c_str(), ipc::sender};
                ipc::route rx{nm.c_str(), ipc::receiver};
                if (!tx.wait_for_recv(1, 2000)) { std::printf("  %s handshake_failed\n", c); continue; }
                const std::size_t huge = static_cast<std::size_t>(-1);
                lo = tx.loan(huge, st);
            }
            else /* E6_storage_unavailable */
            {
                ipc::route::clear_storage(nm.c_str());
                ipc::route tx{nm.c_str(), ipc::sender};
                ipc::route rx{nm.c_str(), ipc::receiver};
                if (!tx.wait_for_recv(1, 2000)) { std::printf("  %s handshake_failed\n", c); continue; }
                /* 2^54: 档位算术不溢出、也不是 size_too_large, 但 40 块 × 该档 ⇒
                 * `chunks_mem_size` 乘积超 mmap 能力 ⇒ 段建不出来 ⇒ storage_unavailable。 */
                lo = tx.loan(static_cast<std::size_t>(1) << 54, st);
            }
            std::printf("  %-22s valid=%d status=%-20s\n", c, (int)lo.valid(), ipc::loan_status_name(st));
            if (lo.valid()) { /* 不该发生 */ }
        }
        dump("AFTER-A");
    }

    /* ---------- ARM-B：扮演应用调用点 ⇒ borrow_failed_pool_exhausted 0→非 0 ---------- */
    else if (std::strcmp(arm, "B") == 0)
    {
        dump("BEFORE-B");
        const char* nm = "lra_armB";
        ipc::route::clear_storage(nm);
        ipc::route tx{nm, ipc::sender};
        ipc::route rx{nm, ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("handshake_failed\n"); return 2; }
        int denied_pool = 0, denied_other = 0;
        std::vector<ipc::loan_t> held;
        for (int i = 0; i < 200; ++i)
        {
            ipc::loan_status st = ipc::loan_status::ok;
            auto lo = tx.loan(64 * 1024, st);
            if (lo.valid()) { held.push_back(lo); continue; }
            /* ↓↓↓ 这就是提议补丁在应用调用点要做的事（只用既有 ID） ↓↓↓ */
            if (st == ipc::loan_status::pool_exhausted)
            {
                CounterRegistry::instance().inc(CounterId::borrow_failed_pool_exhausted);
                ++denied_pool;
            }
            else if (st == ipc::loan_status::no_receiver)
            {
                CounterRegistry::instance().inc(CounterId::borrow_failed_no_receiver);
                ++denied_other;
            }
            else
            {
                CounterRegistry::instance().inc(CounterId::borrow_failed_reason_unknown);
                ++denied_other;
            }
        }
        for (auto& h : held) tx.discard_loan(h);
        std::printf("ARM-B loaned=%zu denied_pool=%d denied_other=%d (应用调用点逻辑)\n",
                    held.size(), denied_pool, denied_other);
        dump("AFTER-B");
    }

    /* ---------- ARM-C：两类传输层计数在场 ⇒ 证明不冒充 ---------- */
    else if (std::strcmp(arm, "C") == 0)
    {
        dump("BEFORE-C");
        const char* nm = "lra_armC";
        ipc::route::clear_storage(nm);
        ipc::route tx{nm, ipc::sender};
        ipc::route rx{nm, ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("handshake_failed\n"); return 2; }
        /* 借满该档 ⇒ 每次拒绝都走 acquire_storage(kind="loan") ⇒ chunk_alloc_failed */
        std::vector<ipc::loan_t> held;
        for (int i = 0; i < 100; ++i)
        {
            auto lo = tx.loan(64 * 1024);
            if (!lo.valid()) break;
            held.push_back(lo);
        }
        for (auto& h : held) tx.discard_loan(h);
        dump("AFTER-C");
    }

    /* ---------- ARM-D：旧 API（bool 面）逐位不变 + valid() 语义不变 ---------- */
    else if (std::strcmp(arm, "D") == 0)
    {
        const char* nm = "lra_armD";
        ipc::route::clear_storage(nm);
        ipc::route tx{nm, ipc::sender};
        ipc::route rx{nm, ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("handshake_failed\n"); return 2; }
        /* 同一次调用, 旧/新两个入口必须给出**同一** valid 与 size（出口不改变结果） */
        auto old_lo = tx.loan(4096);
        ipc::loan_status st = ipc::loan_status::ok;
        auto new_lo = tx.loan(4096, st);
        std::printf("ARM-D old_valid=%d new_valid=%d old_size=%zu new_size=%zu status=%s "
                    "=> %s\n", (int)old_lo.valid(), (int)new_lo.valid(), old_lo.size, new_lo.size,
                    ipc::loan_status_name(st),
                    (old_lo.valid() == new_lo.valid() && old_lo.size == new_lo.size &&
                     (st == ipc::loan_status::ok) == old_lo.valid()) ? "CONSISTENT" : "⛔DIVERGENT");
        if (old_lo.valid()) tx.discard_loan(old_lo);
        if (new_lo.valid()) tx.discard_loan(new_lo);
    }

    /* ---------- ARM-E：溢出请求必须被拒绝（t46 安全修复回归） ---------- */
    else if (std::strcmp(arm, "E") == 0)
    {
        const char* nm = "lra_armE";
        ipc::route::clear_storage(nm);
        ipc::route tx{nm, ipc::sender};
        ipc::route rx{nm, ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("handshake_failed\n"); return 2; }
        const std::size_t sizes[] = {static_cast<std::size_t>(-1),
                                     static_cast<std::size_t>(-1) / 2,
                                     (static_cast<std::size_t>(1) << 62)};
        for (std::size_t s : sizes)
        {
            ipc::loan_status st = ipc::loan_status::ok;
            auto lo = tx.loan(s, st);
            std::printf("ARM-E req=%-22zu valid=%d status=%s\n", s, (int)lo.valid(),
                        ipc::loan_status_name(st));
            if (lo.valid()) tx.discard_loan(lo);
        }
    }

    /* ---------- ARM-F：存储不可建（mmap 失败）⇒ storage_unavailable, 不是 pool_exhausted ---------- */
    else
    {
        const char* nm = "lra_armF";
        ipc::route::clear_storage(nm);
        ipc::route tx{nm, ipc::sender};
        ipc::route rx{nm, ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("handshake_failed\n"); return 2; }
        ipc::loan_status st = ipc::loan_status::ok;
        auto lo = tx.loan(static_cast<std::size_t>(1) << 54, st);   /* 2^54: 段建不出来 */
        std::printf("ARM-F req=2^54 valid=%d status=%s (⛔ 不得归为 pool_exhausted)\n",
                    (int)lo.valid(), ipc::loan_status_name(st));
        if (lo.valid()) tx.discard_loan(lo);
        /* 对照: 正常大请求必须在**稀疏成功带**内, 且容量不虚报。 */
        ipc::loan_status st2 = ipc::loan_status::ok;
        auto lo2 = tx.loan(64 * 1024, st2);
        std::printf("ARM-F req=64KiB valid=%d size=%zu status=%s (正常带, 期待 ok/65536)\n",
                    (int)lo2.valid(), lo2.size, ipc::loan_status_name(st2));
        if (lo2.valid()) tx.discard_loan(lo2);
    }
    std::printf("LOAN_REASON_ARMS_DONE\n");
    return 0;
}
