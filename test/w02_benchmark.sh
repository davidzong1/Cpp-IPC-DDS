#!/usr/bin/env bash
# W02 统一跨进程基准 · 启动脚本
# ============================================================================
# 依据：docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md §4 W02、
#      §12（证据目录与机器可读清单）、§13.1（发送计划独立于接收完成）。
#
# 这个脚本只做三件事，别的都不做（避免"脚本偷偷改了测量条件"）：
#   ① 显式重建构建（配置 + 编译）—— §10.6 的假通过防线：file(GLOB) 是配置期展开的，
#      不重跑 cmake 会漏掉新增源文件、保留已删除实现的旧产物；
#   ② 清理**本次运行拥有的** SHM/topic 残留后拉起基准（不做全局 rm /dev/shm/*）；
#   ③ 把 git 状态、实际加载库、配置哈希、结果目录打印出来，便于归档。
#
# 用法：
#   test/w02_benchmark.sh smoke               # 四档(64B/1KiB/64KiB/1MiB) × TLV/A/B 正确性用例
#   test/w02_benchmark.sh smoke-dds           # 再加 CycloneDDS(udp/iceoryx) 两档
#   test/w02_benchmark.sh dds                 # 只跑 CycloneDDS 两档（需 RouDi）
#   test/w02_benchmark.sh matrix              # 默认矩阵（3 路径 × 4 档 × 3 工作负载 × 定速）
#   test/w02_benchmark.sh raw -- --path=tlv --payload=1024 --workload=full
#
# 环境变量：
#   BUILD_DIR    构建目录                （默认 <repo>/build）
#   RUN_ID       运行编号                （默认 日期-时间-W02）
#   ARTIFACTS    结果根目录              （默认 <repo>/artifacts/perf）
#   ROUDI        RouDi 可执行文件        （默认从 PATH 找 iox-roudi）
#   W02_SKIP_BUILD=1  跳过重建（只在你确定二进制是最新的时候用）
#   W02_PURPOSE        run 用途：evidence（默认，**显式认领**权威指针）/ verification / experiment
#   W02_CITATION_AUTHORITY  证据 run 的 run_id（复核轮必须给，指向现行证据 run）
#
# ⚠️ 权威指针纪律（t50/R-3）：**认领 currently_citable 是显式动作**。
#   本脚本默认按 `--purpose=evidence` 运行（它生成本 run 的证据）；
#   ⛔ 复核/验证轮**必须**显式改为：W02_PURPOSE=verification W02_CITATION_AUTHORITY=<证据 run>
#      —— 否则（缺 --purpose）基准会按 verification 处理且**不认领**权威指针，
#      你产出的目录不会被任何人当作现行 run（这是"默认安全"：宁可不认领，不得悄悄抢）。
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build}"
ARTIFACTS="${ARTIFACTS:-$REPO/artifacts/perf}"
MODE="${1:-check}"
shift || true
EXTRA=()
if [[ "${1:-}" == "--" ]]; then shift; EXTRA=("$@"); fi

say() { printf '\n[W02] %s\n' "$*"; }

say "仓库: $REPO"
say "构建目录: $BUILD_DIR"

if [[ "${W02_SKIP_BUILD:-0}" != "1" ]]; then
  say "① 重新执行 CMake 配置（§10.6：配置期 GLOB，不重跑会漏编/留旧）"
  cmake -S "$REPO" -B "$BUILD_DIR" >/dev/null
  say "② 重新编译 xproc_benchmark"
  cmake --build "$BUILD_DIR" --target xproc_benchmark -j"$(nproc)" >/dev/null
fi

BIN="$BUILD_DIR/bin/xproc_benchmark"
[[ -x "$BIN" ]] || { echo "[W02] 找不到可执行文件: $BIN" >&2; exit 2; }

say "③ 二进制与库指纹"
sha256sum "$BIN" | sed 's/^/     exe  /'
LIB="$(ldd "$BIN" | awk '/libipc\.so/ {print $3; exit}')"
[[ -n "${LIB:-}" ]] && sha256sum "$LIB" | sed 's/^/     lib  /' || echo "     lib  (未动态链接 libipc)"
say "git HEAD: $(git -C "$REPO" rev-parse HEAD)"
say "git status --short:"
git -C "$REPO" status --short | sed 's/^/     /' || true

