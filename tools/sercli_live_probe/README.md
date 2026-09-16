# sercli_live_probe —— ser-cli Auto 的跨进程活体驱动 + 独立 base+2 只读探针

**为什么有这个目录**（仓库事实，不是偏好）：

| 需求 | 仓库现状 |
|---|---|
| 跨进程 ser-cli 可执行 | ❌ `exec/` 只有 `dzipc_list` / `dzipc_pub` / `dzipc_topic_cat`，**没有 ser-cli** |
| Python 双进程 ser-cli 驱动 | ❌ `python/ipc_demo.py` 的 `pick_ipc_type()` 只映射 shm/socket，**传不了 Auto** |
| Auto ser-cli 既有消费者 | ⚠️ 只有 gtest `test/test_sercli_auto_path.cpp`，其 `Rig` 是**同进程双线程** |
| 切到 SHM 后仍能观测握手通道 | ⚠️ 见下方「关于 `-w`」—— **`--transport auto` 下会静默消失**，但 `--transport socket` 下**不会** |

### 关于 `-w`（订正一处早先的过度断言）

`dzipc_topic_cat -w` 的观测挂在 **socket 腿**上（`sniffer.h:25-28`：`shm_sniffer` 的构造
**不接** `watch_handshake`，只有 `socket_sniffer` 接）。在 `--transport auto` 下，通道一变
（Auto 从 socket→SHM）`main.cc:152-171` 就**必须重建** sniffer ⇒ 观测在**最需要它的那一刻**
（切换之后）静默消失且不报错 —— 这正是 `main.cc:228-237` 那条 note 说的事。

**但这不是不可避免的**：改成 `--transport socket -w true` 即可。理由是
`transport_select.h:88-100` 的 `transport_allowed()` 在 Socket 偏好下把 shm 条目滤掉，
选型**恒为 socket 条目** ⇒ `Selection::same_channel()`（只比 `shm` 与 `domain_id`）恒真
⇒ **不重建**，观测连续；而 socket 条目在切换后**仍在池里**（`stop_data_plane()` 不注销池登记），
所以不会断货。（此订正来自 测试验证-claude 的复核，我逐条按 file:line 复核后采纳。）

⇒ `run_live_auto_shm.sh` 因此起**两个**只读观察者：
- `--transport socket -w true` —— **横跨切换的握手见证者**（电平采样）；
- `--transport auto` —— **腿切换的第三方见证者**（会打出 `Transport changed to SHM`）。

即便如此，**独立探针仍然必要**：topic_cat 是「电平采样 + 依赖池选型」，而 base2_probe 是
「事件流 + 与池/sniffer 完全无关」。两者互补才是「独立」的意义。

所以：**活体 = 两个独立 OS 进程 + IPC_AUTO**；**切换后仍可观测 = 独立进程的 base+2 探针**。
两者都不是单测，也都不是 `-w` 能替代的。

## 构建（不改任何 CMakeLists）

```bash
tools/sercli_live_probe/build.sh        # -> tools/sercli_live_probe/bin/{sercli_live_driver,base2_probe}
```

直编直链仓库已有的头与 `build/lib/libipc.so`，**不碰 CMake**，因此不会与任何人的
CMake 改动相撞（也绕开 `test/CMakeLists.txt` 的 `file(GLOB)` 需重跑 cmake 那条）。

## `sercli_live_driver` —— 跨进程 Auto ser-cli

```bash
# 服务端（常驻到 SIGTERM）
sercli_live_driver --role server --topic T --domain D --auto [--via direct|factory]
# 客户端（N 次 RPC；退出码 0 当且仅当 N 次全 ok）
sercli_live_driver --role client --topic T --domain D --auto --count N \
                   [--period-ms M] [--hold-ms H] [--timeout-ms X] [--via direct|factory]
```

`--via` 两条路**都真的走 Auto**，没有 shm/socket 硬编码：

