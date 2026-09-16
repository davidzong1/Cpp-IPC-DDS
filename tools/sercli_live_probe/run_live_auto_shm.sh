#!/usr/bin/env bash
# 端到端活体场景: **三个进程** —— 独立 base+2 只读探针 + ser-cli Auto 服务端 +
# ser-cli Auto 客户端(可选再加 topic_cat 作第四条证据)。
#
# 为什么必须是多进程而不是多线程: 同进程双线程的 Rig(test/test_sercli_auto_path.cpp)
# 是单测; 本脚本要的是"活体进程场景", 判据是外部脚本能逐行对账。
#
# 用法: tools/sercli_live_probe/run_live_auto_shm.sh [--count N] [--period-ms M] [--hold-ms H]
#                                                     [--topic NAME]
#                                                     [--with-topic-cat 0|1] [--via direct|factory]
#                                                     [--force-no-evidence]
# 退出码: 0 = client 的 N 次 RPC 全 ok; 1 = 有失败; 其它 = 环境/启动失败。
#
# ⛔ --topic 的默认值**不带前导斜杠**。这是**被判的行为本身**, 不是命名风格, 见下方
#    "topic 命名" 一节。要复现带 `/` 的失败证据: --topic "/live_auto_$(date +%s)_$$"。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
BIN="${HERE}/bin"
DRV="${BIN}/sercli_live_driver"
PROBE="${BIN}/base2_probe"
TCAT="${ROOT}/build/app/dzipc_topic_cat"

COUNT=8
PERIOD_MS=100
HOLD_MS=2500
WITH_TCAT=1
VIA=direct
TOPIC=""
EXTRA=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --count) COUNT="$2"; shift 2;;
    --period-ms) PERIOD_MS="$2"; shift 2;;
    --hold-ms) HOLD_MS="$2"; shift 2;;
    --topic) TOPIC="$2"; shift 2;;
    --with-topic-cat) WITH_TCAT="$2"; shift 2;;
    --via) VIA="$2"; shift 2;;
    --force-no-evidence) EXTRA+=(--force-no-evidence); shift;;
    *) echo "unknown arg $1" >&2; exit 2;;
  esac
done

for f in "${DRV}" "${PROBE}"; do
  [[ -x "$f" ]] || { echo "ERROR: $f 缺失, 先跑 tools/sercli_live_probe/build.sh" >&2; exit 2; }
done

# ---- topic 命名: 每次跑用**唯一 topic**, 且默认**不带前导斜杠** ----------------
#
# (1) 唯一性: 复用 topic 会让上一轮的池条目/控制面段变成下一轮的"占用"证据,
#     使结果不可复现。
#
# (2) 前导斜杠 —— 这是**被判的行为**, 不是风格。带 `/` 的 topic 会让本脚本
#     **永远测不到 SHM 腿**, 而且全程零报错:
#
#       shm_service_prefix()  (src/dzIPC/common/name_operator.cc:31) 对 topic 名**不做
#         sanitize**(它的注释称"传的已是处理过的名字", 而调用方传的是原始名),
#         于是 topic="/live_auto_123" 拼出段名 "dz_ipc_d3_/live_auto_123_ser_control2";
#       → ipc::shm::object_name()  (src/libipc/platform/posix/shm_posix.cpp:46) 只补**前导**
#         `/`, 段名里的**第二个** `/` 原样进 shm_open;
#       → POSIX 规定 shm_open 的 name 形如 /somename 且 somename **不含** `/`, 违反返回
#         **EINVAL(22)** —— 实测产品库自己打的就是这行:
#           fail shm_open[22]: /dz_ipc_d3_/live_auto_1789481050_2295567_ser_control2
#       → shm_ser_cli_ipc.cc:275 的 control_plane_.open() 失败 → :277 throw runtime_error
#       → auto_ser_cli_ipc.cc:324 的 leg->InitChannel() 抛出 → :350 catch(...) 吞掉 →
#         established=false → :668 withdraw_to_socket(ShmRendezvousTimeout)。
#
#     静默点: 业务消息继续走 socket 且**全部送达**, 上层拿不到任何错误 ⇒ 外部看起来
#     "跑通了", 只有逐行对账 switch_attempts/switch_fallbacks 和探针 shm_events 才看得见。
#     ⛔ 别据此以为"SHM 不通是环境问题": 同一个名字在 **pub/sub** 那条路上是通的, 因为
#     shm_topic_segment_name()(name_operator.cc:27)**有** sanitize —— 这个不对称正是缺陷。
#
#     所以默认值用**无斜杠**的 `live_auto_<ts>_<pid>`。带 `/` 的失败证据**保留**: 传
#       --topic "/live_auto_$(date +%s)_$$"
#     即可复现, 且脚本会在 run 目录留下 slash_topic=1 标记(见下), 不会与正常跑混淆。
if [[ -z "${TOPIC}" ]]; then
  TOPIC="live_auto_$(date +%s)_$$"
