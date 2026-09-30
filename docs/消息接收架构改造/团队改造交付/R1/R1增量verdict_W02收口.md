# R1 增量 verdict · W02 收口复核（t47 后）

> **⛔ 本文件是「增量」verdict，非 R1 主体重开。** 只针对 W02 的 **t14 needs_revision 项**是否闭合。
> ⛔ 不重开 `t11`；⛔ 不重做 t14 中已 PASS 的 W01 / W04 / W06 / W01-FU。
> 评审人：**架构负责人**（第三方，⛔ 不是 W02 实现者）。任务 `t48`；attempt_id `381ed87e-04e3-4947-8443-0d40e75a0535`。
> 复核对象：`artifacts/perf/20260928-r30-W02/`（现行 `currently_citable`）、`团队改造交付/W02/`（README §5/§5bis、`结果格式与使用` §7.1/§7.2/§7.3/§11.6）、`test/w02_summary_schema_check.py`、`test/w02_sample_audit.py`。
> 复核方式：**只读复算 + 独立注入**（注入在 `build/t48/` 的**副本**上做，⛔ 未改仓库内他人文件、⛔ 未改任何既有 run、⛔ 未改 t11 终态）。

---

## 0. 判定

| # | 收口项 | 判定 |
|---|---|---|
| 1 | **F0 引用链** | ✅ **闭合** |
| 2 | **F3 失败量进判定** | ✅ **闭合**（改的是实现，⛔ 不是改措辞） |
| 3 | **C)② A/B 可分性更正** | ✅ **闭合** |
| 4 | **B)⑥ 两套基准更正** | ✅ **闭合** |
| 5 | **结构闸有效性（独立注入）** | ✅ **拦得住**（7 组注入全转红 + 正/负控成立） |
| 6 | **B 档 warmup 同形要求** | ✅ **闭合**（要求已入文档；缺陷已修，量级我复算确认） |
| 7 | **回归 `ctest -j4`** | ✅ **28/28** |

### ⇒ **W02 的 t14 `needs_revision` 项已闭合。**

**供队长判断 t14 最终状态**：t14 的判定是 `needs_revision`（failed），其**唯一**的 needs_revision 原因是 W02（W01/W04/W06/W01-FU 在 t14 已 PASS）。现 W02 的收口项经本轮**独立复算与独立注入**全部闭合 ⇒ **t14 的 needs_revision 在 W02 维度上已无未闭合项**。但按纪律：⛔ **我不自行改写 t14 的终态**（终态不可变），请你据本文件判断是否另作记录。

**两条不构成阻断的残余（如实登记，⛔ 不写成已解决）**：
- **R-1（low）**：结构闸的**动态半**（逐行 `gate↔failure_reasons` 一致性）**不在 ctest 自动回路内** —— ctest 登记的是 `--source-only`（静态半）。我注入 F/G（`gate=0` 而名点空 / `gate=2`）**只有动态半能抓**；这两个形态在 CI 里不会被自动拦住。
- **R-2（low）**：「真实负载名点」的归档件 `superseded/w02_r24.out` **路径不存在** —— 实物在 `tmp/w02_r24.out`，而 `.gitignore:97` = `tmp/**` ⇒ **未被 git 跟踪、可被清理**。我已把副本落到 `R1/复算脚本/t48_evidence/w02_r24.out`（sha256 与原件逐字相同 `0ff35030…`）以免证据丢失，但**归档位置本身**的声明仍与实物不符。

---

## 1. F0 引用链 —— ✅ 闭合

### 1.1 `cells` / `skipped` 逐 run 独立复算（**三套实现交叉**）

我用**三个独立实现**跑同一批 run（⛔ 不采信 t47 与队长的转述）：

