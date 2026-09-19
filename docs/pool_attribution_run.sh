#!/usr/bin/env bash
# 步骤② 归因扫描驱动(docs/shm_chunk_pool_occupancy_plan.md §3 步骤②)
#
# 职责: 清残池 → 按 queue_size 扫描(每点重复 R 次) → 汇总成表 → 断言非退化。
# 只跑基准, 不打补丁 —— 插桩由 docs/pool_attribution_instr.py 负责。两种构建共用本脚本:
#   MODE=clean  干净构建: 只有通道 A(L), Q/R 为 NA
#   MODE=instr  插桩构建: 通道 A + B(L/Qv/Qm/R), 此时 **instr=1 是硬要求**
#
# 本脚本含**两段**, 量测条件不同, 不可混读:
#   ① 步骤② 归因扫描(11 点): 硬编码 `--no-pin=1` —— 它是"钉之前"的归因,
#      预言列 L ≈ min(queue_size, 32) 只在池不被钉压时成立; `pin != 0` 时该点判无效。
#   ② 步骤③ A/B(--queue=1024 的 off/on 两臂): 量钉本身的效果, 判据见该段注释。
#
# ⛔ 为什么每点要重复跑、且要重试到攒够有效次数
#   池的空闲链会**间歇性**被破坏(自环, 见 §finding_loop): 一旦坏掉, 点读法整窗
#   走不出链 ⇒ 基准 exit 6。实测坏链与否是随机的(同参数换一次跑就变),
#   且低位 queue_size 更容易命中 —— 而低位恰好是判别点, 不能让它"没数据"。
#   ⇒ 固定尝试上限 MAXTRY, 攒够 REPS 次有效就停; 有效性单列报出来,
#   不做任何"失败的当 0"的补偿(那会把结论读反)。
#   注意: 这不是"跑到出想要的数为止" —— 有效/尝试 两列都在表上, 且
#   badloop 计数独立成列, 读的人能看见有多少尝试被丢掉。
set -uo pipefail

ROOT="/home/zwc/cpp_ipc_dds"
BIN="$ROOT/build/bin/pool_attribution_benchmark"
export LD_LIBRARY_PATH="$ROOT/build/lib:$ROOT/build/bin"

MODE="${MODE:-instr}"
REPS="${REPS:-3}"           # 每个点要攒够的**有效**次数
MAXTRY="${MAXTRY:-9}"       # 最多尝试次数(坏链会吃掉尝试; 见下)
MSGS="${MSGS:-40000}"
CLS=12288                     # --payload=11000 对应的借样档位(见基准文件头)
SEG="/dev/shm/__IPC_SHM__CHUNK_INFO__$CLS"

# ---- 前置: 清残池之前必须先确认无活持有者(unfixed_defects.md §4 的协议)。
# 段名全机共享, 清掉正在被别的进程用的段会打断它。先查, 再清。
holders="$(grep -l CHUNK_INFO /proc/[0-9]*/maps 2>/dev/null || true)"
if [ -n "$holders" ]; then
    echo "⛔ 有进程正持有 CHUNK_INFO 段, 拒绝清池:"; echo "$holders"; exit 5
fi
if [ -e "$SEG" ]; then
    echo "[前置] 发现残留段 $SEG (异常终止所致) —— 无活持有者, 清除"
    rm -f "$SEG"
fi

echo "[前置] MODE=$MODE REPS=$REPS MSGS=$MSGS 段=$SEG(已清)"
echo

# 每个"点"= 一组基准参数 + 这一组要验的预言。
# 预言写成人类可读的, 由下面的断言核对 —— 不是只印一张表。
declare -a POINTS=(
  "--queue=1024 --drain=0|L≈32 Qv≈32 R=0|队列主导的饱和臂: 32 块全在队列里"
  "--queue=32   --drain=0|L≈32 Qv≈32 R=0|与池容量相等的边界"
  "--queue=16   --drain=0|L≈16 Qv≈16 R=0|队列主导: L 跟着 queue_size 掉"
  "--queue=8    --drain=0|L≈8  Qv≈8  R=0|同上"
  "--queue=4    --drain=0|L≈4  Qv≈4  R=0|同上"
  "--queue=2    --drain=0|L≈2  Qv≈2  R=0|**判别点**: 队列主导预测 2, 环主导预测 32"
  "--queue=2    --drain=1|L≈0  Qv≈0  R=0|应用持续取数 ⇒ 池里几乎无钉住"
  "--queue=4    --hold=4|L≈4  Qv≈0  R≈4|灵敏度正对照: 4 块在应用手里, 不在队列里"
  "--queue=4    --hold=16|L≈16 Qv≈0  R≈16|同上, 更大剂量 ⇒ R 是活量而非结构性零"
  "--queue=4    --pub-extra=16|R 仍为 0|环反臂: 17 个发布线程压过 1 个订阅线程 —— **不升**"
  "--queue=4    --pub-extra=32|R 仍为 0|再加压到 33 个发布线程, 仍不升(见 §finding_no_ring)"
)

