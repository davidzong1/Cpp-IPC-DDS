# 与新版 UDP 实现的独立整合验证

组合分支：`fix/writer-boundaries-integrated`，工作区 `/tmp/dzipc-writer-integrated`。合并提交 `91f4121c` 的父提交为另一位 Agent 的 `b16df1a6` 和本轮采样分支 `9dbc33ea`。**主工作区 dev 保持 b16df1a6，未改动其索引或未跟踪证据。**

## 代码边界

唯一冲突在 `PublisherEndpoint::Impl::deliver`：保留新版 `call_started_ns`、MessageTraceKey 和全部消息追踪调用，将原有发布者ID/序号三字段填写替换为本分支统一的 `identify(out)`。本机/离线路径同时使用该函数，各发布者独立序号及已有提交/重试语义保持一致。

对另一位 Agent 从共同起点修改的 `src`、`include`、`test/shared_net` 范围逐文件比较，39个文件中38个与 b16df1a6 的 Git blob 完全一致，只有上述发布端文件进行组合修改。v2协议、端点生命周期、socket/worker、公平调度、benchmark.py及网络工装均保留，见 [provenance.json](provenance.json)。本分支另有原来的四个诊断缝，未新增等待策略或延迟候选。

## 干净构建修复

第一次配置失败：CMake引用的既有 `test/perf/w10/w10_rebuild_crash.cpp` 被 `.gitignore` 的 `*build_*` 忽略，干净工作区没有该文件。已从原独立工作区原样纳入，SHA256与主工作区的同名原文件一致；添加仅针对该源文件的忽略例外，提交 `7f8a6f10`。没有删除构建目标或使用空实现绕过配置，实际已成功编译该工装，但未运行其故障注入场景。

配置使用 RelWithDebInfo、shared-net ON、tests ON、Python绑定及demos OFF、自动生成更新 OFF。构建产物位于 `build-integrated`，未使用另一 Agent 的构建目录。

## 验证结果

|验证|结果|
|---|---|
|共享网络 CTest 标签|首轮38/39通过；唯一失败为漏构建CLI测试工具|
|CLI缺失产物补齐|先补dzipc_topic_cat，复测发现还缺dzipc_list；补齐后该项1/1通过，全部39项均已有通过记录|
|发布者身份确定性回归|包含在首轮通过的partial_submit中，覆盖本机/离线、两个发布者及独立序号|
|1 SUB/4KiB，3秒100Hz|300次发布/300次接收，零丢失/重复/损坏|
|1 SUB/1MiB，3秒100Hz|300次发布/300次接收，零丢失/重复/损坏|
|32 SUB/64B，3秒100Hz|300次发布/9600次接收，零丢失/重复/损坏|

三场景共900次发布、10200次接收。未开启分段诊断或perf；这些是短时功能验证，**不能替代无诊断54窗口正式性能验收**。实际加载库均为本工作区的 `build-integrated/lib/libipc.so.1.6.0`，命令、源码提交、二进制哈希和原始CSV见 [local-smoke/manifest.json](local-smoke/manifest.json)。

首次漏构建CLI的两次失败日志均保留，不声称39项在第一次完整调用中全部通过。补齐工具后只复测失败项，未重复通过项挑选结果。组合版本本轮未重新执行ASan/UBSan、shared-net OFF或原正式矩阵，前期两分支各自的验证不能改称为本组合版本的新成绩。

## 状态

本轮完成了恢复perf取证、内核路径关联、独立代码整合及相关普通回归。原1 SUB/4KiB p50、1 SUB/1MiB p50和32 SUB/64B p99三个正式失败仍开放；尚无经过对照验证的生产延迟候选，因此未用相同实现反复跑原矩阵挑选通过轮次。

下一项性能工作以本组合版本为源码起点，并与原f066a82保持相同基准工装和原门槛。需分别解决4KiB等待波动、广播唤醒累计和1MiB复制执行成本，不能以已通过的功能回归、单轮小消息低延迟或UDP隔离完成代替这三项验收。
