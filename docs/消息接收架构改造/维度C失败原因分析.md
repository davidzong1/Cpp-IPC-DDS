# 维度 C 失败原因分析报告（t7 交叉 Review 后续）

> 起因：t7（attempt_id `3a83b019-be17-4f0e-9e86-08d2e2cbd060`）判定 **verdict=needs_revision**：维度 A（方案符合性）通过、维度 B（配置与硬编码）通过，唯**维度 C（线程安全与资源释放）不通过**。
> 定位：本报告只解释「为什么维度 C 会失败」（原因链 + 流程缺口 + 修复方向），**不重复 findings 清单**；findings 原文见 [交叉Review报告.md](交叉Review报告.md) §5。
> 性质：只读分析，未改产品代码。基线 `HEAD = 1d38512`；证据均为本次 `cmake -S . -B build` + 重编后可复现的结构性读数（A8 纪律）。

---

## 0. 结论摘要

维度 C 失败的**直接原因**是 10 条 findings 中有 8 条落在维度 C（C-01～C-08）。归类后只有 **3 个根因**，其余是同一根因在不同文件上的复制：

| 根因 | 覆盖 | 一句话 |
|---|---|---|
| **RC-1 共享状态指针的同步收尾缺口** | C-01、C-02、C-06、C-07、C-08 | 本次移植把并发边界从「每对象一把锁」扩到「worker 与处理线程共享的 state 对象」，**新增的 `receive_state_` 持有者指针成了第四条同步边界**，却没进入任何一份方案的收尾清单（t3 建了 `state_mtx_`，t4/t5 没建） |
| **RC-2 契约条款「注释声明」与「调用点落地」脱节** | C-03、C-04 | 两处都在注释里写了「启动前 claim / 退出后 release」，代码里却没有对应调用（t5 兼容线程 0 处 `compat_thread`；t3 回退路径 `req_route_` 为空导致 claim/release 被短路），t3 头注释还反向承诺「两条路径都会建好 `req_route_`」 |
| **RC-3 新中间层的资源上限未与下游容量联立** | C-05 | 形态 (b) 新增的「每 server 有界 FIFO」持有完整请求 buffer，容量（64 条 / 8 MiB）未与 SHM chunk 池档位（`large_msg_cache = 40`）联立论证，方案文档无取值依据 |

**流程层面的根因（为什么验收没能拦住）**：验收面对 `reset_message` / `reset_callback` / `restart_data_plane` 这三条并发入口**零覆盖**，工程内也**没有 TSAN/helgrind 等竞态检测器**；`ctest` 的 12 条用例全是功能/生命周期用例，结构性竞态在功能用例下必然为绿。⇒ 维度 C 的唯一防线是「人工逐访问点核对」，而三名作者各自独立核对时 t3 做了、t4/t5 漏了——这正是 RC-1 能存活到 Review 的机制。

---

## 1. 失败面与证据矩阵（可复现）

