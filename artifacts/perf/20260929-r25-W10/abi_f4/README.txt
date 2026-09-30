# F4 ABI 复现：陈旧调用方（旧头）× 新库
# 旧头 = 当前头截掉 t30 追加的 5 个 uint64（scan_rounds…deferred_depth_max）
# 复现①（-O2，无 stack protector 触发）：guard 被越界写坏
  sizeof(old)=136  guard_clobbered=4  rc=0
# 复现②（连续 64 次按值取 stats ⇒ 覆盖堆指针）：3/3 崩溃
  run1/2/3 rc=134（*** stack smashing detected ***）
  gdb: SIGABRT ← __GI_abort ← ...  （与 R1-F4 记录的 rc=139 __libc_free←main 属同一类：by-value 返回槽越界）
# 结论：SIGSEGV/abort 位置取决于优化与栈布局，故症状可不同；判定依据是「by-value 返回 + 尾部追加字段」这一 ABI 形态。