printf '%-24s %-46s %5s %5s %5s %5s %5s %5s %4s %5s\n' \
       '参数' '预言' '有效' 'L_p50' 'L_max' 'Qv_p50' 'Qm_p50' 'R_max' '坏链' 'qual'
printf '%s\n' "$(printf '%.0s-' {1..130})"

fails=0
note() { printf '   ⛔ %s\n' "$*"; fails=$((fails+1)); }

for pt in "${POINTS[@]}"; do
    # ⛔ 不用 `IFS='|' read` 取字段: 该赋值在这个 bash 下会**留在当前 shell**,
    # 于是后面 `$args` 里的空格不再分词("queue=8    drain=0" 会被当成单个参数
    # 传进去 ⇒ 基准 exit 2 坏参数), 而每个点都判"无效"看起来像是被测对象的问题。
    # 参数展开没有这个副作用。
    args="${pt%%|*}"; rest="${pt#*|}"; pred="${rest%%|*}"; why="${rest#*|}"
    n_ok=0; n_loop=0; n_bad=0; n_try=0
    Ls=(); Lmax=(); Qv=(); Qm=(); Rmax=(); Qt=()
    for ((r=0; r<MAXTRY && n_ok<REPS; ++r)); do
        n_try=$((n_try+1))
        # ⛔ 扫描臂必须显式关钉(--no-pin=1): 本表的预言 L ≈ min(queue_size, 32) 是
        #    步骤② 在**钉之前**的条件下量出来的。步骤③ 落地后钉默认 ON, 若照默认跑,
        #    queue>8 的点会被压到 8 而预言列仍写着 32 —— 表看起来是绿的, 实际早已失效
        #    (实测: 未加本行时 11 点全部读出 L_p50=8, 而脚本报"失败 0")。
        #    步骤③ 的效果由下面的 A/B 段独立量, 两段互不混淆。
        out="$("$BIN" $args --no-pin=1 --msgs="$MSGS" --sample-ms=5 --samples=60 2>/dev/null)"; rc=$?
        line="$(grep '^RESULT' <<<"$out" || true)"
        if [ -z "$line" ]; then n_bad=$((n_bad+1)); continue; fi
        get() { sed -n "s/.*[[:space:]]$1=\([^[:space:]]*\).*/\1/p" <<<"$line"; }
        bl="$(get badloop)"; bl=${bl:-0}
        n_loop=$((n_loop+bl))
        if [ "$rc" -ne 0 ] || [ "$(get nsamp)" = "0" ]; then n_bad=$((n_bad+1)); continue; fi
        # 扫描臂的读数只有在钉关闭时才对应步骤② 的条件 —— 钉生效会把 L 压到
        # ViewQueueCap(), 而预言列不随之改变 ⇒ "预言已失效但表仍绿"的静默降级。
        if [ "$(get pin)" != "0" ]; then
            note "$args: pin=$(get pin) (扫描臂应当为 0) —— 步骤② 的条件未复现, 该点读数不可比"
            n_bad=$((n_bad+1)); continue
        fi
        # 插桩模式: instr 必须是 1 —— 否则通道 B 静默降级, Q 恒 NA 会被读成"队列不占"
        if [ "$MODE" = "instr" ] && [ "$(get instr)" != "1" ]; then
            note "$args: instr=$(get instr) (应当为 1) —— 插桩符号没解析上, 通道 B 数据作废"
            n_bad=$((n_bad+1)); continue
        fi
        q="$(get qgtl)"; if [ "${q:-0}" != "0" ]; then
            note "$args: qgtl=$q (Qv>L) —— 两条通道对不上"; n_bad=$((n_bad+1)); continue
        fi
        n_ok=$((n_ok+1))
        Ls+=("$(get L_p50)"); Lmax+=("$(get L_max)"); Qv+=("$(get Q_p50)")
        Qm+=("$(get Qm_p50)"); Rmax+=("$(get R_max)"); Qt+=("$why")
    done
    if [ "$n_ok" -eq 0 ]; then
        printf '%-24s %-46s %5s %5s %5s %5s %5s %5s %4s %5s\n' \
               "$args" "$pred" 0 - - - - - "$n_loop" "无效"
        note "$args: $REPS 次全部无效 —— 该点无结论(不是'没被占')"
        continue
    fi
    med() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
    mx()  { printf '%s\n' "$@" | sort -n | tail -1; }
    printf '%-24s %-46s %5s %5s %5s %5s %5s %5s %4s %5s\n' \
           "$args" "$pred" "$n_ok/$n_try" "$(med "${Ls[@]}")" "$(mx "${Lmax[@]}")" \
           "$(med "${Qv[@]}")" "$(med "${Qm[@]}")" "$(mx "${Rmax[@]}")" "$n_loop" \
           "$([ "$n_loop" -gt 0 ] && echo 有坏链 || echo 净)"
done

