# D02-005 状态

该批次按 `ABBA|BAAB` 启动，并使用 `--ambient-root /home/zwc/MPC_GPU`。第 1 窗完整通过；第 2 窗结束时运行器发现未声明竞争者并按契约停止：`/home/zwc/.vscode-server` 下的 `MainThread`/`cpptools` 工作目录在 `/home/zwc`，不属于用户声明的 JAX 根目录。

本批次不用于 A/B 统计，不从已完成窗口抽取样本。用户声明 JAX 不需要终止，相关进程未被本任务触碰；完整替代批次为 `d02-l0-pairs/006`。