| ID | 严重级 | 位置 | 复现命令与读数 | 违反的明文约束 |
|---|---|---|---|---|
| C-01 | high | `src/dzIPC/socket_ser_cli_ipc.cc:397/412/842`（读）、`:952`（写）、`:1004-1005`（reset） | `grep -n "receive_state_" src/dzIPC/socket_ser_cli_ipc.cc` ⇒ 全部命中点**无一持锁**；对照 `grep -n "state_mtx_" src/dzIPC/shm_ser_cli_ipc.cc` ⇒ `781/803/909` | 对同一 `std::shared_ptr` 的并发读+写是数据竞争（C++ UB）；t3 已在 `include/dzIPC/shm_ser_cli_ipc.h:91-95` 写明「`ser_state_` 的线程安全读口…避免与 `reset_message/reset_callback` 的写入竞争」 |
| C-02 | medium | `src/dzIPC/socket_pub_sub_ipc.cc:957-965`（读）、`:1040`（写） | `grep -n "receive_state_" src/dzIPC/socket_pub_sub_ipc.cc` ⇒ `:1040` 的写入不在任何锁内（`:1031-1039` 的 `topic_msg_mtx_` 临界区已结束）；`grep -c "state_mtx_" include/dzIPC/socket_pub_sub_ipc.h src/dzIPC/socket_pub_sub_ipc.cc` ⇒ `0` | 同上（t4 未建读口） |
| C-03 | medium | `src/dzIPC/socket_ser_cli_ipc.cc:786-829` | `grep -n "compat_thread" src/dzIPC/socket_ser_cli_ipc.cc` ⇒ **0 命中**；对照 `grep -n "compat_thread" src/dzIPC/socket_pub_sub_ipc.cc src/dzIPC/shm_ser_cli_ipc.cc` ⇒ 各 1 处 | 契约 §4.6 / `recv_worker.h:151-158`：兼容线程启动前 `try_claim_recv(compat_thread)`、退出后 `release_recv()` |
| C-04 | medium | `src/dzIPC/shm_ser_cli_ipc.cc:810/812/818/824/834/839`（六处 `return fallback(...)`）vs `:841`（`req_route_ = route;`） | `grep -n "req_route_ =" src/dzIPC/shm_ser_cli_ipc.cc` ⇒ **仅 `:841`**，六处回退分支全在它之前；`sed -n "86,89p" include/dzIPC/shm_ser_cli_ipc.h` ⇒ 注释承诺「两条路径都会建好 `ser_state_`/`req_route_`」 | 同上 + 头注释与实际相反 |
| C-05 | medium | `src/dzIPC/shm_ser_cli_ipc.cc:109-110`（FIFO 上限）、`:243-248`（入队） | `grep -n "kMaxPendingBytes" docs/消息接收架构改造/shm_ser_cli线程池移植方案.md` ⇒ **0 命中**；池容量口径见 `include/libipc/def.h:49`（`large_msg_cache = 40`）与 `src/libipc/ipc.cpp:461` | 需求 §1.2 第二句 / §7：不得让 SHM chunk 脱离现有配额控制（未越界，但**新增持有窗口未量化**） |
| C-06 | low | `src/dzIPC/socket_pub_sub_ipc.cc:1161/1172/1176` | `sed -n "1156,1185p"` ⇒ `stopping=true` → `cancel_wait` → `running=false` → join | 方案 §5 的停机顺序收紧要求（工程一致性，非契约硬条款） |
| C-07 | low | `src/dzIPC/shm_ser_cli_ipc.cc:294`、`src/dzIPC/socket_pub_sub_ipc.cc:329`、`src/dzIPC/socket_ser_cli_ipc.cc:316` | 三处 `cv.wait_for(...)` 返回值全部丢弃；对照共享层 `src/dzIPC/threepools/recv_worker.cc:722-727` 会打诊断 | 契约 §4.1/§4.4 + 方案 §4 第 6 步：「超时只打诊断」 |
| C-08 | low | `src/dzIPC/shm_ser_cli_ipc.cc:676-679`、`src/dzIPC/socket_pub_sub_ipc.cc:1049-1052` | 两处 claim 失败分支直接 `return`，无日志 | 契约 §4.6 的意图（防「静默无收」） |

---

## 2. 根因分析（逐条 5 Whys）

### RC-1｜共享状态指针的同步收尾缺口（C-01 / C-02）

```text
Why1 为什么会有数据竞争？        —— 同一 shared_ptr 成员被不同线程无锁读写。
Why2 为什么没加锁？              —— 该成员的「读口」从未建立；作者只在紧邻处复用了 message_mtx_ /
                                    topic_msg_mtx_，而那两把锁保护的是 message_ / topic_msg_，
                                    不是 receive_state_ 本身。
Why3 为什么没人发现？            —— 三模块由三名成员分别独立实现。t3 建立了 pattern，但 t4/t5 拿到的
                                    是「方案文档 + 共享层头文件」，方案里没有「state 同步契约」这一条。
Why4 为什么方案没有这一条？      —— 方案模板是「从 shm_pub_sub 复用机制」，而 shm_pub_sub 的 SubState
                                    自带 mutex（sub_state_->topic_msg_mtx），于是「共享状态自带同步」被默认；
                                    但本次新增的 receive_state_ 是模块自己新建的**间接层**，它不在 SubState 模板里。
Why5 为什么这算范式迁移缺口？    —— 本次改造把「每 server 一线程 + 对象成员」改成「共享 worker + shared state」，
                                    新引入的间接层（state + 它的持有者指针）成了**第四条同步边界**，
                                    而收尾检查表只覆盖了前三条（对象成员锁 / 队列锁 / wait-set 归属）。
```