RUN_ID="${RUN_ID:-$(date +%Y%m%d-%H%M%S)-W02}"
OUT_DIR="$ARTIFACTS/$RUN_ID"

# 只清理本次运行拥有的对象：control block 名以 run_id 为前缀，topic 名同样带 run_id。
say "④ 清理本运行编号的 SHM 残留（不做全局清理）"
for f in /dev/shm/w02ctl_"${RUN_ID}"* /dev/shm/*w02_"${RUN_ID}"*; do
  [[ -e "$f" ]] && rm -f "$f" && echo "     removed $f"
done
true

if [[ "$MODE" == "dds" || "$MODE" == "smoke-dds" ]]; then
  if ! pgrep -x iox-roudi >/dev/null; then
    ROUDI_BIN="${ROUDI:-$(command -v iox-roudi || true)}"
    if [[ -n "${ROUDI_BIN:-}" && -x "$ROUDI_BIN" ]]; then
      say "⑤ 启动 RouDi（iceoryx 守护进程）: $ROUDI_BIN"
      nohup "$ROUDI_BIN" >/dev/null 2>&1 &
      sleep 2
    else
      say "⑤ 未找到 iox-roudi：CycloneDDS+iceoryx 档将走预检失败路径（显式标记，不静默跳过）"
    fi
  fi
fi

case "$MODE" in
  smoke)     ARGS=(--smoke) ;;
  smoke-dds) ARGS=(--smoke-dds) ;;
  dds)       ARGS=(--smoke --dds-only) ;;
  matrix)    ARGS=(--compare=tlv,dzflat-a,dzflat-b
                   --payloads=64,1024,65536,1048576
                   --duration=3 --warmup=0.5 --rate=1000) ;;
  raw)       ARGS=() ;;
  *)         echo "未知模式: $MODE（smoke|smoke-dds|dds|matrix|raw）" >&2; exit 2 ;;
esac

PURPOSE="${W02_PURPOSE:-evidence}"
CITE=()
[[ -n "${W02_CITATION_AUTHORITY:-}" ]] && CITE=(--citation-authority="${W02_CITATION_AUTHORITY}")
if [[ "$PURPOSE" == "evidence" && -n "${W02_CITATION_AUTHORITY:-}" ]]; then
  echo "[W02] ⛔ 自相矛盾：purpose=evidence 又给了 citation-authority ⇒ 会按复核轮处理（不认领权威）" >&2
fi

CMDLINE=("$BIN" "${ARGS[@]}" "${EXTRA[@]}"
         --run-id="$RUN_ID" --out-dir="$OUT_DIR"
         --purpose="$PURPOSE" "${CITE[@]}"
         --dds-uri-udp="file://$REPO/test/w02_dds_udp.xml"
         --dds-uri-iox="file://$REPO/test/w02_dds_iox.xml")

say "⑥ 拉起基准（stdout 同时留在 $OUT_DIR/.. 之外的 console 与本次目录）"
printf '     %s\n' "${CMDLINE[*]}"
mkdir -p "$OUT_DIR"
printf '%s\n' "${CMDLINE[*]}" > "$OUT_DIR/command.txt"
set +e
"${CMDLINE[@]}" 2>&1 | tee "$OUT_DIR/console.log"
RC=${PIPESTATUS[0]}
set -e

say "⑦ 结果"
printf '     退出码: %s\n' "$RC"
printf '     目录  : %s\n' "$OUT_DIR"
if [[ -f "$OUT_DIR/manifest.json" ]]; then
  python3 - "$OUT_DIR/manifest.json" <<'PYEOF' 2>/dev/null || true
import json,sys
m=json.load(open(sys.argv[1])); ri=m.get("run_identity",{})
print("     purpose        : %s (explicit=%s)"%(ri.get("purpose"),ri.get("purpose_explicit")))
print("     currently_citable: %s"%m.get("currently_citable"))
if ri.get("authority_undeclared"):
    print("     ⚠️ 本轮未认领权威指针（缺 --purpose=evidence）⇒ 不会被当作现行 run")
PYEOF
fi
ls -1 "$OUT_DIR" | sed 's/^/     /'
if [[ -f "$OUT_DIR/verdict.md" ]]; then
  printf '\n----- verdict.md 摘要 -----\n'
  sed -n '1,40p' "$OUT_DIR/verdict.md"
fi
exit "$RC"
