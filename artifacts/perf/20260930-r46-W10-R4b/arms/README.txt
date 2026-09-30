# t43 五臂复跑（当前库 558f47ed…）
五臂各自独立、互不污染计数语义；每臂前清 /dev/shm（池为全机共享）。

| 臂 | 目标计数 | 造法 | 期望 |
|---|---|---|---|
| A | `chunk_exhausted` | DZFlat 关 + 订阅者**不消费** ⇒ send 腿池空（`kind=no_member_send`），publish **仍返回 true**（降级 64 B 分片 = **已交付**） | 0 → 非 0 |
| B | `chunk_alloc_failed` | B **借样腿** `loan()` 池空 ⇒ 拒绝（**未交付**） | 0 → 非 0 |
| C | `queue_evicted` | `CircularQueue<int>(2)` push 100（**不注册** evict 回调） | 0 → 98 |
| C2 | `queue_evicted`（跨二进制） | 真 pub/sub 队列在 **libipc.so** 内 push，本二进制读计数 | 0 → 非 0（证 ODR 统一） |
| D | `wait_set_full` / `fallback_capacity_full` | `DZIPC_SHM_RECV_WORKERS=1` + 140 订阅 | 原有接线保持 |
| E2 | `wait_token_invalid` | 无效 wait token | 原有接线保持 |

⛔ **A 与 B 是两个不同类别，不得混算**（W09 §5.1）：A 腿的 `chunk_exhausted≠0` 而 `chunk_alloc_failed=0`；
B 腿的 `chunk_alloc_failed≠0` 而 `chunk_exhausted=0` —— 本目录的 BEFORE/AFTER 逐臂印证了这一点。