**关键佐证（同一设计意图未跨成员传播）**：

- t3 头注释已把这把锁的**理由**写清楚：`include/dzIPC/shm_ser_cli_ipc.h:91-95`「`ser_state_` 的线程安全读口（处理线程与注销路径都用它，避免与 `reset_message`/`reset_callback` 的写入竞争）」。
- t5 等价位置 `include/dzIPC/socket_ser_cli_ipc.h:114-122` 只写了 worker 归属，**没有**提到 `reset_message` 并发。
- 实测 `grep -c "state_mtx_" include/dzIPC/socket_ser_cli_ipc.h src/dzIPC/socket_ser_cli_ipc.cc` ⇒ `0`；`grep -c "state_mtx_" include/dzIPC/socket_pub_sub_ipc.h src/dzIPC/socket_pub_sub_ipc.cc` ⇒ `0`（t3 为 3 处）。

**可达性与后果**：`reset_message()` / `reset_callback()` 是 `ser_ipc_base` / `sub_ipc_base` 的虚接口（`include/dzIPC/ser_cli_base.h:19`、`include/dzIPC/pub_sub_base.h:22`），经 `pimpl::server_ipc_impl::reset_message`（`src/dzIPC/server_ipc.cc:169-171`）暴露给调用方；与 `restart_data_plane()`（`src/dzIPC/socket_ser_cli_ipc.cc:952` 的 `receive_state_ = state;` 就在其调用链上）**可来自不同线程**。后果是 UB——表现为偶发崩溃/引用计数错乱，而非可预测的功能错误。

### RC-2｜契约条款「注释声明」与「调用点落地」脱节（C-03 / C-04）

```text
Why1 为什么契约条款没生效？      —— 代码里根本没有那行调用（t5 兼容线程 0 处 compat_thread 调用）。
Why2 为什么写注释的人没写调用？  —— t5 的注释是从 t3/t4 抄来的措辞，抄了「要 claim/release」的语义，
                                    但 t5 的兼容线程函数体是**继承 HEAD 的旧实现**（HEAD 的 response_thread_func
                                    里没有 claim 概念），于是注释与函数体来自两个不同来源。
Why3 为什么 Review 前没暴露？    —— 契约条款的可判定性依赖「grep 调用点」，而方案接口清单（A6）本就漏了
                                    9 纯虚；兼容线程这一侧更没有任何清单。
Why4 t3 那一侧又为什么漏？        —— t3 建了 claim/release 调用，但挂在一个统一小工具上，
                                    或把它们挂在 req_route_ 上；而 req_route_ 只在 add_route 成功后赋值
                                    ⇒ **回退路径天生拿不到它**（六处 fallback 全在 :841 之前）。
Why5 共同的机制是什么？          —— 「同一份契约在两个来源里各写一半」：注释写了、调用点没写；
                                    调用写了、但被一个前置条件（回退时 req_route_ 为空）短路。
```

**影响评估（避免过度定性）**：两处的**实际双收/丢包风险均低**——

- t5：`InitChannel`/`restart_data_plane` 里两条路径由构造互斥（`start_receive_path()` 成功起 `process_thread_func`、失败起 `response_thread_func`，不会同时存在）；
- t3：回退时根本不 `add_route`，owner 恒为 `none`。

因此扣分点是「**契约条款未落地**」+「**注释与实现相反**」这两条可判定事实，而不是「已经发生双收」。