| run | 状态 | t47 的 `w02_sample_audit.py` | **我 t14 的脚本** | **awk 第三实现** | 恒等式 `cells==4×(rows−skip)` |
|---|---|---|---|---|---|
| r19 | 非现行 | 269896 / 26 | 269896 / 26 | — | ✅ |
| r20 | 非现行 | 269848 / 38 | 269848 / 38 | — | ✅ |
| r23 | 非现行 | 269852 / 37 | 269852 / 37 | 269852 / 37 | ✅ |
| **r30（现行）** | **现行** | **269788 / 53** | **269788 / 53** | **269788 / 53** | ✅ |

三实现逐值一致、四 run `mismatches=0`：**与队长独立复算（269896/26、269848/38、269852/37、269788/53）逐位吻合** ✅。

### 1.2 「`cells` 不是常量、恒等式才是判据」—— ✅ 落实且**我机械验证了它的必要性**

```
  run     cells  skipped  4*(rows-skip)   恒等式
  r19    269896       26         269896   ✅
  r20    269848       38         269848   ✅
  r23    269852       37         269852   ✅
  r30    269788       53         269788   ✅
  cells 取值集合 = [269788, 269848, 269852, 269896] ⇒ **4 个不同的值，不是常量**
```
文档落实点：`结果格式与使用:358` 给出「行口径（现行 run r30）」行并在 `:362` 直书 **「cells 不是常量（逐 run 变化），能被机械复核的是恒等式」**；`:392` 的运行示例加注「⛔ 不要把它当成固定值」；`:399` 的验收表把判据写成 `cells==4*(rows-skipped_rows)`；audit 脚本 `:94` 有 `assert cells == 4*(rows-skipped_rows)`。**manifest 侧**：`evidence_discipline` 未直接写该恒等式（它在动 run 的 `summary.csv` 与脚本里），但 `:392` 的命令行与 `:399` 的判据表是机器可执行的 ⇒ **可接受**。

### 1.3 三个机器可读键 —— ✅ 正确

| 键 | 实测 |
|---|---|
| `currently_citable` | `"20260928-r30-W02"` ✅ |
| `run_identity` | `{"this_run_id":"20260928-r30-W02","purpose":"evidence","this_run_is_evidence":true,"citation_authority":"20260928-r30-W02"}` ✅ |
| `superseded_runs.non_current_runs` | **8 条**，逐条 `{run_id,status,reason}` ✅（另有 `superseded_run_ids` **27 条** 与 `authority_rule`） |

**⛔ 合同特别要求的那一条 —— r19 的 reason 是否如实记录「t25 合同引用的数字」**：
```
{"run_id":"20260928-r19-W02","status":"非现行",
 "reason":"*_messages 因 D-17 计数双写作废（约 2× 且回退场景下把「期望路径」记成「实际路径」）；
           cells=269896/skipped=26 是 t25 合同引用的数字，⛔ 不得再作为现行结论"}
```
✅ **逐字包含**「cells=269896/skipped=26 是 t25 合同引用的数字，⛔ 不得再作为现行结论」—— 队长的错误被**永久、机器可读**地登记为「为何该 run 非现行」的原因之一。这是比改一句文档更强的处置。

**8 条的目录实况**（我逐个 stat）：r19/r20/r23 目录存在；r24…r28 目录**不存在**（已登记为「仅控制台归档」）⇒ 与 `status` 相符，无「声称非现行却仍是唯一实物」的矛盾。
⚠️ 一处**不影响结论**的不一致（如实登记）：`non_current_runs`（8）⊂ `superseded_run_ids`（27），且 **r23 只在 `non_current_runs`、不在 `superseded_run_ids`**（其 status 是「历史证据 run」而非「非现行」）。两个键**语义不同**（一个是「非现行+原因」的结构化清单，一个是「目录已清理」的名单），故不构成错误，但读者需明白二者不可互相代入。

### 1.4 负控（合同要求）—— ✅ **我独立跑了一轮，权威指针未被抢走**

