# UDP/SHM 后续修复任务清单

| 编号 | 任务与负责人 | 优先级 | 依赖 | 验收标准 | 验证/交付物 |
|---|---|---|---|---|---|
| F1 | ser-cli topic 前导 `/` 兼容迁移设计与实现（coder-claude；架构评审-claude 复核） | 高 | 现有 `name_operator` 段名规则、跨版本样本 | **完成**：POSIX 仅将 `/`→`_`，合法 topic 字节不变；斜杠 E2E 与别名碰撞保护通过；旧 `/` 名历史上无可迁移段，回滚为 revert | 设计说明、代码、M1/M2/M3 回归与活体证据 |
| F2 | SHM 切换失败 wire 标签与计数修复（coder-claude） | 高 | F1 失败路径保持可注入 | **完成**：新增 wire 5..8，分别表示占用、建腿失败、会合超时、运行期断链；失败路径各计一次且同连接不重复计数；旧值 4 不再猜测为占用 | `test_sercli_auto_path` 15/15，wire 布局与工具解码回归通过 |
| F3 | UDP 内核缓冲运维调整与长包验证（性能测量-claude；测试验证-claude） | 中高 | 运维批准变更窗口；F1/F2 不阻塞 | **完成首轮**：`rmem_max/wmem_max=512MiB`；64KiB/256KiB/1MiB 的 SHM 与 socket 共 6 场景消息丢失均为 0，socket 分片缺失为 0 | `build/f3_perf_512mb/results.{csv,json}` 与硬件快照；长稳压测另行执行 |
| F4 | sniffer `open_or_create` 契约文档与测试同步（writer-claude） | 低（约束） | 架构评审既有裁定 | **完成**：公共 libipc 默认语义不变；文档明确保留原因与工具侧 open-only 探测纪律；未引入产品 API 变更 | 文档补丁、契约回归检查 |

执行顺序：先 F1 设计评审，再并行实施 F1/F2；F3 需运维窗口后执行；F4 可独立收口。任何改动不得修改公共 `libipc` 默认 `open_or_create` 语义，且 F1 必须提供可回滚路径。

当前状态（2026-09-16）：F1/F2 已实现，leader 实跑 `test_sercli_auto_path` 15/15；wire/工具/控制面/UDP 边界关键回归 5 套件 33/33 通过。F3 在 512MiB 内核上限下完成 6 个长包场景首轮验证；F4 文档同步完成。公共 `libipc` 默认语义未改。

用户拍板（2026-09-16）：F2 允许扩展 wire 信号枚举以区分占用、建腿失败和运行时断链；F3 已确认 `rmem_max/wmem_max=512MiB`，可直接执行长包验证。F1 追加 M3/A11 别名碰撞回归与 verbose 诊断日志，作为最终收口门。

F3 首轮实测（leader，2026-09-16）：`net.core.rmem_max = net.core.wmem_max = 536870912`。`dzipc_perf_benchmark --payloads=65536,262144,1048576 --duration=2 --warmup=0.2 --cases=pubsub_shm,pubsub_socket --skip-tput --frag-stats` 共 6 用例均判定通过；SHM 与 socket 在 64KiB/256KiB/1MiB 的消息丢包均为 0%，socket 分片缺失均为 0。结果见 `build/f3_perf_512mb/results.{csv,json}`；1MiB socket 有一次发布失败计数（`failed=1`），但接收数 217、发送成功数 216，最终 `lost=0`、分片缺失 0，需在长跑/更高负载复测中单独关注。该轮没有把内核参数回退到 212992 做同会话 A/B，因此只证明 512MiB 配置下结果，不宣称相对旧配置的定量提升。

**设计快照（writer-claude，2026-09-16）**：§A 记录 F1/F2/F3 验收边界，§B 记录 F4 契约与工具侧 open-only 探测纪律，§C 记录当时的只读复核。后续执行结果以本文开头的状态表为准；设计快照中的“未运行”不再代表最终状态。

## A · F1/F2/F3 验收边界

### A1 · F1 冻结设计（2026-09-16 三轮讨论收敛）