### RC-3｜新中间层的资源上限未与下游容量联立（C-05）

```text
Why1 为什么 FIFO 容量值得审？    —— 它持有的是完整请求的 ipc::buffer（SHM chunk 池资源），
                                    而改造前「取到即处理」不产生这个持有窗口。
Why2 为什么给了 64 条 / 8 MiB？  —— 与 socket 侧「每服务有界队列」对齐（kQueueCap=64）；
                                    8 MiB 来自「大请求 + 多客户端」的直觉余量。
Why3 为什么没有量化？            —— SHM chunk 池是**每尺寸档 40 块且全机共享**（include/libipc/def.h:49
                                    `large_msg_cache = 40`；src/libipc/ipc.cpp:461 的既有说明），
                                    论证「8 MiB 上限是否挤占 Publisher 的 DZFlat 借样」需要一次前后实测对照，
                                    超出模块作者的单测能力。
Why4 为什么方案模板没兜住？      —— 方案「必须保持的行为」里 chunk 那一条写的是「不引入第二套记账」，
                                    它被满足（确实没有新记账），但**没有写「新增持有窗口需给出上界依据」**。
```

**当前状态**：属「**缺证据**」而非「已确证有害」——背压路径零丢弃且 FIFO 有界，但没有前后对照读数。

### RC-4｜「能跑通」验收下不可见的诊断面缺口（C-06 / C-07 / C-08）

```text
Why1 为什么停机序/诊断面被漏？  —— 它们不影响功能正确性（用例全绿），只影响「出问题时能不能定位」。
Why2 为什么不算进验收？          —— 需求 §10 与三份方案的验收标准都以功能、线程数、CPU 为主，
                                    没有「注销超时必须留痕」这类可观测性条目。
Why3 为什么仍要登记？            —— wait_quiescent 超时意味着「有个 recv_once 跑了 2s 没回来」，
                                    静默继续会让现场只剩「偶发丢消息」这一个现象，无从归因。
```

---

## 3. 为什么流程没能提前拦住（维度 C 失败的系统性原因）

| # | 缺口 | 证据（本次实测） | 后果 |
|---|---|---|---|
| P1 | **验收面为空**：`reset_message` / `reset_callback` / `restart_data_plane` 三条并发入口在 `test/` 下零覆盖 | `grep -rl "reset_message" test/ | wc -l` ⇒ **0**；`grep -rl "reset_callback" test/ | wc -l` ⇒ **0**；`grep -rn "restart_data_plane|stop_data_plane" test/` ⇒ 仅 `test/test_topic_cat_select.cpp:4` 的一句注释 | C-01/C-02 的触发路径**永远走不到任何用例**；`ctest 12/12 Passed` 对它们**不可证伪** |
| P2 | **无竞态检测器** | `grep -rn "sanitize" CMakeLists.txt test/CMakeLists.txt src/CMakeLists.txt cmake/*.cmake` ⇒ 空 | 数据竞争只能靠人工读代码发现 |
| P3 | **Review 是第一道（也是唯一一道）防线** | t7 报告 §4.0 的核对方法全部是「grep 访问点 + 读控制流」 | 防线强度取决于审查者细致度；本次靠「t3 有、t4/t5 无」的**横向对照**才判出，属对照法而非判据法 |
| P4 | **范式迁移收尾清单缺一条**：三份方案的「必须保持的行为」都没有「新引入的对象/指针是第几条同步边界」 | 方案 §2 复用机制清单（三份）均无此条 | 同一设计意图靠口头传播，跨成员即丢失（RC-1） |
| P5 | **契约条款以注释形式存在时无落地校验** | C-03/C-04 的注释与调用点来自不同来源（必须同时读注释与函数体才能发现） | 「声称已做」可长期与「实际没做」共存（RC-2） |

> 补充：A8 纪律（重跑 cmake + 重编）保证的是**读数真实**，它不产生并发真值；因此「重编后 12/12 绿」**不能**用来反驳 C-01。

---

## 4. 修复方向（建议，供 t8 采用；不含实现）

