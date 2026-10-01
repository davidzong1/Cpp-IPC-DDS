#!/bin/bash
# t65 · W09 判决性实验独立复现（⛔ 必须在**单次 bash 调用**内跑完：/dev/shm 不跨调用保留）
#
# 设计要点（第一版脚本的缺陷已修正）：**每个库在单跑前各自重新注入一次泄漏**。
# 原因（第一版实测发现）：修复库单跑成功后，残留段已被它自己的 `reclaim_orphan_segment`
# 复位成"干净可用"状态 ⇒ 后续基线库看到的是**干净段** ⇒ 假 PASS（顺序污染）。
#
# 三个库，唯一变量 = 库
#   FIX  = 当前树 build/lib/libipc.so.3（含 v2 修复）
#   ABL  = 消融库：与 FIX 的源码**只差一个调用点**（不调用 reclaim_orphan_segment）
#   BASE = 基线库：源 = e800ccc（t22 **之前**）的 ipc.cpp 逐字快照，独立重编 ⇒ 无 reclaim
#          ⇒ 期望 C 臂 FAIL（无修复能力）、G 臂 bad_rounds=0（缺口不存在）
#
# 臂
#   A 基线（无泄漏）                        × {FIX, ABL, BASE} ⇒ 期望全 PASS
#   对每个库 L：B 注入 leaker 40 unpublished + SIGKILL ⇒ 段残留
#               C 有残留段时单跑 L          ⇒ 期望 ABL/BASE FAIL、FIX PASS
#               E 只删该残留段后单跑 L      ⇒ 期望 PASS
#   F 对照：leaker **活着** → 修复库**不得**复位 ⇒ 期望 FAIL（判据承重）
#
# 用法: w09_decisive_repro.sh <out_dir>
set -u
OUT=${1:?usage: w09_decisive_repro.sh <out_dir>}
cd /home/zwc/cpp_ipc_dds || exit 1
mkdir -p "$OUT"
ABL=$PWD/tmp/t65/lib_ablate
FIX=$PWD/tmp/t65/lib_fixed
BASE=$PWD/tmp/t65/lib_baseline
LEAKER=$PWD/tmp/t65/exp/leaker
SEG=/dev/shm/__IPC_SHM__CHUNK_INFO__132096__C40
BIN=build/bin/test_dzflat_builder

say() { printf '%s\n' "$*" | tee -a "$OUT/run.log"; }

run_solo() {  # run_solo <libdir> <tag>  —— 直接跑二进制（保留 stderr，不靠 ctest 摘要）
  local lib=$1 tag=$2
  local out="$OUT/$tag.log"
  LD_LIBRARY_PATH=$lib timeout 120 "$BIN" --gtest_color=no > "$out" 2>&1
  local rc=$?
  local passed failed ex orr
  passed=$(grep -c '^\[       OK \]' "$out" 2>/dev/null)
  # ⛔ 只数**去重后的用例名**：gtest 会打印三次（RUN 行 / "listed below:" 行 / 末尾汇总），
  #    且末尾那个还带 "(1105 ms)"。故取 `[  FAILED  ] <Suite>.<Case>` 的**唯一名字**再计数。
  failed=$(grep -oE '^\[  FAILED  \] +[A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' "$out" 2>/dev/null \
           | sed 's/^\[  FAILED  \] *//' | sort -u | wc -l)
  ex=$(grep -c 'chunk pool exhausted' "$out" 2>/dev/null)
  orr=$(grep -c 'orphan segment reset' "$out" 2>/dev/null)
  local names; names=$(grep -oE '^\[  FAILED  \] +[A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' "$out" 2>/dev/null | sed 's/^\[  FAILED  \] *//' | sort -u | tr '\n' ',')
  printf '%-40s lib=%-13s rc=%-3s passed=%s failed=%s exhausted=%s orphan_reset=%s  %s\n' \
      "$tag" "$(basename "$lib")" "$rc" "$passed" "$failed" "$ex" "$orr" "${names%%,}" | tee -a "$OUT/run.log"
  echo "$tag|$rc|$passed|$failed|$ex|$orr|$(basename "$lib")" >> "$OUT/results.tsv"
}

