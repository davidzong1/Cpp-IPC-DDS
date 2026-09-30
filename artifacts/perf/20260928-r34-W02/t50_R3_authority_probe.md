# R-3 前后对照：不带参数的复核轮是否会「抢」权威指针（t50）

## 1. 修复前的实物证据（⛔ 不是我造的探针，是 t47 那轮的产物本身）

`artifacts/perf/20260928-r30-W02/manifest.json` 的 `run_identity`（该轮命令行**未给** `--purpose`）：

```json
{"this_run_id": "20260928-r30-W02", "purpose": "evidence",
 "this_run_is_evidence": true, "citation_authority": "20260928-r30-W02"}
```

⇒ `purpose` **默认成 evidence**、`this_run_is_evidence=true`、且 `currently_citable` 指向**自己**。
这正是 t48/R-3 实测的「默认抢指针」形态 —— 当时没有 `purpose_explicit` 字段，读方**无法分辨**
"我显式声明了我是证据 run" 与 "我什么都没说却被默认当成证据 run"。

## 2. 修复后的行为（本轮三组探针，全部实测）

| 命令行 | `currently_citable` | `run_identity` | 是否符合要求 |
|---|---|---|---|
| **不带任何参数** | **`null`** | `purpose=verification(缺省), purpose_explicit=**false**, this_run_is_evidence=false, authority_undeclared=**true**, claims_authority=false, citation_authority=null` | ✅ **不认领**（指针不变） |
| `--purpose=evidence` | 本 run | `purpose=evidence, purpose_explicit=true, claims_authority=true, authority_undeclared=false` | ✅ 显式认领 |
| `--purpose=verification --citation-authority=<证据 run>` | **证据 run** | `purpose=verification, purpose_explicit=true, claims_authority=false` | ✅ 复核轮正确 |
| `--purpose=whatever`（非法值） | — | — | ✅ **rc=2 拒绝**（不产出任何产物） |

**关键机械判据（t50 验收要求）**：探针命令前/后，**证据 run 的 `manifest.json` sha256 不变**：

```
$ sha256sum artifacts/perf/20260928-r30-W02/manifest.json     # 跑前
09bb9483a68f9150…
$ <跑不带参数的探针轮>                                          # 修复前：r30 之外的新 run 会认领指针
$ sha256sum artifacts/perf/20260928-r30-W02/manifest.json     # 跑后
09bb9483a68f9150…   ← 不变（因为"抢指针"改的是**新 run 自己的** manifest，旧 run 不被改写）
```

⚠️ **诚实说明**：修复前的「抢指针」**不会改写旧 run 的文件** —— 它的危害是**新 run 自己**把
`currently_citable` 写成了自己，从而让"哪个 run 是现行"这件事**静默改变**。⇒ 判据不能只看
旧 run 的 sha256 不变（那在修复前后都成立），**必须同时看新 run 的 `currently_citable`**：

| 判据 | 修复前 | 修复后 |
|---|---|---|
| 新轮（不带参数）的 `currently_citable` | **指向自己**（静默改变现行指针） | `null`（不改变） |
| 新轮是否可自证「显式认领」 | ⛔ 无 `purpose_explicit` 字段 | ✅ `purpose_explicit=false` + `authority_undeclared=true` |

## 3. 为什么选「缺省不认领」（方案 (i)）而不是「缺省 rc≠0」（方案 (ii)）

本基准的 **自检用例（`test_w02_xproc_benchmark`）与 `w02_benchmark.sh` 的默认路径**都不带该参数。
若缺省即拒绝发布（方案 ii），它们会**全部变成假红** —— 而它们本身与"认领权威"无关。
⇒ 安全性由「**认领必须是显式动作**」保证（缺省一律 `currently_citable=null`），
而不是靠「让常用路径失败」保证；非法 `--purpose` 值仍**立即 rc=2 拒绝**（不产出半成品产物）。

## 3bis. 探针实物归档（本目录 `r3_probe/`）

三组探针的 `manifest.json` 原样归档（无需重跑即可复核）：

| 目录 | 命令行 | `currently_citable` | `run_identity` 关键键 |
|---|---|---|---|
| `r3_probe/noargs/` | **不带任何参数** | **`null`** | `purpose=verification, purpose_explicit=false, authority_undeclared=true, claims_authority=false` |
| `r3_probe/explicit/` | `--purpose=evidence` | `t50_explicit`（自己） | `purpose_explicit=true, claims_authority=true, authority_undeclared=false` |
| `r3_probe/verif/` | `--purpose=verification --citation-authority=20260928-r30-W02` | `20260928-r30-W02`（证据 run） | `purpose_explicit=true, claims_authority=false` |

⇒ 三态互不混淆；**"不带参数"落在 `noargs` 那一态（不认领）**，这正是 R-3 要求的默认安全。

## 4. 落地位置

- 判定：`test/xproc_benchmark.cpp`（`Config::purpose` 默认**空**、`purpose_given`；manifest 的
  `currently_citable`/`run_identity` 与 `evidence_discipline.authority_claim_rule`）
- 脚本：`test/w02_benchmark.sh`（`W02_PURPOSE` 默认 `evidence`、`W02_CITATION_AUTHORITY` 可选；
  跑完打印 `purpose`/`currently_citable`，并在 `authority_undeclared` 时给出警告）
- 纪律成文：README §5 使用约定 + 《结果格式与使用》§11.55（含两条可机械核对判据）
