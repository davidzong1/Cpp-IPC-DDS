# W10 收口复跑（t33 / D-26）—— 证据目录

run 时间 2026-09-30 02:00:17 +0800

## 前置指纹（方案 §10.6）
库 libipc.so.1.3.0  81fe91ffa14ee00a9ebfeb8eee23cd1bfae71956545ee79e686919a8e6f8d4c9
w10_matrix          2ad64f61b8e99d3d614bdbb2af852465ac73636bca481c8b834f03552bec51d2
w10_scancost        21ad0d413ad1244234fbc3ae4463742493dd3f84aba353ca3090eeed38e08a00
socket_pub_sub_ipc.cc (t29)  b1867d0f34101a395129d2a20ba99bc039af3ff74cf8855276fd83f2159c665c
recv_worker.cc        (t30)  0dde3eabd02e23bafffa3142c20c525535f65b1da2635704c5b25458f742650c
git HEAD            e800ccc496ac710b711c9346709e86a148c41241

## 内容
| 目录/文件 | 内容 |
|---|---|
| final-1/2/3 (+.log) | socket 1000 独立话题臂 **3 轮**（§13.2 六条逐条判定 + 逐 route 台账） |
| scan10.2/ | §10.2 五项量：N(100/500/1000)×W(4/8/16/32)×diag(off/on) 共 24 组 + summary_table.tsv |
| thr/ | 线程成分归因（订阅侧 33 线程 vs 发布侧 301 线程） |
| observation-nonrepro-round/ | 非复现异常轮（1/5，重名补齐缺陷所致）与其 README |
| ctest_j32/ | ctest -j32 × 12 轮（干净单实例；12/12 rc=0） |
| hotpath_gate_before.log / _after.log | 热闸前后读数 |

## 收敛摘要

| 项 | 读数 |
|---|---|
| socket 1000 独立话题 | **3/3 轮 PASS**：`registered=1000/1000`、`valid_rx=1000/1000`、`fallback=0`、`corrupt=0`、`重名=0` |
| §13.2 六条 | #1–#5 ✅；**#6 = 部分测量**（queue/chunk 无公开读数 API ⇒ 未测） |
| §10.2 | 24 组 = N(100/500/1000)×W(4/8/16/32)×diag(off/on) 全 rc=0；接线点 `recv_worker.cc:233` |
| ctest -j32 | **12/12 轮 rc=0**；t11 两项待观察缺陷均未复现（⛔ 未复现 ≠ 结案） |
| 热闸 | before 未过闸 / after 过闸；1 MiB 档 12 轮探针 **5/12 低于下限** ⇒ 既有波动面 |
| 工装修复 | W1（取错列）/W2（未排除临时端口区间）/W3（重名补齐⇒串包）三处，均已修 |

⛔ 本目录**只追加**；r25-W10 未被覆盖。
⛔ `ctest_j32_mixed_two_jobs/` 是作废件（两次脚本交叠），不得引用。
