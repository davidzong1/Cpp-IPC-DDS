#!/usr/bin/env bash
# ============================================================================
# UF-004 / UF-009 判定实验: --via direct vs --via factory 的退出语义矩阵
# ============================================================================
#
# 【要判定什么】
#
# 已知现象(2026-09-16, build/live_runs/20260916_153546): `--via factory` 的 run
# **没有**打印 `SERVER SUMMARY`, 并在 /dev/shm 留下 17 个段; 而同脚本只差一个
# `--via` 的 direct 跑 3/3 都打印了 SUMMARY 且零残留。当时无法区分三件事:
#
#   (A) **崩溃**   —— 进程被信号打死/abort;
#   (B) **挂起**   —— 进程活着但不退出(需要外部 SIGKILL 才收得掉);
#   (C) **库接管信号处理器后 `std::exit(0)`** —— 应用自己的处理器被覆盖,
#                   于是应用的主循环永远不会因为 g_stop 而退出; 库的 detached
#                   线程走 StopDzipcLog → CleanupIpcInstances → std::exit(0)。
#                   `std::exit` **不展开栈**, 而 IPC 实例是 main 里的
#                   `shared_ptr` 局部量 ⇒ 析构不发生 ⇒ **段不 unlink** ⇒ 残留。
#
# 【三个假设各自的可观测签名(这就是本脚本采集的字段)】
#
#   | 假设 | 退出码 | 信号 | 是否需我们补 SIGKILL | 主循环走完(SUMMARY) | /dev/shm 残留 |
#   |---|---|---|---|---|---|
#   | (A) 崩溃 | 134/139 等 | SIGABRT(6)/SIGSEGV(11)/… | **否** | 否 | 有 |
#   | (B) 挂起 | 137 | **SIGKILL(9) —— 这一刀是**我们**补的** | **是** | 否 | 有 |
#   | (C) 库 exit(0) | **恰好 0** | 无(handler 被覆盖, 我们那一刀从没发出) | 否 | **否** | **有** |
#
#   ⛔ **2026-09-18 订正: 旧版这条判定规则有结构性错, (A) 与 (B) 结构上不可区分。**
#     旧规则写的是 `crash: by_signal>0 或 rc>128`, 而 (B) 行的信号**恒为 9**:
#     我们补的是 `kill -9`, `wait` 于是返回 137 = 128+9, `sig_of` 给出 9 ⇒
#     **每一条"挂起"行同时满足"崩溃"判据**, 挂起被**单向**吞进崩溃。两个后果:
#       ① 假设 (B) 永远统计不到(哪怕它真的发生了);
#       ② 反过来也读不出 (A) —— 看到 by_signal=9 时无法判断那是真崩还是我们补的刀。
#     现改为三条**互斥**规则, 由 classify() **机器执行**(不再靠读者事后按结果挑解释),
#     每个 CSV 行落一个 hypothesis 列:
#       (A) `crash`           : `hang=0` 且 信号 ∈ {4,6,7,8,11}(SIGILL/ABRT/BUS/FPE/SEGV)
#                               —— ⛔ 显式排除 9(9 只可能来自本脚本的看门狗);
#       (B) `hang`            : `hang=1`, 其唯一来源是**看门狗补刀前留下的标记文件**(不是 rc);
#       (C) `library_exit0`   : `hang=0` 且 rc=0 且 信号=0 且 SUMMARY 缺失 且 残留>0。
#     另补 `killed_ext`(信号 9 但 hang=0 —— 不是我们补的刀)、`signal_other`、
#     `exit_nonzero`、`exit0_other`、`undetermined_rc`(启动失败行), 以及
#     `prereq_fail_summary_pre`(见下)。⇒ "三个假设之外还有别的形态"不再被硬塞进某一类。
#   ⇒ 判据的核心是 **SUMMARY 缺失 × 退出码 0 × 有残留** 这个三元组;
#     只看其中任一项都会被 (A)/(B) 混进来。
#
#   ⛔ **另一处会静默给出错读数的订正: per-pid 退出耗时。** 旧版对两个 pid **串行**
#     `wait`, 而耗时统一从 T_KILL 起算 ⇒ **server 的 elapsed 下界是 client 的 elapsed**
#     (client 先等完 GRACE_MS 才轮到 server)。一旦 client 走慢路径, server 的
#     exit_elapsed_ms 被抬到秒级, 而"elapsed ≈100~200ms(库的 100ms 轮询)"正是假设 (C)
#     的四个证据之一 ⇒ 该字段失去判别力, 且**偏向把 (C) 判成非 (C)**。现改为轮询
#     `/proc/<pid>/stat` 状态, 每个 pid 自己计时, 与另一个 pid 无关。
#
#   ⛔ **2026-09-18 第二批订正(采纳 reviewer seq130 的 R1/R2/R4)** —— 都是"判定规则
#     本身还会给出错结论"的项, 故在**开跑之前**改:
#
#     R1(高) **(C) 丢掉了 `exit_elapsed_ms` 合取项 ⇒ (B) 会被 (C) 吞掉。** 这是与 D1
#       **同一个错的反方向**: hang 的唯一来源是"看门狗补了刀" ⇒ **只有超 grace 才可能是
#       (B)**, 于是"库的慢退出路径在 grace 内把进程收掉"这一形态**结构性落不进 (B)**,
#       会被自动归档成 (C)。修法: (C) 按耗时拆三分 ——
#       `library_exit0_fast`(elapsed < `--lib-fast-ms`, 默认 1000ms) /
#       `library_exit0_slow`(仍是 (C) 三元组但退出被拖慢, ⛔ 只能记"(C) 机制 + 慢退出",
#       **不得写成 (C) 已证**) / `hang`(超 grace)。
#
#     R2(高) **残留归因缺失**: 旧版把同一个 `RES_N` 同时写进 client 行与 server 行 ⇒
#       **没有能力区分是哪一端残留**, 而本文件头自己写的理由恰恰是"只采一端会把'某一端
#       没残留'误读成'整体无残留'" —— 设计意图与实现不符。修法: 发 SIGTERM **之前**采
#       `/proc/<pid>/maps` 的段名做**映射归属**, 拆出 `residue_ser`/`residue_cli`/
#       `residue_unattr`, 并让 classify() 用**本角色自己的**残留数。
#
#     R4(中) **`startup_fail` 行是字面 echo 的短行** ⇒ 永不经过 classify(), `startup_fail`
#       成了死标签, 与规则集自相矛盾。修法: 补齐到 16 列, hypothesis 写字面 `startup_fail`。
#
#     另随结果一起改(R3/R5/R6): `app_handler_alive` 主判据只取 SUMMARY(残留按 topic 取,
#     控制面与 `create|open` 垃圾段会落进来, 当必要条件会造假阴性); `n_rounds`/`n_res0`
#     先筛掉 rc 非数字的 startup_fail 行; 新增 `signal_source` 列(⛔ `by_signal` 在挂起行
#     恒为 9, 那个 9 是**我们补的刀**, 只看该列会重犯 D1)。CSV 表头 16 列, 见脚本尾部
#     results.json 的 columns_note。
#
# 【为什么"SUMMARY 缺失"能代表"应用处理器被覆盖"】
#
#   driver 在 `main()` 里 **先**装自己的 handler(`:406-407`, `on_signal` 置
#   g_stop=1), **之后**才创建 IPC 对象。而公共工厂的四个 Make 都调
#   `EnsureShutdownMonitorStarted()`(dzipc.cc:130/142/154/166), 它
#   `std::signal(SIGTERM, SignalHandler)`(dzipc.cc:97) **覆盖**应用刚装的 handler。
#   ⇒ factory 模式下 `g_stop` 永远为 0 ⇒ `while(!g_stop)` 不退出 ⇒
#     该函数尾部的 `SUMMARY`(:302/:397) 永不打印。
#   ⇒ 而 direct 模式直接构造 `autopath::auto_ser_ipc`, **不经过**公共工厂 ⇒
#     不装 handler ⇒ 应用处理器照常生效 ⇒ SUMMARY 打印。
#
#   ⛔ 这条推理本身是"安装顺序"论证, 本脚本用 **direct 3/3 的对照组**把它变成实测:
#     若 factory 恒缺 SUMMARY 而 direct 恒有, 顺序论证成立; 若两边都缺,
#     那是 SUMMARY 本身丢了(另一回事), 顺序论证不成立。
#
# 【残留判据: 用差集, 不用"某名字不出现"】
#
#   ⛔ 不要断言"某段名不存在"。本仓已有先例(共享区 `a1_a2_criterion_defect_...`):
#     "不出现"会因为**无关原因**(段本来就被别人合法建着)而变红。故一律用
#     **before/during/after 三次快照的差集**, 且只统计本次唯一 topic 相关的名字。
#
# 【为什么 server 与 client 都要采】
#
#   两端各自独立装/被覆盖一次; 只要有一端走的是库的 exit(0), 那一端就会
#   留下它自己建的段(ser 侧 req+res 两条, 见 shm_sniffer 的控制面命名)。
#   只采一端会把"某一端没残留"误读成"整体无残留"。
#
# 用法:
#   tools/sercli_live_probe/exit_semantics_matrix.sh [--rounds 2] [--count 3]
#       [--hold-ms 6000] [--grace-ms 3000] [--lib-fast-ms 1000] [--out DIR]
#
# 退出码: 0 = 矩阵跑完并写出 results.json(不代表某个假设成立);
#         2 = 环境/参数失败(缺二进制等); 1 = 有一轮自身执行失败。
#
# ⛔ 本脚本**只读产品**: 不碰 src/ include/, 不改任何产品行为; 它只启动已构建好的
#    二进制并采集退出码/信号/日志/段名单。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
DRV="${HERE}/bin/sercli_live_driver"