```
./build/bin/xproc_benchmark --path=tlv ... --purpose=verification \
    --citation-authority=20260928-r30-W02 --run-id=t48_verif_probe --out-dir=build/t48/verif_probe
⇒ rc=0
   manifest.run_identity  = {"this_run_id":"t48_verif_probe","purpose":"verification",
                             "this_run_is_evidence":false,"citation_authority":"20260928-r30-W02"}
   manifest.currently_citable = "20260928-r30-W02"     ← **指回证据 run** ✅
```
并核对**证据 run 自身未被改动**：`r30/manifest.json` 的 mtime 仍为 `10:42:41`（我的复核轮跑在 `13:53`）⇒ ⛔ 未被覆盖 ✅。

**⚠️ 我另做了一次「不带参数」的反驳尝试**（合同未要求，但属独立复核应有之疑）：
```
--purpose=evidence（默认）+ 只给新 run-id、不给 --citation-authority
⇒ manifest.currently_citable = "t48_noargs_probe"（**指向自己**）
```
⇒ 机制上**复核轮确实可能抢走权威指针**（当复核者忘记带参数且把 `--out-dir` 指向 `artifacts/perf/` 时）。我的探测落在 `build/t48/`，⛔ 未影响 r30。**文档面（README / 结果格式与使用）没有把「复核轮必须带 `--purpose=verification` + `--citation-authority`」写成纪律**（两文档 `grep -c purpose` 均为 0）⇒ 记为 **R-3（low，建议而非阻断）**：把这条写进 README §5 使用约定，或让非 `evidence` 轮在缺 `--citation-authority` 时**拒绝写 `currently_citable`**。

### 1.5 引用审计 —— ✅ 我独立重扫，结论与 `f0_reference_audit.md` 一致

我自己扫三份 W02 文档的 `20260928-rNN-W02` 出现分布：

| 文件 | r19 | r20 | r23 | r24–r28 | **r30（现行）** |
|---|---|---|---|---|---|
| `README.md` | 1 | 0 | 0 | 0 | 3 |
| `W02_统一跨进程基准_结果格式与使用.md` | 0 | 0 | 0 | 0 | 4 |
| `W02_中间轮次留痕与r18去向.md` | 1 | 1 | 5 | 0 | 2 |

与 `f0_reference_audit.md` 声称的分布**逐格吻合** ✅。逐处读上下文：README `:95` = 「⚠️ r19 的 `*_messages` 已作废；r20/r23 亦非现行 —— 只可引用 `currently_citable`」；留痕文档 `:31–35` 的 r19/r20/r23 全部在「保留/非现行 + 原因」列。**未发现把旧 run 当现行结论引用的位置** ✅。

---

## 2. F3 失败量进判定 —— ✅ 闭合（改实现，⛔ 非改措辞）

### 2.1 列与 gate —— ✅ 落盘

| 检查 | 实测 |
|---|---|
| `summary.csv` 列数 | **93**（r23 = 87，净增 6）✅ |
| 5 个 gate 布尔列 | `late_ok` / `backlog_ok` / `send_blocked_ok` / `abnormal_ok` / `bad_header_ok` **均存在** ✅ |
| `bad_header` 补列 | **存在**（r23 全表零命中）✅ |
| `results.json.gates` | 逐 case 有 5 键：`{late_ok,backlog_ok,send_blocked_ok,abnormal_ok,bad_header_ok}` ✅ |

### 2.2 确实进 `failure_reasons` —— ✅（**我踩到的假阴性本身就是证据**）

⚠️ 我先用**静态 grep** 扫 `add_reason(r, "...")`，得出「`late`/`backlog` **不进**」——**这是假阴性**：这两条的文案由 `snprintf` 写进 `char buf[256]` 再 `add_reason(r, buf)`，字面量里不含关键词。⇒ 我改用**运行期产物**判定，这正是队长点名的那条纪律「静态 ∪ 运行期，单一静态口径会误判」。