| finding | 最小改动方向 | t9 的复核判据（可判定） |
|---|---|---|
| C-01 / C-02 | 两模块各加一把只保护 `receive_state_` 自身的 `mutable std::mutex`（t3 的 `state_mtx_` 即模板）；读/写/reset 全部走访问器；锁内不得再取 `state->mtx`（保持 `topic_msg_mtx_ → state->mtx` 单向锁序） | `grep -c "state_mtx_|state_lock_" src/dzIPC/socket_ser_cli_ipc.cc src/dzIPC/socket_pub_sub_ipc.cc` > 0，且 `grep -n "receive_state_"` 的每个命中点要么在锁内、要么在访问器内 |
| C-03 | `response_thread_func` 进入循环前 `try_claim_recv(RecvOwner::compat_thread)`（失败即 `return`），退出前 `release_recv()` | `grep -n "compat_thread" src/dzIPC/socket_ser_cli_ipc.cc` 命中（与 t3/t4 同形） |
| C-04 | 二选一：(a) claim/release 抽成不依赖 `req_route_` 的形态（直接在 `SerState` 上 CAS）并在回退路径调用；或 (b) 所有路径先建并保存 `SerRequestRoute`（兼容路径不 `add_route`）。同时修正 `include/dzIPC/shm_ser_cli_ipc.h:86-89` 注释 | `grep -n "req_route_ ="` 赋值点数量（选 b 应为 1）；或 claim/release 出现在 `req_route_` 之外（选 a）；头注释与实现一致 |
| C-05 | 方案补容量依据 + 与 chunk 池档位的关系；或降容量并给背压计数；若接受取舍须给前后 `DzFlatFallbackCount` 对照 | 文档出现 `kMaxPendingBytes` 的取值论证；或有前后对照读数 |
| C-06 | `running=false` 提前到 `stopping=true` 同处 | 顺序 grep 可验 |
| C-07 | 三处 `wait_for` 返回 false 时打显式诊断 | `grep -n -A6 "wait_quiescent" <三模块>` 命中 `ipc::error`/`std::cerr` |
| C-08 | 两处 claim 失败分支打日志 | 同上 |

---

## 5. 防复发建议（超出本波，交队长 / t11 决策）

1. **把「新引入的同步边界」写进方案模板**：任何「从 per-X 对象迁移到共享 state」的改造，方案必须列一张表：`新成员 | 谁读 | 谁写 | 由哪把锁保护 | 读口函数名`。三份方案当前都没有这张表（P4）。
2. **契约条款的落地校验机械化**：凡在注释里声明的契约条款（claim/release、唤醒、超时诊断），补一条「注释里出现了就必须有调用点」的 grep 清单，放进 t7/t9 检查表（P5）。
3. **补两条最小用例**（不需要 TSAN）：
   - `reset_message` / `reset_callback` 与收包并发跑 N 轮（当前触发路径为 0 覆盖，P1）；
   - `stop_data_plane()` → `restart_data_plane()` 的 churn（`test/test_topic_cat_select.cpp:4` 的注释已指出该组合是真实场景，但没有断言）。
4. **先探明竞态检测器可用性**：本机 `grep sanitize` 为空；若要长期守住这一层，需要一次构建配置评估（TSAN 与现有 `IPC_EXCEPTION_`/pimpl 宏的兼容性未验证，P2）。

---

## 6. 一句话总结

维度 C 失败不是「某个模块写崩了」，而是**一次范式迁移的收尾缺口**：本次改造把并发边界从「每对象一把锁」扩到「共享 state 对象」，新增的 `shared_ptr` 持有者指针成了**第四条同步边界**，但它没有进入任何一份方案的收尾清单——同一设计意图在 t3 落地、在 t4/t5 丢失。叠加验收面对 `reset_*` / `restart_*` 三条入口零覆盖（且无竞态检测器），这个缺口在功能用例下必然全绿，只能靠人工逐访问点横向对照才发现。这就是 t7 判 needs_revision 的全部理由。