ROUNDS=2
COUNT=3
HOLD_MS=6000
GRACE_MS=""      # 见下: 默认值由 HOLD_MS 推出, 不能写死
DOMAIN=3
OUT=""

# ---- UF-004 opt-out 臂开关(2026-09-18 tester-claude 追加; 默认值下行为与旧版逐位相同) ----
# ⛔ 三个开关的**默认值都必须保持"不传"的输出形态** —— 否则旧基线指纹无法作为回归门。
#    OPT_DISABLE=early: 给两端都加 --no-shutdown-monitor(应用在**任何 IPC 构造之前**关掉库接管)
#    PRELOAD:           两端都带 LD_PRELOAD(变异臂用: 把 opt-out 判定钉死/把监控提前装好)
#    DRV:               可指向**冻结副本**, 使"同一二进制"这件事可跨 coder 重建被复现
OPT_DISABLE="none"
PRELOAD=""
TAG="default"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds)   ROUNDS="$2"; shift 2;;
    --count)    COUNT="$2"; shift 2;;
    --hold-ms)  HOLD_MS="$2"; shift 2;;
    --grace-ms) GRACE_MS="$2"; shift 2;;
    --lib-fast-ms) LIB_FAST_MS="$2"; shift 2;;   # (C) 的 fast/slow 分界, 见 classify()
    --out)      OUT="$2"; shift 2;;
    --driver)   DRV="$2"; shift 2;;
    --optout)   OPT_DISABLE="$2"; shift 2;;
    --preload)  PRELOAD="$2"; shift 2;;
    --tag)      TAG="$2"; shift 2;;
    *) echo "unknown arg $1" >&2; exit 2;;
  esac
done

[[ -x "${DRV}" ]] || { echo "ERROR: ${DRV} 缺失, 先跑 tools/sercli_live_probe/build.sh" >&2; exit 2; }
[[ "${OPT_DISABLE}" == "none" || "${OPT_DISABLE}" == "early" ]] \
  || { echo "ERROR: --optout 只支持 none|early(晚调用臂见 uf004_optout_acceptance.sh)" >&2; exit 2; }
if [[ -n "${PRELOAD}" ]]; then
  [[ -e "${PRELOAD}" ]] || { echo "ERROR: --preload ${PRELOAD} 不存在" >&2; exit 2; }
fi

# 本轮**实际用到**的两个二进制的指纹 —— 只登记, 不判定(判定由验收脚本做)。
DRV_MD5=$(md5sum "${DRV}" | cut -d' ' -f1)
# 库的 md5 由**运行期**解析结果决定(可能被 LD_LIBRARY_PATH 换掉), 见下面每次采样的 LIB_MD5。
LIB_MD5="${LIB_MD5:-未指定}"