**运行期证据（`f3_negctl/`，阈值压到 ~0）**：
```
case = r0_tlv_full_blocking_1024B_1000Hz
gates = {late_ok: false, backlog_ok: false, send_blocked_ok: true, abnormal_ok: true, bad_header_ok: true}
case_ok = false
failure_reasons[0] = 迟发超出阈值: late=1 (0.1000%) > 阈值[abs<=0 且 rate<=0.0000%]（阈值见 manifest.failure_thresholds；仅落列不判定=静默排除，故进判定）
failure_reasons[1] = 发送积压超出阈值: backlog_max=114085 ns = 0.114 个周期 > 阈值 0.000 （阈值见 manifest.failure_thresholds）
⇒ rc=1；cli_overridden=true；run_identity.purpose=verification，citation_authority 指回证据 run
```
✅ 两个 gate **同一轮**被判亮；原因句**带阈值与实测值**（不是只写"失败"）。`send_blocked`/`abnormal`/`bad_header` 的 `add_reason` 在源码里可直接 grep 到（`:2654/:2669` 一带 + `send_blocked`/`abnormal`/`bad_header` 三条）。

### 2.3 阈值写入 manifest —— ✅ 最完整的一个交付点

`manifest.failure_thresholds` 实测含：6 个阈值（`late_abs_max=20`、`late_rate_max=0.05`、`backlog_max_periods=10`、`send_blocked_max=0`、`abnormal_max=0`、`bad_header_max=0`）+ `cli_overridden=false` + `gates`（5 项）+ **`gates_in_judgement`（逐 gate「进判定 + 本 run 触发 N 个 case」）** + **`informational_only`（5 个量逐条给「为什么只作信息性保留」的理由）** + `note`。
✅ 这正是我在 t14 里要求的形态：**不是「只落列」**，而是「阈值显式 + 逐量声明」。

### 2.4 三层证据 —— ✅ 齐（且第②层我做了**独立复算与独立重跑尝试**）

| 层 | 内容 | 我的独立核验 |
|---|---|---|
| ① **人工负控** | 阈值→0 ⇒ rc=1、`late_ok=0 且 backlog_ok=0` | ✅ 我直接读 `f3_negctl/results.json`（**非引用文字**）：gates 两 false、`case_ok=false`、两条名点带阈值与实测值、`cli_overridden=true` |
| ② **真实负载名点** | 0.5%/1 周期档 40 处名点 | ✅ **独立复算归档件**：`迟发超出阈值` **= 40**、`发送积压超出阈值` **= 32**（合 72）；含名点的 23 个 case **全部** `-> FAIL`；阈值串实测 `abs<=5 且 rate<=0.5000%` 与原档位一致。⚠️ 我还**亲自重跑**了同档位（`--late-abs-max=5 --late-rate-max=0.005 --backlog-max-periods=1.0`）3 轮 + 4 路并发，**均未复现名点**（本机当前负载偏低，`late` 多为 0–3）⇒ **该层依赖归档件**，无法当场复现（**非缺陷**，属负载条件差异，但读者须知）。⚠️ 归档路径声明有误（见 R-2） |
| ③ **默认档 0 触发** | 5%/10 周期 ⇒ 0 case 亮 | ✅ 我复算 r30 默认 run：`cases=75`、`FAILED=0`、**5 个 gate 触发计数全 0** |

另外 r30 的 headline 我**全部独立核验**：`cases=75`、`case_ok=True` 75/75、`failure_reasons` 空 75/75、`child_killed` 合计 0、`sent_ok==plan` 75/75、`recv_measure==plan` 75/75、`missing/dup/ooo/crc` 全 0、样本文件 75 个 ✅。

### 2.5 前后对照 —— ✅ 我独立复算，与 t47 声称一致

| 维度 | r23（修前） | r30（修后） | 我的复算方式 |
|---|---|---|---|
| `late` 非零 case | **17/75** | **14/75** | 逐行读 `summary.csv` |
| Σ`late` | **71** | **55** | 同上 |
| 其中「`late` 非零但 `failure_reasons` 为空」 | **17 —— 17/17 全部无判定通路** | **14 —— 逐 case 可读 `late_ok=1`** | 交叉 `summary.csv` × `results.json` |
| `late_ok` 列 | 不存在 | 存在（14 个非零 case **全为 1**，即「已判定、未超预算」） | 同上 |

