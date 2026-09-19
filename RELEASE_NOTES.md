# RELEASE NOTES — 2026-09-18(验收证据链升级:三方指纹)

> 范围:**验收/证据基建**,不含产品码变更。本文记录 UF-009 复跑闭环、
> 三方指纹(driver_md5 / libipc_md5 / tree_head)的验收规范,以及本次脚本变更。

---

## 1. 背景

**UF-004 / UF-009 判定实验**(2026-09-18 14:30,artifact
`build/uf004_exit_semantics/20260918_143034_582610/`)产出后,发现其运行记录
`results.json` **不含任何运行时指纹**——当时采集用的是冻结版脚本
`matrix_frozen_8f68e7e3.sh`(连 md5 打印都没有)。后果:

- 读数无法回溯到"哪个驱动二进制、哪个库、哪个源码态"跑出来的;
- UF-004 验收期(17 点)升级的现行脚本写入了 `driver_md5` / `libipc_md5` 两件,
  但 **`tree_head` 至今无人写**,三件套仍缺一;
- 树库在此期间已漂移:`libipc.so` 从 `b7e3c9ca…` → `ed5a0265…`(§16 队列钳制 ≥2
  落码所致),driver 锚从 `142f8152…` → `f295bbb0…`;台账 0.3.4/0.3.5 限定 1 的
  **"构建指纹不同"**约束因此无法自证边界——旧读数是否仍然承重,没有任何机械判据。

**处置原则**:旧 artifact 事后补算 md5 **等于伪造**,不做;缺口用
**"带全指纹复跑 + 对账"**补——先给脚本补齐第三件,再用与原始采集相同的参数
复跑一次,读数逐项一致则旧判定得到佐证、可迁移。

---

## 2. 三方指纹定义

每条验收类运行记录必须绑定**三枚指纹 + 一条解析路径**,缺一即不具证据资格:

| 指纹 | 含义 | 采集方式 | 钉死什么 |
|---|---|---|---|
| `driver_md5` | 被测驱动二进制 | `md5sum tools/sercli_live_probe/bin/sercli_live_driver` | **行为态**:执行主体 |
| `libipc_md5` | 实际加载的库 | `ldd <driver>` 解析 → `md5sum` | **库本体**:不是"源码里的库",是 loader 真正选中的那份 |
| `tree_head` | 源码态 | `git rev-parse HEAD` | **源码态**:同一 md5 的库可由不同树态产出(本地脏改动、rebase 中间态),缺它则"这份读数是哪个源码态跑出来的"不可回溯 |
| `libipc_resolved` | 库解析路径 | `ldd` 原样输出 | 区分 `build/lib/` 与 `local/lib/` 等多副本共存时的加载来源 |

**在证据链中的作用**:

1. **绑定**:把 results.json 的每个读数钉死到(驱动, 库, 源码态)三元组;
2. **漂移判定**:树/库漂移后,机械判定旧读数是否仍承重——带全指纹复跑、
   逐项读数一致 ⇒ 读数可迁移;不一致 ⇒ 旧读数作废、必须重采;
3. **防伪**:指纹由脚本在**运行时**采集写入,事后不可补、不可改
   (补写 = 伪造,见 §5 第 5 条)。

---

## 3. 复跑证据升级路径(UF-009 本轮闭环)

**步骤一:脚本补齐 `tree_head`**(见 §6 变更清单第 1 条)。

**步骤二:同参数复跑**(与 0.3.5 原始采集完全同参数):

```bash
bash tools/sercli_live_probe/exit_semantics_matrix.sh \
  --out build/uf004_exit_semantics/<OUT> \
  --rounds 2 --grace-ms 10000 --hold-ms 6000 --lib-fast-ms 1000
```

本轮实际产物:**`build/uf004_exit_semantics/20260918_233904_fp/`**
(`results.json` + `matrix.csv` + direct/factory × r1/r2 四个腿目录)。

**步骤三:对账**(新 results.json 显式记录的指纹):

```
driver_md5      = f295bbb0d5b7bb944c67326d8b9d70a0
libipc_md5      = ed5a026508fea1e516f061795c373bf6
tree_head       = 3818899687efefd35f8514acdff565226bec7cce
libipc_resolved = /home/zwc/cpp_ipc_dds/build/lib/libipc.so.3
```

| 读数 | 0.3.5 原始(14:30, 无指纹) | 本次复跑(23:39, 带全指纹) | 一致? |
|---|---|---|---|
| direct `server_hypotheses` | `app_handler_alive=2` | `app_handler_alive=2` | ✓ |
| direct SUMMARY / 零残留 | 2/2 / 2/2 | 2/2 / 2/2 | ✓ |
| factory `server_hypotheses` | `library_exit0_fast=2` | `library_exit0_fast=2` | ✓ |
| factory SUMMARY | 0/2(缺失坐实) | 0/2 | ✓ |
| factory 零残留 | 0/2(残留 17/腿) | 0/2 | ✓ |

**结论**:0.3.5 / 0.3.4 的判定读数在当前(驱动 `f295bbb0…`,库 `ed5a0265…`,
树 `38188996…`)三元组下**得到佐证,读数可迁移**。

**边界(不得超读)**:

- 这是"同参数复跑佐证",**不是**两次独立复现;2 轮/via 的样本量限制不变;
- UF-009 的 `⏸️待决策`、⛔不授权改产品码 的状态**不变**——本轮只升级证据,
  不改授权边界;
- 台账 `docs/unfixed_defects.md` 处于冻结单写者协议,本轮**未触碰**。

---

## 4. 验收要求(自本轮起生效)

1. **强制指纹**:此后所有验收/判定类运行记录(results.json 等)**必须显式列出
   三枚指纹** `driver_md5` / `libipc_md5` / `tree_head`;缺任一枚 = 该记录不具
   验收证据资格。
2. **用现行脚本**:现行 `tools/sercli_live_probe/exit_semantics_matrix.sh`
   (变更后 md5 `ffce4f7651fe08d07283171d0a4081ff`)已内建三键自动采集写入,
   直接使用;**不得**再引用冻结副本 `matrix_frozen_8f68e7e3.sh` 采集新证据。
3. **读旧记录先看指纹**:有指纹 → 对三元组后引用;无指纹 → 只能当历史线索,
   **不得**当验收读数引用。
4. **漂移后必须复跑对账**:树/库漂移(libipc.so md5 或 HEAD 变化)后若需引用
   旧读数,必须带全指纹复跑并逐项对账,一致才可迁移(流程见 §3)。
5. **禁止事后补写指纹键**:任何"事后把 md5/HEAD 补进旧 results.json"的操作
   都是伪造证据,一律禁止;缺指纹的旧记录只能按第 3/4 条处置。

---

## 5. 变更清单

| # | 变更 | 类型 | 说明 |
|---|---|---|---|
| 1 | `tools/sercli_live_probe/exit_semantics_matrix.sh` | 工具脚本(未跟踪文件,本轮修改) | **+`tree_head` 采集**:采集点(~:192-194,`git rev-parse HEAD`,取不到时写 `<unavailable>`)与 results.json 写入点(~:526)。三件套凑齐;变更后 md5 `ffce4f7651fe08d07283171d0a4081ff`。`bash -n` 语法通过 |
| 2 | `build/uf004_exit_semantics/20260918_233904_fp/` | 新运行记录 | 带全指纹复跑产物(§3);四键显式在 results.json |
| 3 | `RELEASE_NOTES.md` | 新文档 | 本文件 |

**明确不在本轮**:0.3.5/UF-009 台账文档写回(已叫停);`docs/unfixed_defects.md`
(冻结协议);任何产品码。