# ⛔ grace 必须**结构性地**大于客户端自己的 hold 余额, 否则会把"应用自己那条有界路径"
#   误判成"挂起"。推导: 客户端的 hold 是 `while(!g_stop && now<hold_deadline)` —— 若应用
#   处理器**生效**, 它在 hold 到期时就自然退出(那一刻 rc=0 且**有** SUMMARY); 若被覆盖,
#   则只有库的 exit(0) 能让它退出。两者都在"有限时间内退出", 区别在**快慢与 SUMMARY**。
#   但如果 grace < hold 余额, 前者的退出还没发生我们就宣布 hang ⇒ **假挂起**。
#   ⇒ 默认 grace = hold + 4000ms, 保证"慢的那条路"也来得及被观察到。
if [[ -z "${GRACE_MS}" ]]; then GRACE_MS=$(( HOLD_MS + 4000 )); fi

if [[ -z "${OUT}" ]]; then
  OUT="${ROOT}/build/uf004_exit_semantics/$(date +%Y%m%d_%H%M%S)_$$"
fi
mkdir -p "${OUT}"
CSV="${OUT}/matrix.csv"
echo "round,via,role,rc,by_signal,hang,exit_elapsed_ms,summary,residue_this_topic,during_gain_this_topic,hypothesis,signal_source,residue_own,residue_ser,residue_cli,residue_unattr,tag,optout,optout_ret" > "${CSV}"

