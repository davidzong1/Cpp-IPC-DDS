# W10-R4b 证据目录（run_id 20260930-r46-W10-R4b）

> t43｜负责人：验证与性能负责人｜报告：docs/消息接收架构改造/团队改造交付/W10/R4b_计数闭环复验.md

## 库/工装

```
# t43 成对指纹（R0-A1）
captured 2026-09-30 08:40:03 +0800

libipc.so.1.3.0 558f47ede2419e14554766af55e53671e69d05d940b05351b1d4aa0e45d79031
w10_matrix      c62f80c9378586398ddc86a04e3e46b145ef827d2dd8c3f96399cae0c5a6d7f2
w10_r4b_firstfail 9fc8e56eeafe7f6ec7ce9369e36edbad0183941cff2624b8c912ecc7387973cd
w10_r4b_runtimetruth 83a05415dd636f172cd4d3272905dd9330c5c612a3a6d2a3fe0e8c6213270a0c
f1_wiring_probe 782b432a7551b6d75fc32cf7bdf6fb07da435a4415ae447dbd7d32a6fd1396fd
armD_waitset_full 4f0347c20e07688e5ada80a1fb83dc56c6c870502ba63cce9735b79bac8d0544
armE2_token_invalid e64b634de35e322503c96cc0864dbdd27647b27d774b47d407a691b99b792909

## 源文件（护栏）
counters.h      1f9c2bd0b9419aea1332c9c295e032ef8e5889e40bf48ebdbf79d8659530b6ae
ipc.cpp         379918b9f6a24da928bda1c843adb29a7bd8367d5e0e14ca3a25dfa2873d0d59
circularqueue.h 4fb899810d83466b60a6b44336c6104076bb4aca5f83fba064844ae74280754e
```

## 内容
| 目录 | 内容 |
|---|---|
| dual-arm/ | SHM + socket 各 1000 独立话题（R3 工装），逐 route 台账 + counters.json（含覆盖声明） |
| arms/ | A(send腿)/B(loan腿)/C(队列)/C2(跨二进制)/D(waitset)/E2(token) 六臂 BEFORE/AFTER + 汇总 |
| scale-capacity/ | W=1（873 回退）、hotcold 20000/40000（19936/39936 淘汰）、H7 修后 3 轮 |
| firstfail/ | first_failed_resource 两阶段对照（none → chunk_exhausted，locatable=true） |
| runtime-truth/ | 运行期真相表三条腿（t35 27 项对账 ⇒ 发现 1 项静态误判） |
| h7-race/ | 工装竞态 H7 的修前证据与说明 |
