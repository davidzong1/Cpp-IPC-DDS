# S4 独立验收（第 2 轮）· W04 —— 针对 t60 收口的增量复核

> 复核者：**测量统计负责人**（W03；**未参与 W04 实现**，亦未参与 t60 修复）。
> 被复核对象：`t60`（repair round 2，实现者 = 共享层负责人）。
> 上一轮：`团队改造交付/R1/S4验收_W04.md`（t57，verdict = needs_revision，8 条 findings）。
> 本轮任务：`t61`，attempt `6daa089b-f3a3-4417-9f96-ade76f60b7a6`。日期：2026-10-01。
> 纪律：**只读复核**；本报告是唯一新增产物。

## 0. Verdict（第 2 轮）

```text
W04 第 2 轮独立验收：verdict = needs_revision
```

**性质**：**低级别收口**——第 1 轮的 **8 条 findings 全部核实闭合**，契约的实质技术内容（状态机 / 锁顺序 /
崩溃反例 / 覆盖判定）**独立复算一致**；本轮**仅新增 2 条 doc-accuracy 级 finding**（一个计数标签错配、
一个计数名不存在同名符号），均为**文字级**、不影响任何技术结论。
⇒ 不建议重开完整修复轮；若队长判断这 2 条属形式问题，**可折入 W12 跨文档一致性收口**（与 `udp.h` 行号、
UF-06 那批同批处理），不必再走一次 W04 repair。**在此之前 W04 维持 S3。**

---

## 1. 第 1 轮 8 条 findings 的逐条闭合核实（⛔ 我独立复算，不采信自述）

| ID | 声明闭合方式 | 我的独立核实 | 判定 |
|---|---|---|---|
| **F1**（blocker）条数与 ctest 项数 | §0 改为「**9 条**（L1–L9）+ ctest **18/18**（**19:47 快照时点**；20:07 复取 22/22）」+ 新增【计数沿革更正】 | `git show HEAD:test/test_lifecycle_contract.cpp \| grep -c '^TEST('` = **9** ⇒ 「8 条」确是**少算一条**（我独立复算一致）；工作区现 **10** 条（L10）。§0:27 / §2:84 / §9:464 / §11.2:575 四处数字自洽，且都标注了时点 | ✅ **闭合** |
| **F2**（medium）§3 行号 | 换成 682 / 713 / 795 / 810-811 / 893-894 / 955 / 973-983，并点明原文 `704/735/817/832/915` 在 `e800ccc` 树**全不是** RouteSession 调用点 | 我逐行 `git show e800ccc:…` 复核 **10/10 全部命中**（682 `stop_and_wake`、713 `wait_quiescent`、795 `begin_rebuild`、810/811、893/894、955 `acquire_receive`、973/983 `release_receive`）。另核其**新加的当前树漂移值**（1451/1467/1485/1572/1747/1771/2114/2141/2144）**亦全部命中**工作区 `route_session_` 调用 | ✅ **闭合** |
| **F3**（low）接入现状时点 | 表标题改「**W04 复审时点快照**（HEAD `e800ccc`）」+ 行尾「复审时点结论」；紧跟新增 **t60 补测表**；写明「⛔ 两者不得混读」 | 表头/行尾标注到位；补测表结论（已接入 ✅）**正确**（见下 N-1 的计数标签问题） | ✅ **闭合**（计数细节见 N-1） |
| **F4**（medium）归属 | §1:57 与 §10:524/526/535 改为 **W05 = socket与数据面负责人（收口）、W06 = 接收池负责人** | 与 team status 的 t6/t7 assignee 一致；§1:57 还注明「原『SHM接入负责人』线已由接收池负责人接替」 | ✅ **闭合** |
| **F5**（medium）§0 与 §11.3 矛盾 | §0:31 加粗「**现状 = F1 已收敛（D-11）/ F3 已修；仅 F2 的重建顺序仍为 W05/W06 准入条件**（与 §11.3 一致）」；F1/F3 条目重写为 ✅ | §0:31 与 §11.3 表（F1 ✅已收敛 / F2 high 仍为唯一未收敛 / F3 ✅已修 / F8 ✅后续已接入）**口径一致**，不再自相矛盾 | ✅ **闭合** |
| **F6**（medium）R-01 无常驻用例 | 新增 **L10** `LifecycleContract.GenerationRebuildRequiresRemoveRouteFirstOrCrashStall`：父进程安全顺序（`add_route==ok` + **重建后收到 2 条**）；子进程不安全顺序，判据取「**SIGSEGV 或停收**」，把「静默正常工作」显式判 FAIL；§9 R-01 改「常驻 L10 + 复现件」 | 见 §2 的**三项独立验证**（存在性 / 有效性 / **负控有牙**）——全部通过 | ✅ **闭合** |
| **F7**（low）8 条三处 | 三处统一（并注明 t60 后为 10 条） | `grep "8 条"` 的 3 处命中**全部落在「沿革更正」语境内**（说明「原文写 8 条是旧值」），不是残留错误 | ✅ **闭合** |
| **F8**（low）时点/指纹 | §11.1 加「**时点 / 指纹**」列，逐行标时间+sha256；新增 2 行 t60 补测；附「时点不可混用」声明；另更正 CMake 追加块 19→**20 行** | 表列齐备且每行有指纹；CMake 块核对 `awk 'NR>=401 && NR<=420' test/CMakeLists.txt \| grep -vc '^$'` = **20** ⇒ 更正正确 | ✅ **闭合** |