inject_leak() {  # inject_leak <tag> —— 借 40 块不发布 + SIGKILL；打印段残留字节数
  local tag=$1
  rm -f "$SEG" 2>/dev/null
  "$LEAKER" 40 > "$OUT/$tag.leaker.out" 2>&1 &
  local lp=$!
  for i in $(seq 1 100); do grep -q 'loaned=' "$OUT/$tag.leaker.out" 2>/dev/null && break; sleep 0.2; done
  local n; n=$(grep -oE 'loaned=[0-9]+' "$OUT/$tag.leaker.out" | head -1)
  local alive; alive=$(cat /proc/$lp/comm 2>/dev/null)
  kill -9 $lp 2>/dev/null; wait $lp 2>/dev/null
  local sz; sz=$(stat -c%s "$SEG" 2>/dev/null || echo 0)
  printf '  [注入 %-14s] %s  进程comm=%-8s 段残留=%s B\n' "$tag" "$n" "${alive:-<已退>}" "$sz" | tee -a "$OUT/run.log"
  echo "$sz"
}

: > "$OUT/run.log"; : > "$OUT/results.tsv"
say "=== t65 W09 判决性实验独立复现（$(date '+%F %T')）==="
for L in ABL FIX BASE; do eval "d=\$$L"
  say "$L = $(basename $d)  sha256=$(sha256sum $d/libipc.so.3 | cut -d' ' -f1 | cut -c1-32)  orphan_str=$(strings $d/libipc.so.3 | grep -c 'orphan segment reset')"
done
say "ABL↔FIX 源码差异行数（应只有 1 处调用点）= $(diff tmp/t65/ablate/src/libipc/ipc.cpp src/libipc/ipc.cpp | grep -c '^[<>]')"
diff tmp/t65/ablate/src/libipc/ipc.cpp src/libipc/ipc.cpp | sed 's/^/    /' >> "$OUT/ablate_diff.txt"
say "初始 /dev/shm 段数: $(ls /dev/shm 2>/dev/null | wc -l)"
rm -f "$SEG" 2>/dev/null

say ""
say "── A. 基线（无泄漏）──"
run_solo "$FIX"  "A1_baseline_fixed"
run_solo "$ABL"  "A2_baseline_ablate"
run_solo "$BASE" "A3_baseline_HEAD"

for L in ABL BASE FIX; do
  eval "d=\$$L"
  say ""
  say "── $L：每库独立注入一次泄漏 ──"
  inject_leak "inj_$L" > /dev/null
  say "  注入后段存在性：$(ls -la "$SEG" 2>/dev/null | awk '{print $5" B"}' || echo '不存在')  /dev/shm 段数=$(ls /dev/shm 2>/dev/null | wc -l)"
  run_solo "$d" "C_after_leak_$L"
  say "  ── 只删该残留段 ──"
  rm -f "$SEG"
  say "  删除后：$(ls "$SEG" 2>/dev/null || echo '不存在')"
  run_solo "$d" "E_after_clean_$L"
done

say ""
say "── F. 对照：leaker **活着**（持段未死）→ 修复库**不得**复位 ──"
rm -f "$SEG" 2>/dev/null
"$LEAKER" 40 > "$OUT/F1_leaker_alive.out" 2>&1 &
LP2=$!
for i in $(seq 1 100); do grep -q 'loaned=' "$OUT/F1_leaker_alive.out" 2>/dev/null && break; sleep 0.2; done
say "  $(cat "$OUT/F1_leaker_alive.out")  仍活着 pid=$LP2 comm=$(cat /proc/$LP2/comm 2>/dev/null)  段存在=$([ -e "$SEG" ] && echo yes || echo no)"
run_solo "$FIX" "F2_solo_leaker_alive_fixed"     # 期望 FAIL：判据承重，不是"看到池空就重置"
kill -9 $LP2 2>/dev/null; wait $LP2 2>/dev/null
rm -f "$SEG" 2>/dev/null


