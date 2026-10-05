# T12 故障、关闭和重启

基于 1afee96。修复发送事务异常路径的目标/发布者配额回收，费用随 Tx 析构归还；enqueue 在 shard 队列锁内重新检查 stopped。停止时收敛 shard，再释放重组、发送事件、目录分页、初始化结果和 bridge。保留对象的重复 stop 幂等。

订阅者析构唤醒并等待已进入的 getter，旧 generation 保持 reader/池租约直到读操作结束；取样仅在 generation 仍有效时交给调用者。普通 C++ 对象生命周期规则仍适用：析构开始后调用方不得发起新的成员调用。

新增 8 条 GTest：

- SIGKILL 真实网关后本机 best-effort 继续交付；旧 Sample 全字节合法；reliable 失败且本机仍交付一次。
- 在最后一片、SHM commit、ACK 生成/丢失、路由撤销边界暂停推进；未提交不 ACK，重放不二次提交。
- 关闭 reader 唤醒等待；重启得到新 gateway epoch，旧对象网络身份不可复用，新对象可登记。
- 20 次并发 publish/reset，失败重置不会留下混合描述。
- 同一保活会话下 100 次公共端点创建/销毁，FD、线程、测试话题 SHM 文件集合/尺寸回到预热基线；活跃登记归零。
- 将重组预算配置为 1 MiB 后接纳 1 MiB 未完成记录，stop 返回前缓存和 stream 归零（gateway epoch 已永久关闭，此时可释放历史）。
- 100 次目标准备异常，发送目标费用归零、每发布者额度仍能重复预留；并发 submit/stop 可退出。

此前 T04/T05 的 SIGSTOP 独占锁、fork 旧句柄、SIGKILL 前后旧会话不重放，与 T07/T08 的旧 route/peer epoch、去重历史测试均在本次全回归中运行。

最终共享 CTest 32/32；ASan/UBSan（detect_leaks=1）三个新增测试程序 3/3。日志同目录。

初次测试两处断言已校正并保留事实：撤销路由返回显式 Rejected，原测试误写 Dropped；首次资源基线错误地取在新 runtime 的出站 eventfd 初始化前，导致固定多 1 个 FD，现改为同一会话完成真实发送后取基线。未使用重试取绿或放宽 FD 允许量。