- **规则 = 平台谓词**：POSIX 上把段名里**每一个** `/` 字节改写为 `_`；Windows 保持原样（Windows 禁集为**空集**，系代码推导、**未在 Windows 实测**）。**不截断、不哈希**（哈希会让名字需在 ≥4 处逐字节一致，任一处不同即按 `create|open` 静默建空壳段，属新增静默失败面）。
- **唯一改点**：`shm_service_prefix`（`src/dzIPC/common/name_operator.cc:31`）—— 单点覆盖 `_ser_r`/`_ser_w`/`_ser_control2` + 占用判定 + `topic_cat` + sniffer ⇒ **libipc 零改动**（同时满足 F4 约束）。
- ⛔ **题面里的"旧段名发现/迁移"是空集**：含 `/` 的 topic **从来没有段** —— `src/dzIPC/shm_ser_cli_ipc.cc:275-278` 控制面 open 失败即 `throw`，位置**先于** `:280-281` 的两次 `clear_storage`，破坏性路径根本到不了；POSIX `shm_open` 必 `EINVAL`。⇒ **无迁移、无需清理、无需发现**。"旧段名"只存在于合法 topic 上，而那些名字**逐字节不变**。
- **守卫已存在，非待新增**：`src/dzIPC/auto_ser_cli_ipc.cc:303` 的 `shm_channel_occupied()` 是 `InitChannel()`（`:324`）的**唯一闸门**；其池匹配（`:100`）按**原始 topic 串**、对别名盲，但 `:121-128` 以**派生段名**（`ser_service_control_name`）做第二证据源 ⇒ 别名等价的两个 topic 撞同一控制面段名时被判**占用**，停在 socket、不触碰在跑连接。本轮只需补 M3 用例与 `:303-310` 的 verbose 诊断日志（同打印**原始 topic + 派生段名**，否则别名碰撞不可诊断）。
- **跨版本**：合法名新旧全互通；`/` 名新↔旧 = 旧侧退回 socket（= 今天行为，安全）；新↔新 = 起 SHM。**无 wire 变更**。
- **回滚**：revert 改名 commit（`/` 名回到 socket 退避，合法名不受影响）；无状态迁移、无残留段 ⇒ 即时安全。

### A2 · F1 验收**不阻塞** F2

F1 的承重断言一律取**服务端**指纹（`kind=Shm`、`switch_successes`、段是否存在），**不使用**客户端 `FINAL kind` / 原因标签。理由见 A3：客户端标签在 F2 修复前不可信。

### A3 · F2 的范围 = 客户端可观测性

根因：`src/dzIPC/auto_ser_cli_ipc.cc:244-265` 的 `withdraw_to_socket()` 对**任何**原因都发 sig 4，而客户端把**任何** 4 都读成"对端拒绝/占用"（`:660-683`）⇒ "对端建腿失败"被记成 `ChannelOccupied` / `ShmChannelOccupied`，且 `switch_attempts`/`switch_fallbacks` **都不递增**。
现网证据：`build/live_runs/20260915_232219_2580718`（带 `/` 的 topic）—— 两行 `shm_open[22]`、`FINAL kind=Socket`、`shm_events=0`，业务仍 8/8 ⇒ 功能退避安全，但**原因标签错**。

### A4 · F3 独立

F3 与 F1/F2 **互不阻塞**，需运维变更窗口。起点基线：实测 `net.core.rmem_max`/`wmem_max` = `212992`（见 `docs/local_shm_fanout_technical_route.md` §7）。

### A5 · F1 测试矩阵（承重三项 + 回归）

- **M1** 无 `/` 的 topic 段名逐字节不变（新二进制直接复用旧段、老数据可读）。
- **M2** `/topic` 起 SHM，用**服务端**指纹断言：`kind=Shm`、段运行中存在、**退出后零残留**、`ok==cb`、`dup_delivery=0`、`topic_cat` 报 `Transport changed to SHM`。
- **M3（别名不摧毁）** 先起 `_foo` 建 SHM 活连接，再起 `/foo`：断言 `_foo` 的 pid/refcount/业务**未被摧毁**、`/foo` 停 socket 且**服务端**记 ChannelOccupied。
  ⛔ **必须带变异条款**：改名**前**跑 M3 是**空过（假绿）**——改名前的 `/foo` 在 `:275-278` 就 `throw`，且改名前的派生名对 topic 串是**单射**（无清洗）⇒ 别名对**根本不存在**，用例过的是"新人惰性"而非"守卫生效"。⇒ 必须**关掉 `:121-128` 的派生命探测后 M3 转红**，才可锁为常驻回归。