# ─────────────────────────────────────────────────────────────────────────────
# G 臂（t73 新增，常驻）：**段级复位 vs 同进程并发首借**（R1/S4 F1 反例）
#
# 背景：t22 的段级复位 `reclaim_orphan_segment()` 曾声称前提①"首次 attach 时本进程必然
#   未持有本档 chunk"**天然满足**。该说法不成立（S4 复核给出 4/120 命中的可执行反例）。
#   t73 已修：把「素净判定 + 取池快照」移进 `handles_` 的 `lock_` 临界区。
#
# 本臂是该反例的**自包含常驻形态**（源码内嵌，⛔ 不依赖任何一次性临时件），并在
#   **同一个二进制**上跑四/五个库 ⇒ 唯一变量 = `libipc.so.3`：
#     fixed    = 当前树 build/lib（含 T73 修复）                ⇒ 期望 bad_rounds=0
#     fixv2    = HEAD 的 ipc.cpp = **t22 v2（无 T73）**          ⇒ 期望 bad_rounds>0
#     negative = 快照在 `lock_` 之外（T73 之前行为）            ⇒ 期望 bad_rounds>0（有牙）
#     base     = e800ccc 的 ipc.cpp（**无 reclaim**，缺口不存在）⇒ 期望 bad_rounds=0
#     ablate   = 有 reclaim 但不调用                            ⇒ 期望 bad_rounds=0
#     （另有历史四臂矩阵含 base=t22 v2 的读数 >0，见 W09 交付 §12）
#
# 成形要点（三个坑，都踩过）：
#   ① 复位只在"首次 attach"那一刻进入，而 `handles_` 是**进程局部**的、`clear_storage`
#      （unlink 段）**不让它失效** ⇒ 必须**每轮一个新进程**，否则第 2 轮起复位根本不跑；
#   ② 复位函数首行是 `if (pool_.invalid()) return false;` ⇒ 若本轮面对**全新段**就早退
#      ⇒ 必须**先 fork 种子进程**借 3 块后 `_exit`（不归还）造出**非素净**段；
#   ③ ⛔ 不要用空前缀：空前缀的 9216 段**全机共享**，并发邻居会污染判据 ⇒ 用**专属前缀**。
# 轮数 1200（600 轮约 1/6 漏判；3000 轮耗时翻倍收益不匹配）。
# ─────────────────────────────────────────────────────────────────────────────
C9SRC=$OUT/c9_alias_probe.cpp
C9BIN=$OUT/c9_alias_probe
cat > "$C9SRC" <<'C9EOF'
/* 自包含反例探针：专属前缀 + 每轮种子进程 + 8 话题并发首借；报告同 id / 同 data 指针对数。 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"

namespace {
constexpr char const *kPrefix = "w09c9alias";   /* ⛔ 不用空前缀：全机共享会被邻居污染 */
constexpr int kTopics = 8;
constexpr std::size_t kLoanSize = 8000;         /* ⇒ 档 9216 */

int one_round(int seed)
{
    std::vector<std::unique_ptr<ipc::route>> rx, tx;
    for (int i = 0; i < kTopics; ++i) {
        const std::string t = "c9_" + std::to_string(seed) + "_t" + std::to_string(i);
        rx.emplace_back(new ipc::route{ipc::prefix{kPrefix}, t.c_str(), ipc::receiver});
        tx.emplace_back(new ipc::route{ipc::prefix{kPrefix}, t.c_str(), ipc::sender});
    }
    for (int i = 0; i < kTopics; ++i)
        if (!tx[(std::size_t)i]->wait_for_recv(1, 3000)) return 3;
    std::atomic<int> go{0};
    std::mutex m;
    struct Owned { int t; ipc::loan_t lo; };
    std::vector<Owned> held;
    std::vector<std::thread> th;
    for (int i = 0; i < kTopics; ++i)
        th.emplace_back([&, i] {
            while (go.load(std::memory_order_acquire) == 0) {}
            auto lo = tx[(std::size_t)i]->loan(kLoanSize);
            if (lo.valid()) {
                std::memset(lo.data, 'A' + i, 64);
                std::lock_guard<std::mutex> g(m);
                held.push_back(Owned{i, lo});
            }
        });
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
    std::fflush(nullptr);
    return (ids || ptrs) ? 1 : 0;
}
}  // namespace