✅ **「修复前 17 条完全无判定通路」经我独立复算成立**（与队长的独立复核 17/17 一致）。

---

## 3. C)② A/B 可分性更正 —— ✅ 闭合

**文档已撤回**：`§7.1` 标题即「A/B 的可分性**只在发布侧**成立（C)② 更正）」，正文写明「本文档早前把 `via_view > 0` 列为 **B 档专属判据**……**该判据无区分力**」；`README §5bis` 第 1 条同样撤回。

**我独立复算 r30（15 case/档）**：

| 档 | `via_view` | `via_object` | `via_dds` | 发布侧 `wire_bytes_source` |
|---|---|---|---|---|
| `dzflat-a` | **13500** | 0 | 0 | `对象 dzflat_size()` |
| `dzflat-b` | **13500** | 0 | 0 | `B 借样 chunk 容量` |
| `tlv` | 0 | **13500** | 0 | `对象 serialize() 长度` |

⇒ **消费者侧 A 与 B 逐值同形** ✅（与文档声称的 `13500/0` 一致；`via_view>0` 对 A/B 确无区分力）。可分性确在**发布侧**：`wire_bytes_source` 两档不同 ✅。
**并已落实为机器可读声明**：`manifest.failure_thresholds.informational_only.via_view` = 「…且实测 A 与 B 两档消费者侧同形（各 13500/0），判它没有区分力」✅ —— 即「撤回」不只是文档措辞，而是**进了 manifest**。

---

## 4. B)⑥ 两套基准更正 —— ✅ 闭合

**文档已撤回**：`§7.3` 标题「本仓存在**两套**基准（B)⑥ 更正，含队长错误记录的撤回）」，正文「此前『本仓只存在一套 W02 基准』的表述**不成立**，队长已记录该错误」；`README §5bis` 第 2 条同样撤回。

**我独立核实两套基准为真**：

| 基准 | 我的实测依据 |
|---|---|
| **W02** = `xproc_benchmark` | `summary.csv` 的 dzflat 相关列**只有 `pub_dzflat`**（`grep -c "dzflat_a_messages\|dzflat_b_messages"` = **0**）⇒ 确为**合并**计数 ✅ |
| **W08** = `test_w08_dzflat_ab` | 实物复算：`r13-W08-dzflat-a-prebuilt/counters_publisher.json` ⇒ `tlv=0 a=120 b=0`；`r15-…-dzflat-b-prebuilt` ⇒ `tlv=0 a=0 b=120` ⇒ 确为 **A/B 分开**计数 ✅ |
| 署名各自独立 | `r30-W02/manifest.work_package="W02"`；`r10/r11-W08-*/manifest.work_package="W08"`（`transport=dzflat-b/tlv`）✅ |

**4 条跨包引用规则**（§7.3）我逐条对上实物：① ⛔ 不得用 W02 的合并 `pub_dzflat` 冒充 A/B 可分（依据成立，见上）；② 要 A/B 分列就引 W08（实物有该能力）；③ 两基准互不替代；④ 各自署名、`work_package` 不参数化（实物 `work_package` 各自正确）✅。

---

## 5. 结构闸独立注入 —— ✅ 拦得住（⛔ 不只读自述）

**闸脚本**：`test/w02_summary_schema_check.py`（sha256 `10d4a38a6ef871fa…`），判据 3 条静态 + 2 条动态。
**注入方式**：把闸脚本与 `xproc_benchmark.cpp` 复制到 `build/t48/inject/`，**在副本上注入**（⛔ 未改仓库内他人文件）。留痕：`R1/复算脚本/t48_evidence/结构闸注入结果.txt`。