echo "=== UF-004/UF-009 判定实验"
echo "=== out=${OUT} rounds=${ROUNDS} count=${COUNT} hold_ms=${HOLD_MS} grace_ms=${GRACE_MS}"
echo "=== tag=${TAG} optout=${OPT_DISABLE} preload=${PRELOAD:-<none>}"
echo "=== driver=${DRV} md5=${DRV_MD5}"
# 运行期**实际解析到**的 libipc(可能被外部 LD_LIBRARY_PATH 换掉) —— 只登记不判定。
# ⛔ 不看这一行就无法证明"两臂用的是同一个库", 而"同一二进制"正是本项的门。
LIB_RESOLVED=$(LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" ldd "${DRV}" 2>/dev/null | awk '/libipc\.so/{print $3; exit}')
LIB_RESOLVED=${LIB_RESOLVED:-<unresolved>}
if [[ -r "${LIB_RESOLVED}" ]]; then LIB_MD5=$(md5sum "${LIB_RESOLVED}" | cut -d' ' -f1); else LIB_MD5="<unreadable>"; fi
echo "=== libipc=${LIB_RESOLVED} md5=${LIB_MD5}"
# 三方指纹之三: 树头(源码态)。driver/libipc 是二进制态 —— 同一 md5 的库可以由不同树态产出,
# 缺 tree_head 时"这份读数是哪个源码态跑出来的"不可回溯(0.3.5/0.3.6 纪律)。
TREE_HEAD=$(git rev-parse HEAD 2>/dev/null || echo "<unavailable>")
echo "=== tree_head=${TREE_HEAD}"

# ---- 进程收尸 ---------------------------------------------------------------
# ⛔ `ALL_PIDS` **只累加、不在轮次之间清空**: 一个轮次若在"server 起来但 client 没起来"
#   的路径上 `continue`, 那个 server 还活着; 若下一轮开头把它清掉, 它就**再也没人收**,
#   而它会继续持有本轮的段 ⇒ **污染下一轮 before/after 差集**, 让"残留"读成一个假数。
#   代价是可能 kill 到已经死掉的 pid —— 故补一道身份校验(见下), 防止 pid 复用误杀。
ALL_PIDS=()
sweep() {
  local p
  for p in "${ALL_PIDS[@]:-}"; do
    [[ -n "${p}" ]] || continue
    # pid 复用保护: 只打**确实是本工具**的那个 pid。抽掉这道校验, 长时间实验里
    # 一个被复用的 pid 可能指向无关进程, 收尸就变成误杀。
    if [[ -r "/proc/${p}/comm" ]] && grep -q "^sercli_live" "/proc/${p}/comm" 2>/dev/null; then
      kill -9 "${p}" 2>/dev/null || true
    fi
  done
}
trap sweep EXIT

# 从 /proc/<pid>/stat 里取**进程状态字符**。stat 的格式是
#     <pid> (<comm>) <state> ...
# 其中 comm 可能含空格/括号, 故用"最后一个 ')' 之后的首个字母"定位 —— 不能用
# `awk '{print $3}'`(comm 含空格会错位)。取不到(进程已消失)时输出为空。
# 状态字符集按 proc(5) 取 A-Za-z 全集(R/S/D/T/t/Z/X/x/K/W/P/I 及小写变体),
# ⛔ 不要只写 `[A-Z]`: 漏掉小写变体会让"取不到状态"与"状态是 x"混在一起。
MARK_RE='s/^[^(]*(.*) \([A-Za-z]\).*$/\1/p'
proc_state() { sed -n "${MARK_RE}" "/proc/${1}/stat" 2>/dev/null; }

# 等一个子进程退出; 结果写进全局 WP_RC / WP_HANG / WP_MS(hang=1 = 需要我们补 SIGKILL)。
#
# ⛔ 三个必须避开的坑(都会静默给出错的结果):
#
#   (0) **hang 与 crash 必须互斥**(2026-09-18 订正, 见文件头) —— hang 的唯一来源是
#       **看门狗补刀前留下的标记文件**, 不是 rc: 我们补的是 `kill -9`, 被补刀的进程
#       `wait` 返回 137 = 128+9, 用 rc 判 hang 就与 `by_signal>0` 的崩溃判据**重叠**。
#       看门狗**先查状态再补刀**: 若进程已退出(僵尸或消失), 就不留标记、不补刀 ——
#       否则会把"恰好在 grace 边界自己退出"记成 hang=1(**假挂起**, 恰好把判定推向 (B))。
#
#   (1) **不能用 `read x < <(wait_pid ...)` / `$(wait_pid ...)`** —— 进程替换/命令替换
#       都跑在**子 shell** 里, 而 `wait` 只能等**当前 shell 自己**的子进程; 子 shell 里
#       `wait <父 shell 的子进程>` 会返回 127 并打印 "not a child of this shell"。
#       那样 rc 列会全是 127, 而 127 又会被 `sig_of` 当成正常退出码 ⇒ **崩溃/挂起/exit(0)
#       三者全部无法区分**。故本函数**直接调用**并回写全局量。
#
#   (2) **不能用 `kill -0` 判"还活着"** —— 子进程退出后若未被 wait, 会变成**僵尸**,
#       而 `kill -0` 对僵尸**成功**。故本函数轮询的是 `/proc/<pid>/stat` 的**状态字符**,
#       并把 `Z`(僵尸 = 已退出、只是还没被回收)与"进程消失"**一并算作已退出**;
#       看门狗那一侧也用同一个状态判据决定要不要补刀。
#
#   ⛔ per-pid 计时(见文件头订正): 退出耗时由本 pid 自己从 t0 起算, **不**受另一个
#      pid 等待时长影响。旧版串行 wait + 共用 T_KILL 会把 server 的 elapsed 抬到
#      ≥client 的 elapsed, 使假设 (C) 的"elapsed≈100~200ms"证据失效。
#
# 残余竞态(如实标注): 轮询粒度 10ms ⇒ "恰好在 grace 边界上退出"的窗口从
#   "一次 sleep 唤醒的抖动"缩到 ≤10ms; 观测到 hang=1 时应结合 WP_MS≈grace 交叉确认。
wait_pid() {
  local pid="$1" grace_ms="$2" t0="$3" wd mark st ms
  local hang=0
  mark="$(mktemp)"; rm -f "${mark}"   # 只要名字: 由看门狗在补刀**之前**创建

  (
    sleep "$(printf '%s.%03d' $((grace_ms / 1000)) $((grace_ms % 1000)))"
    st="$(proc_state "${pid}")"
    if [[ -n "${st}" && "${st}" != "Z" ]]; then
      : > "${mark}"
      kill -9 "${pid}" 2>/dev/null
    fi
  ) &
  wd=$!

  while :; do
    st="$(proc_state "${pid}")"
    [[ -z "${st}" || "${st}" == "Z" ]] && break
    sleep 0.01
  done
  ms=$(( $(date +%s%3N) - t0 ))

  [[ -e "${mark}" ]] && hang=1         # 唯一来源: 看门狗真补了刀
  kill "${wd}" 2>/dev/null || true
  wait "${wd}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null            # 回收(僵尸态在此被清掉)并取退出码
  WP_RC=$?
  rm -f "${mark}"
  WP_HANG=${hang}
  WP_MS=${ms}
}

# 等日志里出现某个模式(不依赖 sleep 常量, 免得慢机器上误判"没起来")
wait_log() {
  local f="$1" pat="$2" iters="$3" i
  for ((i = 0; i < iters; i++)); do
    grep -qE "${pat}" "${f}" 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}

snap() { ls /dev/shm 2>/dev/null | sort; }

# 某个 pid 当前**实际映射**的 /dev/shm 段名(basename)。用来把"退出后残留的段"归到**哪一端**
# (评审 R2: 旧版把同一个 RES_N 同时写进 client 行与 server 行 ⇒ **没有能力区分是哪一端残留**,
#  而文件头自己写的理由恰恰是"只采一端会把'某一端没残留'误读成'整体无残留'" —— 设计意图与
#  实现不符。此处按**映射归属**归因, 比按名字模式猜可靠: 段名里不含角色, 只有 topic, 猜不准)。
# ⛔ /proc/<pid>/maps 第 6 字段就是映射文件名; 已 unlink 但仍被映射的会带 " (deleted)"
#    (那是第 7 字段, 取 $6 自然剥掉)。读 maps 是只读操作, 不影响被测进程。
# ⛔ 必须在**发 SIGTERM 之前**采: 进程一退, 映射就没了。
mapped_segs() {  # $1=pid
  awk '{print $6}' "/proc/$1/maps" 2>/dev/null | sed -n 's|^/dev/shm/||p' | sort -u
}

# 交集的条数(#1=映射名单文件, #2=残留名单) —— 即"这一段确实被该端映射过"的残留条数。
inter_count() {
  local n
  n=$(comm -12 <(sort -u "$1") <(printf '%s\n' "$2" | sed '/^$/d' | sort -u) | grep -c . || true)
  echo "${n:-0}"
}

# 只在本次 topic 的名字上取差集 —— 宿主上本来就有别人的历史残留, 不筛会把它们算进来
topic_diff() {  # $1=before $2=after
  comm -13 "$1" "$2" | grep -F "$3" || true
}

count_lines() { [[ -z "$1" ]] && echo 0 || printf '%s\n' "$1" | grep -c . ; }

# ---- 判定: 三条**互斥**规则, 由本函数机器执行(写进 CSV 的 hypothesis 列) --------
# ⛔ 顺序承重: `hang` **先判**。被看门狗补刀的行 rc 恒为 137(=128+9), 若先判 crash,
#    这些行会被吞掉 —— 那正是旧规则的结构性错(见文件头 2026-09-18 订正)。
# ⛔ crash 显式排除信号 9: 9 只可能来自本脚本的看门狗 ⇒ 那是 (B) 的信号, 不是 (A) 的。
#
# ⛔ **(C) 必须复用 exit_elapsed_ms, 不能丢(评审 R1)** —— 这条是本文件第二次犯"同一个错
#    的反方向": 旧版是 (B) 被 crash 吞掉; 一度改成的版本是 **(B) 被 (C) 吞掉** ——
#    因为 hang 的唯一来源是"看门狗补了刀" ⇒ **只有超出 grace 才可能是 (B)**, 于是
#    "库的慢退出路径仍在 grace 内把进程收掉"这一形态**结构性地落不进 (B)**, 会被自动
#    归档成 (C)。故 (C) 拆三分:
#       library_exit0_fast  : elapsed < LIB_FAST_MS ⇒ 与 dzipc.cc 的 100ms 轮询同量级, (C) 机制成立
#       library_exit0_slow  : elapsed ≥ LIB_FAST_MS ⇒ 仍是 (C) 的三元组, 但**退出被拖慢/阻塞**
#                             ⛔ 只能记成"(C) 机制 + 慢退出", **不得写成"(C) 已证"**
#       hang                : hang=1(超 grace) ⇒ (B)
#   阈值 LIB_FAST_MS 由 --lib-fast-ms 覆盖, 默认 1000ms: 实测应用路径 ≈20~40ms、
#   库路径 ≈100~200ms, 而 SIGTERM 时刻的 hold 余额 ≈4500ms ⇒ 1000ms 同时**远大于**
#   两条已知路径、又**远小于** hold 余额, 两侧都留了一个数量级余量。
LIB_FAST_MS="${LIB_FAST_MS:-1000}"
CRASH_SIGS=" 4 6 7 8 11 "        # SIGILL SIGABRT SIGBUS SIGFPE SIGSEGV
classify() {  # $1=rc $2=by_signal $3=hang $4=summary $5=residue_own $6=elapsed_ms
  local rc="$1" sig="$2" hang="$3" sum="$4" res="$5" ms="$6"
  if [[ ! "${rc}" =~ ^-?[0-9]+$ ]]; then echo "undetermined_rc"; return; fi
  if [[ "${hang}" == "1" ]]; then echo "hang"; return; fi                       # (B)
  if [[ "${rc}" -gt 128 ]]; then
    if [[ "${CRASH_SIGS}" == *" ${sig} "* ]]; then echo "crash"; return; fi      # (A)
    if [[ "${sig}" == "9" ]]; then echo "killed_ext"; return; fi  # 9 但不是我们补的刀
    echo "signal_other"; return
  fi
  if [[ "${rc}" -eq 0 ]]; then
    if [[ "${sum}" == "0" && "${res}" -gt 0 ]]; then
      # (C) 的三元组齐了 ⇒ 再按耗时三分(R1)
      if [[ -n "${ms}" && "${ms}" -lt "${LIB_FAST_MS}" ]]; then echo "library_exit0_fast"; return; fi
      echo "library_exit0_slow"; return
    fi
    # ⛔ 主判据只取 SUMMARY>0(评审 R3): 残留数**不再**参与这一条 —— 残留按 topic 取,
    #    控制面段与 create|open 造出的垃圾段也会落进来, 用它当必要条件会造出假阴性。
    #    残留归因改由 residue_own/ser/cli 三列**分列报告**(R2), 不再混进判定。
    if [[ "${sum}" -gt 0 ]]; then echo "app_handler_alive"; return; fi
    echo "exit0_other"; return
  fi
  echo "exit_nonzero"        # 正常返回的非 0 退出码(例如 client 有 RPC 失败) —— 不是崩
}

# 信号来源(评审 R6): ⛔ `by_signal` 在挂起行恒为 9, 而那个 9 是**我们补的刀** ——
# 只看 matrix.csv 的人会重犯"D1: 挂起被读成崩溃"。故显式落一列。
signal_source() {  # $1=by_signal $2=hang
  if [[ "$1" == "0" ]]; then echo "none"; return; fi
  if [[ "$2" == "1" ]]; then echo "watchdog_sigkill"; return; fi
  if [[ "$1" == "9" ]]; then echo "external_sigkill"; return; fi
  echo "process"
}

ROUND_RC=0

# ---- UF-004 臂的实例化(每轮常量) -------------------------------------------
# ⛔ 用**未加引号的字符串展开**而不是数组: 空数组在 `set -u` 下的 `"${arr[@]}"` 会报
#    unbound variable(bash 4.3 及更早), 而这里两个变量都可能为空。两个值都**不含空白**,
#    故按词展开是安全的。
OPT_ARG=""
[[ "${OPT_DISABLE}" == "early" ]] && OPT_ARG="--no-shutdown-monitor"
PRE_ENV=""
[[ -n "${PRELOAD}" ]] && PRE_ENV="LD_PRELOAD=${PRELOAD}"

# 从日志里取 driver 自报的 opt-out 返回值(`OPTOUT no_shutdown_monitor=1 ret=<0|1>`)。
# ⛔ **不传开关时必须得到 `na`, 传了而拿不到必须得到 `missing`** —— 后者是承重的:
#    它区分"opt-out 成功"与"开关根本没生效"(静默失效即缺陷)。
optout_ret() {  # $1=log
  local r
  r=$(grep -o '^OPTOUT .*ret=[0-9]' "$1" 2>/dev/null | tail -1 | sed 's/.*ret=//')
  if [[ -n "${r}" ]]; then echo "${r}"
  elif [[ "${OPT_DISABLE}" == "early" ]]; then echo "missing"
  else echo "na"; fi
}

for VIA in direct factory; do
  for ((R = 1; R <= ROUNDS; R++)); do
    # 唯一 topic(不带前导斜杠 —— 带 `/` 会让 ser 侧段名含第二个 `/`, shm_open 必返
    # EINVAL(22), 那样测到的是已知的回退路径, 不是退出语义)
    TOPIC="exsem_${VIA}_$(date +%s)_$$_r${R}"
    D="${OUT}/${VIA}_r${R}"
    mkdir -p "${D}"
    echo "=== ---- via=${VIA} round=${R} topic=${TOPIC}"

    snap > "${D}/shm_before.txt"

    # 服务端先起(常驻)
    env ${PRE_ENV} "${DRV}" --role server --topic "${TOPIC}" --domain "${DOMAIN}" --auto --via "${VIA}" \
        ${OPT_ARG} > "${D}/server.log" 2>&1 &
    SER_PID=$!; ALL_PIDS+=("${SER_PID}")
    if ! wait_log "${D}/server.log" "^SERVER ready" 200; then
      echo "!! server 未 ready; 见 ${D}/server.log" >&2
      # ⛔ 16 字段, 与表头逐列对齐(评审 R4: 旧版这里字面 echo 10 字段, 表头 11 列
      #    ⇒ 该行**永不经过 classify()**, `startup_fail`/`undetermined_rc` 成了死标签,
      #    与规则集自相矛盾。现在补齐到 16 列, 且 hypothesis 直接写字面 `startup_fail`)。
      echo "${R},${VIA},server,startup_fail,,0,0,0,0,0,startup_fail,na,0,0,0,0,${TAG},${OPT_DISABLE},na" >> "${CSV}"
      ROUND_RC=1; continue
    fi
    sleep 1.0

    # 客户端: hold 很久, 保证我们发 SIGTERM 时它还在主循环里(而不是已经自然退出)
    env ${PRE_ENV} "${DRV}" --role client --topic "${TOPIC}" --domain "${DOMAIN}" --auto --via "${VIA}" \
        --count "${COUNT}" --period-ms 100 --hold-ms "${HOLD_MS}" \
        ${OPT_ARG} > "${D}/client.log" 2>&1 &
    CLI_PID=$!; ALL_PIDS+=("${CLI_PID}")
    if ! wait_log "${D}/client.log" "^CLIENT ready" 200; then
      echo "!! client 未 ready; 见 ${D}/client.log" >&2
      echo "${R},${VIA},client,startup_fail,,0,0,0,0,0,startup_fail,na,0,0,0,0,${TAG},${OPT_DISABLE},na" >> "${CSV}"
      ROUND_RC=1; continue
    fi
    sleep 1.5

    snap > "${D}/shm_during.txt"          # 运行中
    # ⛔ 归因快照必须在 SIGTERM **之前**采: 进程一退映射就没了(评审 R2)。
    mapped_segs "${SER_PID}" > "${D}/mapped_ser.txt"
    mapped_segs "${CLI_PID}" > "${D}/mapped_cli.txt"
    # SIGTERM 之前的 SUMMARY 计数(必须为 0 —— 否则"SUMMARY 缺失"这个判据没意义)
    SUM_SER_BEFORE=$(grep -c "^SERVER SUMMARY" "${D}/server.log" 2>/dev/null || true)
    SUM_CLI_BEFORE=$(grep -c "^SUMMARY" "${D}/client.log" 2>/dev/null || true)
    echo "   SIGTERM 前 SUMMARY: server=${SUM_SER_BEFORE} client=${SUM_CLI_BEFORE}"

    # ---- 发 SIGTERM 给两端(同时), 然后各自带 grace 等待
    # `%s%3N` 取毫秒: 用它算"从 SIGTERM 到退出"的耗时。
    # ⛔ 该字段的**正确读法**(2026-09-18 依 driver 源码订正, 旧注释写"应用路径要等
    #    hold 余额耗尽, 数量级秒级"是**错的**): driver 两条主循环都是
    #    `while (!g_stop && now < hold_deadline)` —— `on_signal` 一置 `g_stop`,
    #    **应用路径也在一个 `sleep_ms(20)` 节拍内退出**(实测量级 ~20ms), 不是秒级。
    #    ⇒ 两条路径的耗时差是**同量级内的一个数量级**, 不是"秒 vs 毫秒":
    #         应用处理器生效   ≈ 20~40ms   (g_stop 立刻见效)
    #         库 exit(0) 生效  ≈ 100~200ms (dzipc.cc 的 detached 线程 100ms 轮询)
    #    ⇒ elapsed 是**佐证**字段(corroboration), 单独不构成判定; 判定仍由
    #      SUMMARY × rc × 残留 三元组做(见 classify())。
    #    ⛔ 也因此必须 per-pid 计时: 旧版串行等待会把 server 的 elapsed 抬到
    #      ≥client 的 elapsed, 两条路径的差就被抹平了。
    T_KILL=$(date +%s%3N)
    kill -TERM "${CLI_PID}" 2>/dev/null || true
    kill -TERM "${SER_PID}" 2>/dev/null || true

    # ⛔ 直接调用(见 wait_pid 头注释坑 (1)): 结果经全局 WP_RC/WP_HANG/WP_MS 带出。
    #   ⛔ 两个 pid 各自用**自己的** t0 计时(见文件头"per-pid 退出耗时"订正) —— 旧版
    #      共用 T_KILL 且串行等待, 会把 server 的 elapsed 抬到 ≥client 的 elapsed。
    wait_pid "${CLI_PID}" "${GRACE_MS}" "${T_KILL}"; CLI_RC=${WP_RC}; CLI_HANG=${WP_HANG}; CLI_MS=${WP_MS}
    wait_pid "${SER_PID}" "${GRACE_MS}" "${T_KILL}"; SER_RC=${WP_RC}; SER_HANG=${WP_HANG}; SER_MS=${WP_MS}
    SUM_SER_AFTER=$(grep -c "^SERVER SUMMARY" "${D}/server.log" 2>/dev/null || true)
    SUM_CLI_AFTER=$(grep -c "^SUMMARY" "${D}/client.log" 2>/dev/null || true)

    # 信号: wait 返回 128+signo 表示被信号终止; 否则 0
    sig_of() { [[ "$1" -gt 128 ]] && echo $(( $1 - 128 )) || echo 0; }
    CLI_SIG=$(sig_of "${CLI_RC}"); SER_SIG=$(sig_of "${SER_RC}")

    sleep 0.5
    snap > "${D}/shm_after.txt"           # 退出后
    RES=$(topic_diff "${D}/shm_before.txt" "${D}/shm_after.txt" "${TOPIC}")
    DUR=$(topic_diff "${D}/shm_before.txt" "${D}/shm_during.txt" "${TOPIC}")
    RES_N=$(count_lines "${RES}"); DUR_N=$(count_lines "${DUR}")

    printf '%s\n' "${RES}" > "${D}/residue_names.txt"
    printf '%s\n' "${DUR}" > "${D}/during_names.txt"

    # ---- 残留归因(评审 R2): 把 RES 按**映射归属**拆成 ser / cli / 无归属 ----------
    # ⛔ 旧版把同一个 RES_N 同时写进 client 行与 server 行 ⇒ 判 client 行用的也是它,
    #    等于**没有能力区分是哪一端残留** —— 与文件头"两端都要采"的设计意图直接冲突。
    #    一个段可能被两端**同时**映射(共享段), 故 ser+cli 可以 > 总数; 无归属的单独报。
    RES_SER=$(inter_count "${D}/mapped_ser.txt" "${RES}")
    RES_CLI=$(inter_count "${D}/mapped_cli.txt" "${RES}")
    RES_UNATTR=$(comm -23 <(printf '%s\n' "${RES}" | sed '/^$/d' | sort -u) \
                          <(cat "${D}/mapped_ser.txt" "${D}/mapped_cli.txt" | sort -u) \
                 | grep -c . || true)
    RES_UNATTR=${RES_UNATTR:-0}
    CLI_SRC=$(signal_source "${CLI_SIG}" "${CLI_HANG}")
    SER_SRC=$(signal_source "${SER_SIG}" "${SER_HANG}")

    # ⛔ 前置门: SIGTERM **之前**两端都不该有 SUMMARY —— 否则"SUMMARY 缺失"这条
    #    判据(假设 (C) 的核心证据)失去意义, 本轮的 hypothesis 一律不可信, 标
    #    prereq_fail_summary_pre 而不是硬塞进某一类(docs 0.3 第 5 条精神)。
    #   classify 的 residue 参数用**本角色自己的**残留数(R2), 不再是共用的 RES_N。
    CLI_H=$(classify "${CLI_RC}" "${CLI_SIG}" "${CLI_HANG}" "${SUM_CLI_AFTER}" "${RES_CLI}" "${CLI_MS}")
    SER_H=$(classify "${SER_RC}" "${SER_SIG}" "${SER_HANG}" "${SUM_SER_AFTER}" "${RES_SER}" "${SER_MS}")
    if [[ "${SUM_CLI_BEFORE}" != "0" || "${SUM_SER_BEFORE}" != "0" ]]; then
      echo "   !!! 前置门失败: SIGTERM 前已有 SUMMARY(server=${SUM_SER_BEFORE} client=${SUM_CLI_BEFORE})" >&2
      CLI_H="prereq_fail_summary_pre"; SER_H="prereq_fail_summary_pre"
      ROUND_RC=1
    fi

    CLI_ORET=$(optout_ret "${D}/client.log"); SER_ORET=$(optout_ret "${D}/server.log")

    echo "   client rc=${CLI_RC} sig=${CLI_SIG}(${CLI_SRC}) hang=${CLI_HANG} elapsed=${CLI_MS}ms SUMMARY ${SUM_CLI_BEFORE}->${SUM_CLI_AFTER} optout_ret=${CLI_ORET} ⇒ ${CLI_H}"
    echo "   server rc=${SER_RC} sig=${SER_SIG}(${SER_SRC}) hang=${SER_HANG} elapsed=${SER_MS}ms SUMMARY ${SUM_SER_BEFORE}->${SUM_SER_AFTER} optout_ret=${SER_ORET} ⇒ ${SER_H}"
    echo "   段: 运行中新增 ${DUR_N} / 退出后残留 ${RES_N}(ser 映射 ${RES_SER} / cli 映射 ${RES_CLI} / 无归属 ${RES_UNATTR})"

    echo "${R},${VIA},client,${CLI_RC},${CLI_SIG},${CLI_HANG},${CLI_MS},${SUM_CLI_AFTER},${RES_N},${DUR_N},${CLI_H},${CLI_SRC},${RES_CLI},${RES_SER},${RES_CLI},${RES_UNATTR},${TAG},${OPT_DISABLE},${CLI_ORET}" >> "${CSV}"
    echo "${R},${VIA},server,${SER_RC},${SER_SIG},${SER_HANG},${SER_MS},${SUM_SER_AFTER},${RES_N},${DUR_N},${SER_H},${SER_SRC},${RES_SER},${RES_SER},${RES_CLI},${RES_UNATTR},${TAG},${OPT_DISABLE},${SER_ORET}" >> "${CSV}"
  done
done

# ---- 汇总: 每个 via 的"零残留轮数 / 有 SUMMARY 轮数 / 退出码集合 / 假设计数" ----
# 判据三元组与 hypothesis 都直接算出来, 免得读取者再去翻每轮的 CSV 行。
{
  echo "{"
  echo "  \"generated_at\": \"$(date -Iseconds)\","
  echo "  \"tag\": \"${TAG}\","
  echo "  \"optout\": \"${OPT_DISABLE}\","
  echo "  \"preload\": \"${PRELOAD:-}\","
  echo "  \"driver\": \"${DRV}\","
  echo "  \"driver_md5\": \"${DRV_MD5}\","
  echo "  \"libipc_resolved\": \"${LIB_RESOLVED}\","
  echo "  \"libipc_md5\": \"${LIB_MD5}\","
  echo "  \"tree_head\": \"${TREE_HEAD}\","
  echo "  \"rounds_per_via\": ${ROUNDS},"
  echo "  \"grace_ms\": ${GRACE_MS},"
  echo "  \"hold_ms\": ${HOLD_MS},"
  echo "  \"lib_fast_ms\": ${LIB_FAST_MS},"
  echo "  \"per_via\": {"
  first=1
  for VIA in direct factory; do
    [[ ${first} -eq 0 ]] && echo ","
    first=0
    # ⛔ 起始门(R5): `n_rounds`/`n_res0` 旧版把 `startup_fail` 行也当成正常轮与零残留轮
    #    (它们 rc 非数字、residue 字面 0) ⇒ 会把"根本没起来"读成"跑过且零残留"。
    #    现在先按"rc 是数字"筛掉 startup_fail, 再统计。
    n_rounds=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && $4 ~ /^-?[0-9]+$/' "${CSV}" | wc -l)
    n_sum=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && $4 ~ /^-?[0-9]+$/ && $8>0' "${CSV}" | wc -l)
    n_res0=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && $4 ~ /^-?[0-9]+$/ && $9==0' "${CSV}" | wc -l)
    n_sfail=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && $4 !~ /^-?[0-9]+$/' "${CSV}" | wc -l)
    # ⛔ 分角色的"需补 SIGKILL"计数。旧版把两个角色混在一列里, 却叫 rounds_needing_sigkill
    #    ⇒ 一个 via 的该列最大可到 2×rounds, 与"轮数"不同量纲, 会读成"全部轮次都挂了"。
    n_hang_cli=$(awk -F, -v v="${VIA}" '$2==v && $3=="client" && $6==1' "${CSV}" | wc -l)
    n_hang_ser=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && $6==1' "${CSV}" | wc -l)
    rcs=$(awk -F, -v v="${VIA}" '$2==v && $3=="server"{print $4}' "${CSV}" | sort -u | paste -sd'|')
    # 服务端从 SIGTERM 到退出的耗时集合 —— 库路径应≈100~200ms(100ms 轮询), 应用路径
    # 则受 g_stop 支配(一个 sleep_ms(20) 节拍内)。⛔ 该字段现在是**每 pid 自计**的,
    # 不再被另一个 pid 的等待时长垫高(见文件头订正)。
    elaps=$(awk -F, -v v="${VIA}" '$2==v && $3=="server"{print $7}' "${CSV}" | paste -sd'|')
    # 假设计数(由 classify() 机器判定, 见文件头): 直接打出来, 便于"哪条路径赢了"一眼可读。
    hyp_s=$(awk -F, -v v="${VIA}" '$2==v && $3=="server" && NF>=11{print $11}' "${CSV}" \
            | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}')
    hyp_c=$(awk -F, -v v="${VIA}" '$2==v && $3=="client" && NF>=11{print $11}' "${CSV}" \
            | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}')
    # UF-004: driver 自报的 opt-out 返回值集合(none 臂恒为 na)。
    # ⛔ `missing` 一旦出现即"开关没生效", 与 `1`(生效) 必须能一眼分开。
    oret=$(awk -F, -v v="${VIA}" '$2==v && NF>=19{print $19}' "${CSV}" | sort -u | paste -sd'|')
    printf '    "%s": {"rounds": %s, "rounds_startup_fail": %s, "rounds_with_server_summary": %s, "rounds_with_zero_residue": %s, "server_needing_sigkill": %s, "client_needing_sigkill": %s, "server_rc_set": "%s", "server_exit_elapsed_ms": "%s", "server_hypotheses": "%s", "client_hypotheses": "%s", "optout_ret_set": "%s"}' \
      "${VIA}" "${n_rounds:-0}" "${n_sfail:-0}" "${n_sum:-0}" "${n_res0:-0}" "${n_hang_ser:-0}" "${n_hang_cli:-0}" \
      "${rcs:-}" "${elaps:-}" "${hyp_s:-}" "${hyp_c:-}" "${oret:-}"
  done
  echo
  echo "  },"
  # 判定规则**预注册**: 先写死规则与预期, 再看数据 —— 避免事后按结果挑解释。
  # ⛔ 规则与 classify() 逐条对应, 且**互斥**(见文件头 2026-09-18 订正):
  #    旧规则 "crash: by_signal>0 或 rc>128" 会把每一条挂起行(信号恒为 9, 我们补的刀)
  #    吞进崩溃 ⇒ 假设 (B) 结构上统计不到。
  cat <<'JSON'
  "decision_rule": {
    "order":            "按 hang → 信号 → rc 依次判; 命中即返回, 保证各类互斥",
    "crash":            "hang=0 且 by_signal ∈ {4,6,7,8,11}(SIGILL/SIGABRT/SIGBUS/SIGFPE/SIGSEGV) ⇒ 假设(A); ⛔显式排除 9",
    "hang":             "hang=1(唯一来源 = 看门狗补刀前留下的标记文件; 其 by_signal 必为 9 = 我们补的那一刀, ⛔不得当崩) ⇒ 假设(B)",
    "library_exit0_fast": "hang=0 且 rc=0 且 by_signal=0 且 SUMMARY=0 且 **本角色残留>0** 且 exit_elapsed_ms < lib_fast_ms ⇒ 假设(C): 库接管处理器后 std::exit(0), 栈未展开 ⇒ 析构缺失",
    "library_exit0_slow": "同上但 exit_elapsed_ms ≥ lib_fast_ms ⇒ **(C) 的机制 + 退出被拖慢/阻塞**, ⛔不得写成\"(C) 已证\"。⛔ 本条的存在理由(评审 R1): hang 的唯一来源是看门狗补刀 ⇒ 只有超 grace 才可能是 (B); 若不把 (C) 按耗时拆开, \"慢退出路径仍在 grace 内收掉进程\"会**结构性地**被归档成 (C)",
    "app_handler_alive":"hang=0 且 rc=0 且 by_signal=0 且 SUMMARY>0 ⇒ 应用处理器生效(对照组应有的形态)。⛔ 主判据只取 SUMMARY(评审 R3) —— 残留按 topic 取, 控制面/create|open 垃圾段会落进来, 用它当必要条件会造假阴性; 残留改由 residue_own/ser/cli 分列报告",
    "killed_ext":       "by_signal=9 但 hang=0 ⇒ 不是本脚本补的刀, 未判定(不得当崩也不得当挂)",
    "signal_other":     "by_signal>128 且信号 ∉ {4,6,7,8,9,11} ⇒ 其他信号, 未判定",
    "exit_nonzero":     "hang=0 且 0<rc≤128 且 by_signal=0 ⇒ 正常非 0 退出码(例如 client 有 RPC 失败), 不是崩",
    "exit0_other":      "rc=0 但 SUMMARY=0 且本角色残留=0 ⇒ 未判定",
    "undetermined_rc":  "rc 非数字的防御分支; 现在 startup_fail 行是字面写死的, 故本标签**当前不可达**",
    "prereq_fail_summary_pre": "SIGTERM 之前就已经打印了 SUMMARY ⇒ 本轮前置门失败, hypothesis 不可信",
    "startup_fail":     "本进程根本没起来(如 server 未 ready) ⇒ 无判据力, 且**不计入** rounds 与 rounds_with_zero_residue"
  },
  "columns_note": {
    "by_signal": "⛔ 挂起行恒为 9, 而那个 9 是**本脚本的看门狗补的刀** ⇒ 只看本列会重犯 D1(挂起读成崩溃)。请并读 signal_source",
    "signal_source": "none / watchdog_sigkill(我们补的) / external_sigkill(不是我们补的) / process",
    "residue_own": "本角色**自己映射过**的残留段数(= ser 行取 residue_ser, cli 行取 residue_cli)",
    "residue_ser/residue_cli": "按 /proc/<pid>/maps 的映射归属拆开(评审 R2)。一个共享段可被两端同时映射 ⇒ 两者之和可 > residue_this_topic; 无归属的单列 residue_unattr"
  },
  "corroboration": {
    "exit_elapsed_ms": "实测(依 driver 源码): 应用路径 ≈20~40ms(g_stop 立刻见效, 一个 sleep_ms(20) 节拍), 库路径 ≈100~200ms(100ms 轮询)。⛔ 旧注释的\"应用路径数量级秒级\"是错的 —— 两条主循环都是 while(!g_stop && now<hold_deadline)。⛔ 该字段**参与** (C) 的 fast/slow 三分(R1), 不再是纯佐证",
    "during_gain_this_topic": "运行中新增的段数。⛔ 它只说明本轮的段确实建起来了; 残留数看 residue_this_topic"
  },
  "preregistered_expectation": {
    "direct":  "app_handler_alive (SUMMARY 有, residue=0) —— 不经公共工厂, 不装监控线程",
    "factory": "library_exit0 (rc=0, SUMMARY 无, residue>0, elapsed≈100~200ms) —— 走 *IPCPtrMake ⇒ dzipc.cc:97 覆盖 main() 里 :406-407 装的处理器"
  }
}
JSON
} > "${OUT}/results.json"

echo "=== ---- 矩阵 ($(basename "${CSV}")) ----"
column -s, -t "${CSV}" 2>/dev/null || cat "${CSV}"
echo "=== ---- 汇总 ----"
cat "${OUT}/results.json"
echo "=== artifacts: ${OUT}"
exit "${ROUND_RC}"