fi
DOMAIN=3
RUN="${ROOT}/build/live_runs/$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "${RUN}"

echo "=== topic=${TOPIC} domain=${DOMAIN} run=${RUN}"
if [[ "${TOPIC}" == /* ]]; then
  # 带前导斜杠 = 已知会阻断 SHM 的输入。照跑(证据要保留), 但**打标+告警**,
  # 免得把"回退 socket"当成 SHM 路径的回归。
  echo "slash_topic=1"  > "${RUN}/slash_topic.txt"
  echo "!! WARN: topic 带前导斜杠 —— ser/cli 段名会含第二个 '/', shm_open 必返 EINVAL(22),"
  echo "!!       SHM 腿**不会**建立且上层无报错(见脚本头 'topic 命名' 一节)。这是保留的失败证据,"
  echo "!!       不是 SHM 路径回归。要验 SHM 请去掉 '--topic' 用默认无斜杠名。"
else
  echo "slash_topic=0"  > "${RUN}/slash_topic.txt"
fi
ls /dev/shm > "${RUN}/shm_before.txt" 2>/dev/null || true
echo "=== /dev/shm 条目数(基线): $(wc -l < "${RUN}/shm_before.txt" 2>/dev/null || echo 0)"

PIDS=()
TCAT_SOCK_PID=""
TCAT_AUTO_PID=""
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  sleep 0.4
  for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null || true; done
}
trap cleanup EXIT

# ---- 1) 独立只读探针先在跑(必须早于任何一端, 才能拍到 Unknown 起始态)
"${PROBE}" --topic "${TOPIC}" --domain "${DOMAIN}" --hz 1000 > "${RUN}/probe.log" 2>&1 &
PROBE_PID=$!; PIDS+=("$PROBE_PID")
sleep 0.5

# ---- 2) topic_cat 两条腿各起一个观察者(只读)。两个都起是有原因的:
#
#  (a) `--transport socket -w true` = **横跨切换的握手见证者**。
#      ⛔ 为什么不是 `--transport auto`: sniffer.h:25-28 里 shm_sniffer 的构造**不接
#      watch_handshake**(只有 socket_sniffer 接); main.cc:152-171 又规定 Auto 下通道
#      一变(socket→SHM)必须**重建** sniffer ⇒ auto 下观测在**最需要它的那一刻**(切换后)
#      静默消失, 且不报错。钉住 socket 偏好则 transport_select.h:88-100 把 shm 条目滤掉,
#      选型恒为 socket 条目 ⇒ same_channel() 恒真 ⇒ **不重建**, 观测连续。
#      (socket 条目在切换后仍在池里: stop_data_plane() 不注销池登记。)
#  (b) `--transport auto` = **腿切换的第三方见证者**, 它会打出
#      "Transport changed to SHM (slot N)"(main.cc:161)。它自己在切换后失去观测,
#      但它要证的正是"切换发生了"这一件事。
if [[ "${WITH_TCAT}" == "1" && -x "${TCAT}" ]]; then
  "${TCAT}" --topic "${TOPIC}" --ser_or_topic true --transport socket -w true \
      > "${RUN}/topic_cat_socket.log" 2>&1 &
  TCAT_SOCK_PID=$!; PIDS+=("$TCAT_SOCK_PID")
  "${TCAT}" --topic "${TOPIC}" --ser_or_topic true --transport auto -w true \
      > "${RUN}/topic_cat_auto.log" 2>&1 &
  TCAT_AUTO_PID=$!; PIDS+=("$TCAT_AUTO_PID")
fi

# ---- 3) 服务端(常驻, 直到被 kill)
# `${EXTRA[@]+"${EXTRA[@]}"}`: set -u 下空数组的安全展开(bash<4.4 直接 "${EXTRA[@]}"
# 会报 unbound; 而 `${EXTRA[@]:-}` 会塞进一个空字符串, 被驱动当成未知参数)。
"${DRV}" --role server --topic "${TOPIC}" --domain "${DOMAIN}" --auto --via "${VIA}" \
    ${EXTRA[@]+"${EXTRA[@]}"} > "${RUN}/server.log" 2>&1 &
SER_PID=$!; PIDS+=("$SER_PID")
# 等 SERVER ready
for _ in $(seq 1 100); do grep -q "^SERVER ready" "${RUN}/server.log" 2>/dev/null && break; sleep 0.1; done
sleep 1.0

# ---- 4) 客户端(独立进程), 跑完 RPC 再 hold, 给探针留观测窗口
"${DRV}" --role client --topic "${TOPIC}" --domain "${DOMAIN}" --auto --via "${VIA}" \
    --count "${COUNT}" --period-ms "${PERIOD_MS}" --hold-ms "${HOLD_MS}" ${EXTRA[@]+"${EXTRA[@]}"} \
    > "${RUN}/client.log" 2>&1
CLI_RC=$?
echo "=== client rc=${CLI_RC}"

# ---- 5) 收尾: 先停服务端再停探针, 让探针拍到服务端消失后的状态
kill "$SER_PID" 2>/dev/null || true
sleep 1.5
kill "$PROBE_PID" 2>/dev/null || true
# ⛔ 这里**不能**写 kill "${TCAT_PID:-0}": `:-0` 在变量未赋值时展开成 0, 而 `kill 0`
# 打的是**整个进程组** —— 会把这个脚本自己和父 shell 一起打掉。启动分支要求
# `-x "$TCAT"`, kill 分支却不查, 两者一旦不一致(如 --with-topic-cat 1 但二进制
# 不可执行)就会命中。故一律用 `${VAR:-}` + 非空判断。
[[ -n "${TCAT_SOCK_PID}" ]] && kill "${TCAT_SOCK_PID}" 2>/dev/null || true
[[ -n "${TCAT_AUTO_PID}" ]] && kill "${TCAT_AUTO_PID}" 2>/dev/null || true
sleep 0.8
cleanup
trap - EXIT

ls /dev/shm > "${RUN}/shm_after.txt" 2>/dev/null || true

echo "=== ---- client.log (RPC 逐条) ----"
cat "${RUN}/client.log"
echo "=== ---- server.log ----"
cat "${RUN}/server.log"
echo "=== ---- probe.log (base+2 只读观测) ----"
cat "${RUN}/probe.log"
if [[ -s "${RUN}/topic_cat_auto.log" ]]; then
  echo "=== ---- topic_cat_auto.log (腿切换见证者: Transport changed) ----"
  # ⛔ 别写 `Type: (SHM|SOCKET)`: main.cc:333-335 的实际格式是
  #   "Type: Service(" + (is_shm?"SHM":"SOCKET") + ")"   (:358-359 是 "Type: Topic(" 的对称分支)
  # 传输名在**括号里面**, 所以 `Type: SHM` 永远匹配不到 —— 该分支会静默打印空块,
  # 而"空块"与"确实没切"在屏幕上看不出区别。这是本仓反复出现的静默失效形态。
  # 也别改成 `^Type:` 锚定: topic_cat 每帧以 \x1b[2J\x1b[H 开头且不换行, 帧会黏在前一行尾部,
  # 用行中子串匹配才稳。
  # `Transport changed to %s (slot %d)`(main.cc:161) 是**主见证**(重建事件);
  # 每帧的 `Type:` 反映的是 sniffer **当前挂的腿**(link_type, main.cc:36) —— 两者互补。
  tr -d '\033' < "${RUN}/topic_cat_auto.log" | sed 's/\[[0-9;]*[A-Za-z]//g' \
    | grep -aE "Transport changed|Found |Type: (Service|Topic)\(" | head -20
fi
if [[ -s "${RUN}/topic_cat_socket.log" ]]; then
  echo "=== ---- topic_cat_socket.log (横跨切换的握手见证者 -w) ----"
  tr -d '\033' < "${RUN}/topic_cat_socket.log" | sed 's/\[[0-9;]*[A-Za-z]//g' \
    | grep -aE "path_state=|Handshake watch|frames=" | tail -20
fi
echo "=== logs: ${RUN}"
exit "${CLI_RC}"