| # | 注入内容 | 期望 | 实测 | 结果 |
|---|---|---|---|---|
| 正控 | 未改动的 r30 现行 run | 通过 | exit 0 | ✅ |
| **A** | **表头加 1 列、格式串不变**（= **t47 的自报形态**） | 转红 | exit 1 | ✅ `FAIL: 表头列数 94 != 格式串字段数 93` |
| B | 数据行格式串多加 1 个 `%llu`、表头不变 | 转红 | exit 1 | ✅ `93 != 94` |
| C | 必需列 `late_ok` 改名 `latex_ok` | 转红 | exit 1 | ✅ `FAIL: 表头缺必需列 ['late_ok']` |
| D | `summary.csv` 首数据行少 1 列 | 转红 | exit 1 | ✅ `FAIL: 1/75 行列数与表头(93)不符 [(2, 92)]` |
| E | `summary.csv` 为 **0 字节**（= t47 的崩溃形态） | 转红 | exit 1 | ✅ `FAIL: 为空（0 字节 ⇒ 可能是格式串越界崩溃）` |
| F | `gate=0` 且 `failure_reasons` 为空 | 转红 | exit 1 | ✅ `FAIL: 1 行出现「gate=0 但 failure_reasons 为空」` |
| G | gate 取非 0/1 值（`=2`） | 转红 | exit 1 | ✅ 同上分支 |
| 负控 | `r23`（F3 修前，缺 6 列） | 转红 | exit 1 | ✅ `FAIL: 缺必需列 ['bad_header','late_ok',…]` |

**7 组注入 + 1 组负控全部转红、正控通过** ⇒ 闸**有效**，且**恰好覆盖 t47 的自报缺陷形态（注入 A）**。

**我另做的独立小实验（用于判定「静态闸 vs 崩溃」的强弱）**：构造最小 C 程序验证「表头/格式串不一致」**不必然 SIGSEGV**（取决于多余/缺失在实参顶行还是格式串顶行）⇒ **静态列数比对是更早、更可靠的拦截**，比「跑一遍看崩不崩」强。✅ 结论支持 t47 的设计选择。

**⚠️ 但闸有覆盖边界（记为 R-1）**：ctest 里登记的是 `--source-only`（**静态半**），而注入 **F/G（逐行 gate↔名点一致性）只能由动态半抓到**、source-only 全绿 ⇒ **这两个形态不在自动回路内**。建议（归 t47/基准负责人）：在自检用例跑完后调一次 `check_run(<新 run 目录>)`，或让 baseline 自检轮把动态半接进 ctest。

---

## 6. B 档 warmup 同形要求 —— ✅ 闭合（要求入文档 + 缺陷已修，量级我复算）

**要求已写入文档**：`结果格式与使用 §7.2` 标题即「B 档 `warmup` 与 `measure` 必须**同形**（t47 新发现并已修）」，说明「修前 B 档的 warmup 帧只 `set_data4(true)` 就发布（没有三段分配、没有时间戳头）⇒ 每条 warmup 帧都被记成 `bad_header` + `abnormal`」；`:208` 的 B 档 must-have 也含「`warmup` 与 `measure` 两段**同形**，见下」；`README §5bis` 第 4 条同。

**我独立复算量级（本人扫描，⛔ 非引用）**：
- r23 的 `stdout/sub_*.log` 里 `bad_hdr=` 非零的 case **恰好 15 个，全部是 `dzflat-b`**（`r30…r44`）⇒ **确为 B 档专属形态** ✅
- 逐 case 实测值：**735–1200**（64 B/1 KiB 档 1138–1200；1 MiB 档 735–737）
- r30 同位置扫描：**非零 0 个**；`summary.csv` 的 `Σbad_header = 0`、`Σabnormal = 0` ✅
- `bad_header` 列：r23 **不存在**（`grep -c` 零命中）→ r30 **存在** ✅

⚠️ **一处数值表述需收窄（low，不阻断）**：文档与 README 三处写「B 档实测 **532**–1200/case」，但我把**所有可查 run** 的 B 档 `bad_hdr` 都扫了一遍，最小值是 **r20 = 706 / r23 = 735**，**未找到任何 532 的实物**。⇒ 区间**上沿 1200 正确、下沿 532 未见实物**（可能是别的档位/别的量纲的读数被并入了区间）。**结论方向与「量存在但不落列」的判断不受影响**。

---

