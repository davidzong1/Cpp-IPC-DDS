# t45 CPU 双工装仲裁 —— 单工装同库重采

## 成对指纹（R0-A1）
```
# t45 单工装重采前置（2026-09-30 10:04:29 +0800）
仲裁工装 = w10_r5_matrix（唯一；⛔ 不用第二工装做对比）
libipc.so.1.3.0 cf209393773d51ee4762dd95bfd1b04f873ec54aa8f61cf5a7ed92be2990fa46
w10_r5_matrix   0802f8d0910a570bfb23cf56aaef8def8d7817ec8089697b97d474ef6e10450e
窗口定义: --window-s 60 --settle-s 4（窗口前静置，不含启动瞬态）
CPU 口径: 进程内 /proc/self/stat token[11]+token[12]（utime+stime）
```

## 结论速览
- 两工装的 CPU **口径完全相同**（都是 /proc/self/stat token[11]+token[12] = utime+stime，已用 rusage 权威交叉核对）
- 外部逐 TID 与进程内读数**一致到 1%**（4 轮比值 0.9925–1.0042）
- **2.3× 分歧不是工装差异**，而是 state2/n=1000 档的 **tick 率双峰（3.4–3.5×）**：同一二进制 14 次重复 cpu∈[0.055,0.188]
- 不变量：cpu ≈ tick 率 × 每 tick 成本；state2/n=1000 的每项每 tick 成本稳定在 **11.9–14.3 ns**，方差几乎全在 tick 率上
- 该档**不可稳定测量**（见报告 §4），建议改用 tick 率归一化判据（§6）

## 根因（t45 结论）
1. 已证实：cpu = tick 率 × 恒定每 tick 成本（state2/n=1000：10–13 µs；state3：45 µs；n=500：7.3 µs）
2. 已证实杠杆：CPU 亲和性 —— 钉核后 cpu 降 2.1–2.9×（3/3 轮），tick 率同步降
3. 已证伪 7 类候选：settle / 窗口长度 / 读 stats() / 名字长度 / SSO-堆布局 / 采样口径 / 注册分散
4. 未闭环：为何同一配置下每 tick 选中项数会落在 7 或 23 ⇒ 开放观察项（归 t32/R6 + 调度器作者）
5. 判定：state2/n=1000 不可稳定测量 ⇒ 该档不作 CPU 判据（或改用 tick 率归一化形态）

## ⚠️ 量纲更正（t51，append-only）
t45 报告列名「每项每 tick」的实测值**实为「每 tick 成本」**（µs/tick）；更正见
`docs/消息接收架构改造/团队改造交付/W10/R6b_CPU双工装仲裁.md` §9。
两口径 **恒不相等**（差一个「注册项数」因子）：`state2 ≈ 11.9 ns/项·tick`、
`state3 ≈ 22.9 ns/项·tick`（**每 tick 成本 = 45.70 µs/tick**）。⛔ 不得互换。
只读复算脚本：`dimension-audit/recompute.py`（输出 `dimension-audit/output.txt`）。
门槛 3 状态**按逐 run 复算更正**：`state3/n=1000` 超 0.20 为 **8/10**（原报 4/10），`max/0.20 = 1.63×`。