int main(int argc, char **argv)
{
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 1200;
    int bad = 0, skip = 0, ids_total = 0, ptrs_total = 0;
    /* ⛔ 只清一次（不是每轮清）：每轮的"非素净"由种子进程现造；清太勤会退化成"永远全新段"。 */
    ipc::route::clear_storage(ipc::prefix{kPrefix}, "c9_seed");
    for (int r = 0; r < rounds && bad == 0; ++r) {
        /* ① 种子进程：借 3 块后 _exit（不归还）⇒ 段留存且空闲链**非素净** ⇒ 复位才会进入。 */
        const ::pid_t sp = ::fork();
        if (sp == 0) {
            ipc::route stx{ipc::prefix{kPrefix}, "c9_seed", ipc::sender};
            ipc::route srx{ipc::prefix{kPrefix}, "c9_seed", ipc::receiver};
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
        int sst = 0; (void)::waitpid(sp, &sst, 0);
        /* ② 测试进程：8 话题并发首借。 */
        const ::pid_t pid = ::fork();
        if (pid == 0) ::_exit(one_round(r));
        int st = 0; (void)::waitpid(pid, &st, 0);
        if (!WIFEXITED(st)) { std::printf("C9_ABNORMAL r=%d sig=%d\n", r, WIFSIGNALED(st) ? WTERMSIG(st) : -1); std::fflush(nullptr); return 1; }
        const int c = WEXITSTATUS(st);
        if (c == 3) ++skip; else if (c == 1) ++bad;
    }
    std::printf("C9_ALIAS rounds=%d topics=%d bad_rounds=%d skipped=%d\n", rounds, kTopics, bad, skip);
    std::fflush(nullptr);
    (void)ids_total; (void)ptrs_total;
    return bad ? 1 : 0;
}
C9EOF
if g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I 3rdparty "$C9SRC" -o "$C9BIN" \
        -L build/lib -lipc -lpthread -Wl,-rpath,"$PWD/build/lib" 2>"$OUT/c9_build.err"; then
  say "C9 探针编译 OK: $(basename "$C9BIN")"
else
  say "⚠️ C9 探针编译失败（见 $OUT/c9_build.err）⇒ 跳过 G 臂"
fi

say ""
say "── G. 段级复位 vs 同进程并发首借（t73 常驻臂；唯一变量 = libipc.so.3）──"
for L in FIX ABL BASE FIXNEG FIXV2; do
  case $L in
    FIXNEG) d=$PWD/tmp/t73/lib_negative ;;
    FIXV2)  d=$PWD/tmp/t73/lib_base ;;
    *) eval "d=\$$L" ;;
  esac
  [ -d "$d" ] || { say "  (跳过 $L：$d 不存在)"; continue; }
  c9out="$OUT/G_${L}.c9.log"
  LD_LIBRARY_PATH="$d" timeout 900 "$C9BIN" 1200 > "$c9out" 2>&1
  c9rc=$?
  line=$(grep -oE 'C9_ALIAS rounds=[0-9]+ topics=[0-9]+ bad_rounds=[0-9]+ skipped=[0-9]+' "$c9out" | head -1)
  printf '%-30s lib=%-13s rc=%-3s %s\n' "G_alias_$L" "$(basename "$d")" "$c9rc" "${line:-<无输出>}" | tee -a "$OUT/run.log"
  echo "G_alias_$L|$c9rc|0|0|0|0|$(basename "$d")" >> "$OUT/results.tsv"
done
say "  判读：FIX / ABL / BASE(无 reclaim) ⇒ bad_rounds=0（无缺口）；FIXV2(t22 v2) / FIXNEG(快照在 lock_ 外) ⇒ bad_rounds>0（缺口存在，判据有牙）"
say ""
say "=== 汇总 ==="
column -t -s'|' "$OUT/results.tsv" | tee -a "$OUT/run.log"
say "最终 /dev/shm 段数: $(ls /dev/shm 2>/dev/null | wc -l)"