## 7. 回归 —— ✅ `ctest -j4` = 28/28

```
ctest --test-dir build -N     ⇒ Total Tests: 28（t47 新增 test_w02_summary_schema 使其 27→28）
ctest --test-dir build -j4    ⇒ 100% tests passed, 0 tests failed out of 28   (55.69 s)
./build/bin/test_w02_xproc_benchmark ⇒ 10 tests from 3 test suites, [ PASSED ] 10 tests
   （含 t47 新增的 FailureCountersAreJudged，源文件 :330）
```

---

## 8. 残余项清单（⛔ 均不阻断本增量 verdict）

| ID | 严重度 | 问题 | 最小修复 | 归属 |
|---|---|---|---|---|
| **R-1** | low | 结构闸的**动态半**（逐行 `gate↔failure_reasons`、`gate∈{0,1}`）**不在 ctest**（只登记 `--source-only`）；注入 F/G 印证该形态不会被自动拦 | 自检用例跑完后调 `check_run(<run_dir>)`，或增加一条带 run_dir 的 ctest 项（用现行 run 目录） | 基准负责人（t47 后续） |
| **R-2** | low | 「真实负载名点」归档路径 `superseded/w02_r24.out` **不存在**；实物在 `tmp/w02_r24.out`，`.gitignore:97 = tmp/**` ⇒ **未跟踪、可被清理**（方案 §12 明令不得依赖 `tmp/`） | 把 `w02_r24.out` 复制进 `superseded/` 并改文档路径；我已在 `R1/复算脚本/t48_evidence/w02_r24.out` 留副本（sha256 `0ff35030…` 与原件逐字相同）以防丢失 | 基准负责人 |
| **R-3** | low | 文档面**未把**「复核轮必须带 `--purpose=verification` + `--citation-authority`」写成纪律；不带参数时复核轮**会**把 `currently_citable` 指向自己（我实测） | 写入 README §5 使用约定；或让非 `evidence` 轮在缺 `--citation-authority` 时**拒绝**写该键 | 基准负责人 |
| **R-4** | low | `bad_header` 量级表述「532–1200/case」的**下沿 532 未见实物**（我扫全部可查 run 的最小值 = 706/735） | 改为实测区间（如 `735–1200`）或标注 532 的来源 | 基准负责人 |
| **R-5** | low | 新字段未进 `field_schema.json`（`bad_header` / 5 个 `_ok` / `failure_thresholds` / `currently_citable` / `run_identity` / `evidence_discipline` 均零命中）—— 而 `field_schema.json` 是 **W03 的单一事实来源**（由 `schema_document_json()` 生成） | 跨包事项：W02 的 6 个新列与 4 个新 manifest 键需在 W03 的 schema 登记（或由 W02 在文档中声明「以下字段不在 W03 schema 射程」） | W02 ↔ W03 |

---

## 9. 边界（不得外推）

1. ⛔ **本文件不重开 `t11`**：`t11` 的 `failed` 是历史终态，我未读取、未修改、未改写其结论（实测其报告内三处「不改本报告的原判定」声明仍在）。
2. ⛔ **未改任何既有 run**：r19/r20/r23/r30 的 mtime 均保持原值（r30 = `10:42:41`，我的复核轮跑在 `13:53` 且落在 `build/`）。**新 run 只落在 `build/t48/`**（`verif_probe` / `load_probe` / `load_bg*` / `noargs_probe`）。
3. ⛔ **未改他人 WP**：注入一律在 `build/t48/inject/` 的**副本**上做；仓库内 `test/xproc_benchmark.cpp`、`test/w02_summary_schema_check.py` 的 sha256 与我复核开始时一致。
4. 本增量 verdict **只覆盖 W02**；W01/W04/W06/W01-FU 的 PASS 沿用 t14，未重做。
5. 「W02 收口项闭合」⛔ **不等于** W02 升 S4 的最终签署 —— 那需队长据本文件判断；也⛔ **不等于**任何 G 门槛状态变化（G2/G4 未解除，三独立状态沿用 t14/R7）。