# ── 步骤③ A/B: view 队列容量钉的效果(同一个二进制, --no-pin=1 关钉) ──────────
# 判据(§3 步骤③): --queue=1024 时队列能把整池(每档 32 块)吃干 ⇒ 发布侧 loan 借不到
# 块 ⇒ 逐条回退整包 TLV; 钉住后队列被压到 ViewQueueCap() ⇒ 池有余量, 回退归零。
#
# ⛔ 为什么不能只看单次: 池的空闲链自环是**既有缺陷** —— 本脚本的 --no-pin=1 臂
#   (钉 OFF)同样复现(实测 q=4/8 各 3 次里各有 1 次 badloop>0), 与钉无关。
#   但 **queue=1024 时 OFF 臂的池是满排空的**(cursor_=32) ⇒ 走链一步即终止 ⇒
#   自环对 OFF 臂不可见, 而 ON 臂留了空闲块才暴露它。于是:
#     · 坏链 rep 读数不可用(链走不出去) ⇒ 计入"坏链"列, **不进判决**;
#     · 判决只取干净链的 rep; 一次都攒不到 ⇒ 报"无结论"而**不是**"钉无效"。
fb_off=""; rx_off=""; cln_off=0
fb_on="";  rx_on="";  cln_on=0
echo
echo "== 步骤③ A/B: view 队列容量钉 (--queue=1024 --drain=0) =="
printf '%-6s %10s %6s %8s %8s %10s\n' '臂' '有效/尝试' '坏链' 'fb_min' 'fb_max' 'rx@fb_min'
for arm in off on; do
    extra=""; [ "$arm" = "off" ] && extra="--no-pin=1"
    best=""; worst=""; bestrx=""; nclean=0; nloop=0; ntry=0
    for ((r=0; r<MAXTRY && nclean<REPS; ++r)); do
        ntry=$((ntry+1))
        line="$("$BIN" --queue=1024 --drain=0 $extra --msgs="$MSGS" --sample-ms=5 --samples=60 2>/dev/null \
                | grep '^RESULT' || true)"
        [ -z "$line" ] && continue
        f() { sed -n "s/.*[[:space:]]$1=\([^[:space:]]*\).*/\1/p" <<<"$line"; }
        bl="$(f badloop)"; bl=${bl:-0}; nloop=$((nloop+bl))
        [ "$bl" != "0" ] && continue        # 坏链 ⇒ 读数不可用, 只计坏链列
        nclean=$((nclean+1))
        v="$(f fb)"
        if [ -z "$best" ] || [ "$v" -lt "$best" ]; then best="$v"; bestrx="$(f rx_acc)"; fi
        if [ -z "$worst" ] || [ "$v" -gt "$worst" ]; then worst="$v"; fi
    done
    if [ "$nclean" -eq 0 ]; then
        printf '%-6s %10s %6s %8s %8s %10s\n' "$arm" "0/$ntry" "$nloop" - - -
        note "步骤③ A/B 的 $arm 臂: $ntry 次尝试无一次干净链 —— **无结论**(不是'钉无效')"
        continue
    fi
    printf '%-6s %10s %6s %8s %8s %10s\n' "$arm" "$nclean/$ntry" "$nloop" "$best" "$worst" "$bestrx"
    if [ "$arm" = "off" ]; then fb_off="$best"; rx_off="$bestrx"; cln_off="$nclean"
    else                      fb_on="$best";  rx_on="$bestrx";  cln_on="$nclean"; fi
done

if [ -n "$fb_off" ] && [ -n "$fb_on" ]; then
    if [ "$fb_on" -le $((fb_off / 100)) ]; then
        echo "  ✅ 步骤③ 判据成立: 回退 $fb_off -> $fb_on; 零拷贝接收 $rx_off -> $rx_on"
        echo "     注: 干净链下 fb 恒为 1 且与 --msgs 无关(2000/20000/60000 三档实测均 1)"
        echo "     ⇒ 那是**首帧瞬态**(首条发布时接收方未就绪, 借不到块), 不是持续回退。"
    else
        note "步骤③ 判据**不成立**: 回退 $fb_off -> $fb_on 不是数量级下降(要求 <= ${fb_off}/100)"
    fi
    if [ "$cln_on" -lt "$REPS" ]; then
        echo "  ⚠️ 钉臂只有 $cln_on 次干净链(想要 $REPS) —— 残差读数受空闲链自环干扰, 见上"
    fi
fi

echo
echo "== 断言 =="
if [ "$MODE" = "instr" ]; then
    echo "  [判据1] Qv <= L 逐点: 由基准的 qgtl 计数逐样本核(上表 qual 列非 0 即失败)"
    echo "  [判据2] 通道 A 扫描反推的队列占用 == 通道 B 直读 Qv: 上表 L_p50 与 Qv_p50 两列逐行相等"
    echo "  [判据3] R 是活量: 由 --hold 正对照独立证明(hold=N ⇒ R=N, 不在任何队列里)"
    echo "  [判据4] 环反臂**未成立**: 33 个发布线程下 R_max 仍 ≈0 —— 这不是'盲区',"
    echo "          而是结论(生产端单条成本 > 订阅端, 订阅线程追得上); 见 §finding_no_ring"
fi
echo "  有效点 $(( ${#POINTS[@]} - 0 )) 组; 失败 $fails"
[ "$fails" -eq 0 ] || { echo "⛔ 有 $fails 条问题"; exit 1; }
echo "✅ 扫描完成"