- `direct`（默认）—— 直接构造 `autopath::auto_ser_ipc` / `auto_cli_ipc`。这就是
  `IPC_AUTO` 在公共工厂里被派发到的**同一个类**（`src/dzIPC/server_ipc.cc:131-136`、
  `:213` 起）。额外好处：能读 `status()`，于是 `decision=` / `fallback=` 有真值。
- `factory` —— 走公共工厂 `ServerIPCPtrMake(..., IPC_AUTO, ...)`（`dzipc.h:49`），
  证明**产品公共 API 的 Auto 路径**本身可用。代价：公共包装只暴露
  `transport_current()`，故 `decision=` / `fallback=` 打 `na`。

`--force-no-evidence`（仅 `direct`）走与真实跨主机**完全相同**的代码路径
（`decision=NoEvidence`，保持 socket），用于复现**回退腿**。

### 输出（stdout 单行、ASCII、无颜色，供逐行对账）

```
CLIENT ready pid=.. topic=.. domain=.. via=direct count=8
HANDSHAKE completed=1
RPC seq=0 ok=1 res=2,3,4 PATH kind=Shm decision=PeerInPool fallback=none state=Active
...
FINAL kind=Shm decision=PeerInPool fallback=none state=Active
FINAL counters switch_attempts=1 switch_successes=1 switch_fallbacks=0 dup_delivery_detected=0 requests_in_switch_window=0 evidence_kind=..
SUMMARY sent=8 ok=8 failed=0
```

服务端侧：

```
SERVER ready pid=.. topic=.. domain=.. via=direct
CB n=1 PATH kind=Shm decision=PeerInPool fallback=none state=Active
SERVER SUMMARY cb=8
```

跨进程对账口径：**客户端 `SUMMARY ok=` 必须等于服务端 `SERVER SUMMARY cb=`**
（callback 次数 = 成功返回次数 ⇒ 无重复副作用），且 `dup_delivery_detected=0`。

## `base2_probe` —— 独立进程的 base+2 只读观测

```bash
base2_probe --topic T --domain D [--hz 1000] [--hold-ms H] [--no-shm-watch] [--quiet]
```

- 只 `create/connect/receive_nowait/close`，**从不 send**；
- 解码复用产品自己的字节布局（`handshake_probe.h`），不做第二套解读；
- base+2 是**组播**，多一个监听者不偷包（该结论见 `handshake_probe.h` 的实测注释）；
- 对 `/dev/shm` **只 `opendir` 列名单**，绝不 open/create，不产生任何段。

输出：

```
PROBE ready pid=.. port=.. topic=.. domain=.. hz=1000 shm_watch=1
HS t=812 server=Unknown(0) client=(none) frames=3 undec=0
HS t=1104 server=ProposeShm(1) client=Unknown(0) frames=9 undec=0
HS t=1210 server=ConfirmShm(2) client=ConfirmShm(2) frames=14 undec=0
SHMSEG +__IPC_SHM__...__<topic>... t=1205
BEAT t=2000 server_frames=20 client_frames=20 frames=40 undec=0
PROBE SUMMARY frames=.. undecodable=0 transitions=.. shm_events=..
```

`BEAT` 是「探针活着但没流量」与「探针死了」的分界；`undecodable` 应恒 0。

## 一键场景

```bash
tools/sercli_live_probe/run_live_auto_shm.sh [--count N] [--period-ms M] [--hold-ms H] \
                                             [--with-topic-cat 0|1] [--via direct|factory] [--force-no-evidence]
```

启动顺序刻意是 **探针 → topic_cat → server → client**：探针必须早于任何一端才拍得到
`Unknown` 起始态；client 带 `--hold-ms` 不立刻退出，否则它一退握手通道就静了，
「切后仍可观测」无从谈起。每次跑用**唯一 topic**，避免上一轮的池条目/控制面段
变成下一轮的「占用」证据。日志落在 `build/live_runs/<ts>_<pid>/`。