**⇒ 第 1 轮 findings 闭合率 8/8。**

---

## 2. F6 的独立验证（本轮最重要的一项）

### 2.1 存在性与有效性：L10 真的在跑、真的断言

```bash
./build/bin/test_lifecycle_contract --gtest_list_tests | tail -3
#   RouteKeyMustNotCarryGenerationBecauseAffinityIsPureFunction
#   GenerationRebuildRequiresRemoveRouteFirstOrCrashStall     ← 存在
./build/bin/test_lifecycle_contract          # 10 tests / 10 PASSED
ctest --test-dir build -R test_lifecycle_contract --output-on-failure | tail -3   # 1/1 Passed, 3.66 s
ctest --test-dir build -j4 --output-on-failure | tail -3                          # 29/29 Passed, 56.0 s
# 单跑到 L10 的 5 轮稳定性：
for i in 1..5: ./build/bin/test_lifecycle_contract --gtest_filter='*GenerationRebuild*'
#   → 5/5 OK（每次 204 ms）
```
t60 记录的属性值我也在**其原始输出**里核对到：`safe_order_received_after_rebuild=2`、`unsafe_order_signal=11`（`W04/证据/w04_r01_l10_negative_control.out.txt`）。

### 2.2 **负控有牙**（我不采信自述，自己注入了一遍）

把 L10 子进程路径里的**不安全顺序改成安全顺序**（插入 `w.remove_route(...)` + `w.add_route(...)`），重编后跑：

```text
不安全顺序（未先 remove_route 就 rebuild）**静默地正常工作**了 —— 这正是 W04-F2 要禁止的形态
[  FAILED  ] LifecycleContract.GenerationRebuildRequiresRemoveRouteFirstOrCrashStall (304 ms)
```

⇒ **判据确实会因"危险态不再可观测"而转红**，不是一条只会绿灯的摆设。还原后复绿。

**⚠️ 必须如实披露的操作**（边界纪律）：该负控是**就地注入**的，我随后**已逐字还原**，并给出三重佐证：

| 佐证 | 值 |
|---|---|
| 注入标记残留 | `grep -c NEGCTL test/test_lifecycle_contract.cpp` = **0** |
| 相对 HEAD 的改动量 | `git diff --numstat` = **+182 / −1**（与 t60 声明的改动量**一致**，即只剩 L10 与头注释） |
| 二进制指纹 | `build/bin/test_lifecycle_contract` = **`88e0a507…`**（与 t60 §11.1 声明的 `88e0a507…` **一致**） |

⇒ 复核结束时该文件的**内容与 t60 交付态逐字节相同**。

### 2.3 判据强度分层（t60 新增，我核对过口径）

§9 缺口 ⑥ 新增的强度分层表把「✔」细分为 **常驻用例 / 文档声明+机械核对 / 仅探针**，并明确
「⛔ 不得再用单个 ✔ 掩盖三者差异」。这正是第 1 轮 F6 要求的内容。据此我重算机械覆盖率（见 §3）。

---

## 3. 32 条竞态清单：第 2 轮机械统计

