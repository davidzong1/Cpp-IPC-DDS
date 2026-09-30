# R-5 复核：新字段规格 ↔ `field_schema.json`（t50 收口）

## 0) 关键前提（W03 明确提示，我复核确认）

- **旧 run 的 schema 是旧版**：本 run（r33）的 `field_schema.json` 里 含部分字段
  ⇒ 复核必须对**本包（W03 t52）之后新产生的 run** 做；对旧 run 复核必然 False（那是旧 run 的时代，不是 R-5 未闭合）。

## 1) W03 提供的新生成 schema（⛔ 它**不是** W02 证据 run，只作核对样本）

| 规格项 | 在 schema 文本中 | run_level_fields 里有结构化登记 |
|---|---|---|
| `bad_header`（summary 列） | ✅ | ✅ |
| `late_ok` | ✅ | ✅ |
| `backlog_ok` | ✅ | ✅ |
| `send_blocked_ok` | ✅ | ✅ |
| `abnormal_ok` | ✅ | ✅ |
| `bad_header_ok` | ✅ | ✅ |
| `cases[].bad_header` | ✅ | ✅ |
| `cases[].gates` | ✅ | ✅ |
| `manifest.failure_thresholds` | ✅ | ✅ |
| `manifest.currently_citable` | ✅ | ✅ |
| `manifest.run_identity` | ✅ | ✅ |
| `manifest.evidence_discipline` | ✅ | ✅ |
| `manifest.field_aliases` | ✅ | ✅ |

`run_level_fields` 共 **13 条**，逐条带 `owner`/`registered_by`/`zero_semantics`：
- owner = ['W02']（新字段的**语义归属 W02**，不是 W03 —— 这点与我的规格一致：`--purpose` 取值集合与权威认领规则由 W02 CLI 决定）
- registered_by = ['W03']（W03 只登记结构）

## 2) 我的 §6 复核脚本原样重跑（对 W03 的新 schema）

```
csv 列登记: {'bad_header': True, 'late_ok': True, 'backlog_ok': True, 'send_blocked_ok': True, 'abnormal_ok': True, 'bad_header_ok': True}
run_level_fields: 13
```

## 3) CSV 侧结构（W03 顺带修的转义缺陷）

- 行数 = 144，列数集合 = {7} ⇒ **列数一致**（W03 报告「t52 前 8 行、追加后 19 行列数不一致」已修）
- ⚠️ 该缺陷**不是本轮引入**，但它是"按列取值的读方会错位"的真缺陷；W03 已加 `csv_escape()` + 判据 `SchemaCsvIsWellFormed` 锁死 ⇒ 我采纳其修法，不改其文件。

## 4) 状态判定（我的口径）

- **规格已提交**：`t50_R5_新字段规格提交W03.md`（本目录交付面）✅
- **W03 已落地 + 自测**：`run_level_fields=13`、6 列可机械拼接（`summary_csv_extra_header()`）、`manifest_extension_keys()` 给 5 键 ✅
- **我的独立复核**：对 W03 提供的新生成 schema 逐项核对 —— 13 项命中、owner/registered_by/zero_semantics 齐备 ✅
- ⛔ **仍未完全闭合**：本 run（r33）与其它既有 run 的 `field_schema.json` **仍是旧版**（生成器是运行时写的）⇒ **只有在 W03 落地之后新产生的 W02 run 才会含新段**。我将在**下一个新 run** 上复核该段并与产物逐字段对照；在此之前 R-5 记为「已落地、待新 run 复核」。

## 5) 一处待 W03 确认（不影响判定）

- `informational_only` 的**理由正文** W03 只登记了「存在 + 键名定位」，未抄正文（避免跨包语义漂移）。
  ⇒ 我**同意**这个取舍：正文属 W02 判定语义（`failure_thresholds.informational_only` 已在我的 manifest 里逐量给出），⛔ 抄进 schema 反而会在 W02 改动时产生漂移。**无需回填**。