- 回归：**M4**（无 `/` 旧名不因改名重建段）、**M5/M6**（混版本：合法名 / `/` 名）、**M8**（`git diff --stat` 不含 `src/libipc/**`）、**M9**（既有 ser/cli 套件）、**M10**（池 entry 的 `topic_name` == 用户原串的不变量，含 `/`）。

## B · F4 契约：公共 libipc 默认 `open_or_create` **不得变更**

依据：共享上下文区 `sniffer_open_only_架构评审裁定.md`（只读审计，未改任何文件）。

### B1 · 冻结项与理由（三条独立，缺一不足以定论）

1. **是文档化契约**：`include/libipc/sniffer.h:64-67` 原文 —— *"The default mode opens-or-creates the underlying SHM, exactly like a normal ipc::chan would, so calling open() before the publisher is fine."*；`include/libipc/shm.h:16` 的默认参数即 `create | open`。
2. **结构上承重**：publisher 侧 `src/libipc/queue.h:33-46` 用**同一模式**且 `Elems` 与 sniffer 的 `elems_t` 同类型同布局，双方 `init()` 都是 DCLP 幂等 ⇒ 去掉"创建" = 去掉"sniffer 先于 publisher"这条**能力**，**不是修 bug**。
3. **调用侧正依赖 + 公共 API**：`exec/dzipc_topic_cat/src/shm_sniffer.cc:76-79` 的 `open_channels()` 仍**无条件**调用、失败即 `std::exit(1)`（改 open-only 会让 `topic_cat` 在"先起 echo、后起 talker"的标准用法下**直接退出**）；`ipc::sniffer` 已导出到 Python（`python/src/interface.cc:476-490`，使用者 `tools/dzplot/main.py:955`/`:1027`）⇒ 加模式参数必须同时决定 pybind 口径，否则 C++/Python 行为分叉。

### B2 · 工具侧 open-only 探测纪律（要探测，就这么写）

- libipc **已有** open-only 语义，**不需要新造**：`include/libipc/shm.h:13-16` `create = 0x01, open = 0x02`；`src/libipc/platform/posix/shm_posix.cpp` 在 `mode == open` 时**不加** `O_CREAT` 且把 size 归零 ⇒ 段不存在时返回 `nullptr`。
- **固定手法**（两处既有同构先例：`src/dzIPC/common/control_plane.cc` 的 `occupied_by_other`、`exec/dzipc_topic_cat/include/control_plane_naming.h` 的 `control_plane_segment_exists`）：
  **free function `ipc::shm::acquire(name, 0, open)` → `get_mem` → `release_no_unlink`**。
- ⛔ **绝不能用 `release()`** —— 它在引用计数归零时会 `shm_unlink`，等于"探测即删段"。
- ⛔ **实现陷阱**：`handle::acquire` 拒绝 `size == 0`（`src/libipc/shm.cpp:76`）⇒ 探测**只能**走 free function；`handle` 路径要 open-only 必须传真实 size。
- **判据取向**：断言 `/dev/shm` 里**出现了哪些名字**，不要断言"函数返回了什么"。

### B3 · ⛔ 已否决：库层"自己建的自己删"

把存储泄漏换成**静默分裂**：引用计数只在 `get_mem` 时 +1，排除不了"对端已 `shm_open` 拿到 fd、尚未 mmap"的窗口；此时 unlink，对端手上 fd 仍指向旧 inode（它自己还能跑），而**之后**启动的第二个对端会新建另一个段 ⇒ 双方都"成功"却**静默互不通**。与本项目端口修复里"新旧进程绑不同端口"属同类失败形态。**不得**按此实现。

### B4 · 残留射程订正（推翻"只有无发布端才有残留"）

真正的机制是 **sniffer 永不 unlink**（`src/libipc/sniffer.cpp` 析构走 `release_no_unlink()`，注释明写 *"must never delete the publisher's SHM segment"*）⇒ **任何"工具比发布端活得久"的运行都会永久留下该段**（dzplot / `topic_cat` 的常见用法），"无发布端"只是其中最显眼的一例。体积约 20KB/通道（256 槽 × 约 80B），ser/cli 是 req+res **两条**。⇒ **"没发布端就别 open"这类调用侧门控只能修掉一个子集**，不足以收口；按此排工才会漏。