**判据**：与第 1 轮同法——该条被至少一个**常驻 CTest 项**覆盖（用例所在 target 在 `ctest -N` 内）；
命名用例若只在探针/一次性复现件里 ⇒ 记「仅文档声明 / 未验证」，**不计入已覆盖**。

| 三态 | 条数 | 占比 | 与第 1 轮对比 |
|---|---|---|---|
| **已由测试覆盖**（常驻 CTest） | **30** | **93.8%** | ↑（第 1 轮 29）：**R-01 经 L10 转入常驻** |
| **文档声明 + 机械核对**（无常驻用例，但判据可机械执行） | **1**（R-02） | 3.1% | 性质由「未验证」细化为「可机械核对」 |
| **仅探针**（进不了自动回路） | **1**（R-30） | 3.1% | 保持不变，**已如实标注** |
| missing（引用名不存在） | **0** | — | — |
| 分母 | **32**（`grep -c '^| R-'` = 32） | — | 一致 |

**逐条三态（仅列与第 1 轮判定不同的三条 + 其余汇总）**

| # | 三态 | 依据 |
|---|---|---|
| **R-01** | ✅ **已由测试覆盖**（**本轮升级**） | `LifecycleContract.GenerationRebuildRequiresRemoveRouteFirstOrCrashStall`（L10，常驻）；另保留一次性复现件（t60 复跑 mode1 **20/20 rc=139**、mode9 **5/5 rc=0**，我在 §4 独立复跑一致） |
| **R-02** | ⚠️ **文档声明 + 机械核对**（**不是**常驻用例） | 判据 = `grep -c "this_thread::get_id" <file>` = **3/3**（我复核 `recv_worker.cc` 与 `socket_recv_worker.cc` **均为 3**）；§9 已显式标注「无独立常驻用例」 |
| **R-30** | ⛔ **仅探针** | 只有 `artifacts/perf/20260929-r24-W06/probe/w06_probe_forkarm`（W06 交付，非 CTest）；§9 已标注「本包没有自己的常驻用例」 |
| R-03～R-17、R-32 | ✅ 常驻 CTest | `test_recv_worker` / `test_lifecycle_contract`（含 L1–L10）/ `test_socket_recv_worker` 等 |
| R-18～R-27 | ✅ 常驻 CTest | `test_recv_worker` / `test_socket_wait_set` / `test_socket_readable` / `test_recv_wait_set` / `test_shm_route_session` / `test_shm_i5_pop_buffer` |
| R-28、R-29、R-31 | ✅ 常驻 CTest | `test_wakeup_artifact`（10 条）/ `test_shm_control_scheduler`（20 条）/ `test_socket_ser_concurrency`（3 条）——三者因通配/文件名写法需人工归位（第 1 轮已说明） |

---

## 4. 四组核心复核的**重跑结论**（确认 t60 未破坏第 1 轮已成立的部分）

| 组 | 第 2 轮复算 | 结论 |
|---|---|---|
| **① 状态转换**（注册/注销/关闭/generation/控制 tick/lease） | 重跑关键取证：`git show e800ccc:src/dzIPC/threepools/recv_worker.cc \| grep -c release_recv()` = **5**（四处非 ok 出口 + remove_route 第 6 步）；`shm_route_session.cc:45-120` 的锁/`disconnect`/`wait`/`release` 结构计数 **8** 且与第 1 轮逐行一致（该文件本波**零改动**） | ✅ **语义仍逐条一致**，t60 未触碰源码 |
| **② 锁顺序 L-1..L-5** | 重跑机械扫描：`recv_worker.cc` 全部 `lock(impl_->mtx)` 作用域内出现 `join_mtx`/`ensure_thread_alive` 的次数 = **0**（与第 1 轮相同） | ✅ **L-1/L-2/L-3 仍成立** |
| **③ 32 条竞态三态** | 见 §3：**30 / 1 / 1**，分母 32 | ✅ 表更准确（R-01 升级为常驻） |
| **④ §10 准入可判定性** | W05-1（头文件成员 grep=0）、W05-3、W05-4、**W05-5/W06-4（重建顺序）**、W05-6 仍可机械判定；**W06-6 仍有计数名问题** ⇒ 见 N-2 | ⚠️ 基本可判定，1 项待修 |

**正向独立复跑**（不采信 t60 日志）：`./build/w04/w04repro_r60 1` × 6 ⇒ **6/6 rc=139**；
`… 9` × 3 ⇒ **3/3 rc=0** 且 `REPRO[mode=9]: survived; received=2`。与 t60 的 20/20、5/5 同向。