### B5 · 验收用例（若要收口，按此立用例；A1/A2 承重）

- **A1（承重）** 无发布端起 `topic_cat`（pub/sub，显式 shm）⇒ 运行中 `/dev/shm` **不出现** `QU_CONN__<seg>__64__8`；**退出后再测一次**（防"运行中建、退出时删"的假通过）。
- **A2（反向对照）** 拿掉门控（变异回旧代码）⇒ A1 **必须立刻红**；不红则 A1 无判据力。
- **A3（契约回归，必须保留）** `ipc::sniffer::open()` 在 publisher **之前**调用仍返回 `true`，且 publisher 起来后能收到消息 —— 这条用例的存在本身就是"默认不许改"的证据。
- **A4（不误删）** 发布端先起、工具后起：工具退出后**发布端的段必须仍在**（`release_no_unlink` 语义未被破坏）。
- **A5（ser/cli）** req+res 两条（`_ser_r`/`_ser_w`）都**不得**被建。
- **A6（主支）** "工具比发布端活得久"单独立用例 —— 裁定指出**它才是主支**；若只做调用侧门控，A6 仍会红，必须显式记为"**已知未覆盖**"。

### B6 · ⚠️ HEAD 现状（2026-09-16 复核 —— 勿按裁定快照排工）

- 工具侧探测**已落地**：`shm_sniffer.cc:57-75` 已用 `control_plane_segment_exists()` 门控**控制面**；段不存在时只打印提示并放弃"发布端重建跟踪"（**不建**控制面段）。
- ⛔ **但数据通道侧仍未收口**：`open_channels()` 依旧**无条件**调用（`:76-79`）⇒ 无发布端时 sniffer 仍按默认 `create|open` **建出** `QU_CONN` 段 ⇒ **A1 在 HEAD 尚未通过**，A6 同样未覆盖。（裁定 F6 记的是 `:74`，行号已随门控落地下移。）
- 本节**只做文档同步，不据此改码**。

## C · 复核方式与剩余未实测项

- **锚点复核（已做，只读）**：`include/libipc/shm.h:13-16`（`create = 0x01, open = 0x02`；`:16` 默认参数 `create | open`）、`src/libipc/shm.cpp:76`（`size == 0` 拒绝）、`include/libipc/sniffer.h:64-67`（契约原文）、`src/libipc/sniffer.cpp`（析构 `release_no_unlink()` 及注释）、`exec/dzipc_topic_cat/src/shm_sniffer.cc:57-79`、`python/src/interface.cc:476-490`、`tools/dzplot/main.py:955`/`:1027` —— 与 §A/§B 引用的内容逐条一致。复核基准 = **HEAD `9d91212`**（复核时工作区对上述文件无改动，故行号与该提交一致）。
- ⚠️ **行号漂移（必读）**：截至本次同步，**F1/F2 正在 `src/dzIPC/auto_ser_cli_ipc.cc`、`src/dzIPC/common/name_operator.{h,cc}`、`test/test_sercli_auto_path.cpp` 上落码**（工作区已改动）⇒ §A 里这些文件的 `file:line` 在 F1/F2 落地后**必须重核**；引用时以 `git show 9d91212:<file>` 为基准，或落地后重取行号。libipc 侧锚点（§B）不受影响。
- **契约回归检查（已实测，2026-09-16）**：`git status --porcelain` 与 `git diff --stat` 中**不含** `src/libipc/**` 或 `include/libipc/**`（工作区改动仅 `M scripts/install.sh`，另有未跟踪的本文档）⇒ 公共 libipc 默认语义在当前工作区**未被改动**。
- **已实测**：F1 段名纯函数、斜杠 topic SHM E2E、别名碰撞不摧毁、F2 wire 编解码与失败计数、相关关键回归，以及 F3 的 6 个长包场景。
- ⛔ **剩余未实测项**：§B 的数据通道残留 A1–A6 尚未作为本轮范围执行；F1 的新旧二进制混版矩阵未做独立进程实测；Windows 禁集为空集仍为代码推导。B4 的残留机制仍需 A6 专项实测确认。