---

## 5. 本轮新增 findings（2 条，均为 doc-accuracy 级）

### N-1（medium，doc-accuracy）：t60 补测表的 `ShmControlScheduler::instance()` 计数标签错配

- **声明**（§3:118 与 §11.3 W04-F8 行两处）：「`ShmControlScheduler::instance()` **13 处**（含 `:884`、`:895`）」。
- **实测**（`src/dzIPC/shm_pub_sub_ipc.cc`）：
  ```
  grep -o "ShmControlScheduler::instance()" src/dzIPC/shm_pub_sub_ipc.cc | wc -l   → 5
  grep -v '^\s*\*' … | grep -c "ShmControlScheduler::instance()"                   → 4（排除注释行）
  grep -o "ShmControlScheduler" … | wc -l                                          → 13   ← 「13」属于这个更宽的模式
  ```
  ⇒ **13 是 `ShmControlScheduler`（无 `::instance()`）的出现次数**，被贴到了 `::instance()` 上。
  同表另外两个数我核对**正确**：`SubRecvRoute` = **7**、`RecvWorkerPool::instance()` = **3**（其点名的 `:560`/`:1702`/`:1825` 也逐行命中）。
- **影响**：**不影响结论**（`ShmControlScheduler::instance()` 实为 5 处，仍 **> 0** ⇒ 「✅ 已接入」成立，
  我另外确认 `:884` `worker_active()`、`:895` 注册项构造、`:2234`/`:2242` 两处均在位）。
  但这是第 1 轮 **F2（行号错配）的同类形态**——计数标签与实际模式不一致，复现者按字面 grep 会得到不同数。
- **requiredFix**：把两处的「13 处」改为「**5 处**」（`ShmControlScheduler::instance()`），
  或改写为「`ShmControlScheduler` 13 处（其中 `::instance()` 调用 5 处）」。

### N-2（low→medium，可判定性）：§10 W06-6 的 `fallback_activated` 无同名符号

- **声明**（§10:542 与 §8.4:442）：「记录 `fallback_activated`、`wait_set_full`、`wait_token_invalid` 三个独立计数，并证明目标规模下均为 0」。
- **实测**：`CounterId::fallback_activated` **不存在**（`counters.h` 中仅出现在注释里）；
  实际符号为 `fallback_total`（`src/` 命中 **20**）、`fallback_capacity_full`（4）、`fallback_backend_unavailable`（5）；
  `wait_set_full`（19）、`wait_token_invalid`（5）存在。
- **影响**：§10 是「可直接抄进各自回报」的准入口径，**按字面 grep 会 0 命中** ⇒ 读方可能误判「未接线」。
  缓解事实（不消除问题）：`test/perf/w10/w10_matrix.cpp:1812-1826,1855` 已显式处理别名
  （打印 `fallback_total(=fallback_activated)`），W10 侧不会被绊倒；W04 文档本身没有这层提示。
- **requiredFix**：§10:542 与 §8.4:442 改为实际符号（建议「`fallback_total`（=方案 §13.3 的 `fallback_activated`）」），
  并给可机械判定的命令与读数（例：`grep -rn 'CounterId::fallback_total' src/ | wc -l` = 3 处写入点）。
  ⚠️ 该条我在第 1 轮已列为 **P1**，但**未落入 t60 的 inScope**（t60 只列了 18/18、22/22、行号、W05/W06、R-02/R-30 等片段）⇒ 本轮虽闭合 8/8，这一项**仍开着**。

---

## 6. 逐节判定门（第 2 轮）

| 判定门 | 第 2 轮结论 |
|---|---|
| (1) 复用既有组件、不重设计 | ✅ 成立（未变） |
| (2) 状态转换与锁顺序固化 | ✅ **语义成立**；行号（F2）已修正并经我逐行复核 |
| (3) 六种运行态行为 | ✅ 成立（未变） |
| (4) 容量偏斜/边界 | ✅ 成立（L1 直方图 min=28/max=34、126/127 ok、128 `wait_set_full`、128/128 同 worker 均可复读） |
| (5) 首次初始化责任方 | ✅ 成立（`pool.stop` 全 `src/` 命中 0；L8 常驻） |
| (6) 禁止运行中切换后端 | ✅ 成立（进程级三态缓存） |
| 验收：非实现者独立评审 | ⚠️ 本轮结论 **needs_revision**，但**仅剩 2 条文字级 finding**；技术面已可判「通过」 |

---

## 7. 该套件「不能支撑」的结论（第 2 轮更新，仍须随结论一起报告）

1. **不能支撑「不安全顺序必然 SIGSEGV」**：L10 的判据是「SIGSEGV **或**停收」两种可观测失败
   （t60 明确说明理由：崩溃是实现相关的时序后果）。⇒ 若未来实现变成"稳定停收而不崩"，
   用例**仍会绿**——这是**有意**的设计，但它意味着**不能用 L10 反推"必然崩溃"**。
2. **不能支撑 R-02 的行为**：同线程 `remove_route`（Release 下等满 2000 ms + 诊断）**无常驻用例**，
   只有 `grep` 判据 + 一次性探针。
3. **不能支撑 R-30（fork pid 闸）**：仅 W06 探针，进不了自动回路。
4. **不能支撑千路规模**：本套件最大在册 route = **128**（构造性偏斜）；1000 次哈希调用不等于 1000 条 route 收发（属 W10）。
5. **不能支撑跨进程**：全套件除 L10 的 `fork()` 子进程外均为单进程；跨地址空间唤醒在另一套件。
6. **不能支撑真 SHM 多片大消息**：R-14/R-12/R-13 用 stub route/sender；L5 用闸门等价构造（文档缺口 ④ 已登记）。
7. **不能支撑 Release 下 I4 的「多一次 release」**：`assert` 被 `NDEBUG` 编译掉（W04-F6 已登记）。
8. **不能支撑 Windows**（缺口 ⑤）、**不能支撑混合进程池实例**（缺口 ①）。
9. **不能替代 W00 门槛 3 的空闲 CPU 证据**：L4/L4b/L5 是**上界/确定性**判据（36 次/s、24 次/s 等），
   不是性能测量。
10. **本轮的复核强度边界**：L-4/L-5 仍为**抽查**；32 条三态仍是**静态判定**（用例存在且在 CTest），
    **未逐条实跑**——若某用例名对但断言被改弱，我的方法看不出来。建议 W10 补一次「按 §9 表逐条跑并留原始输出」。

---

## 8. 边界、独立性与自证

- ✅ **独立性**：W04 实现者 = 共享层负责人；t60 修复者 = 共享层负责人；我均未参与 ⇒ 独立性成立。
- ⛔ **未改** W04 交付物 / 产品代码 / 工装 / 他人 WP / 既有 run：本报告是唯一新增产物。
  - 唯一例外且已完全还原：为验证 F6 的「负控有牙」，我**就地**把 L10 子进程路径改成安全顺序跑了 1 次，
    随后逐字还原；三重佐证见 §2.2（`NEGCTL` 残留 0、`+182/−1` 与 t60 一致、二进制 **`88e0a507…`** 与 t60 声明一致）。
  - 复核后重跑：`ctest -j4` = **29/29 Passed**、`test_lifecycle_contract` = **10/10 Passed**。
- ⚠️ 被复核文档跨时点（`e800ccc` 复审时点 / `dcaa0d9`+`f0ebc3ef…` t60 补测）：本报告凡涉"当前状态"
  均带时点，凡涉契约语义一律锚定 `e800ccc` / 工作区实测并注明。

```text
W04 第 2 轮独立评审：verdict = needs_revision（低级别收口）
第 1 轮 findings 闭合核实：F1 ✅  F2 ✅  F3 ✅  F4 ✅  F5 ✅  F6 ✅（含负控独立验证）  F7 ✅  F8 ✅   → 8/8
反证覆盖：有效订阅 ☑  预期路径 ☑  故障触发 ☑（R-01 反例 + L10 负控均由我独立复跑）  统计口径 ☐（静态判定，未逐条实跑）
本轮新增：N-1（medium，计数标签 13→5）  N-2（low→medium，fallback_activated 无同名符号，第 1 轮 P1 未落 t60 范围）
建议：若队长判定 N-1/N-2 属形式问题 ⇒ 折入 W12 跨文档一致性收口，W04 即可升 S4；否则再走一轮最小修复
评审者（非实现者）：测量统计负责人        日期：2026-10-01
```
