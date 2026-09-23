# 阶段 4 · 跨平台 recv wait-set 实现说明

> 状态：实现说明，代码未改。供团队按后端分工落地。
> 配套：`docs/消息接收架构改造/事件驱动线程池需求.md` §5、`docs/消息接收架构改造/shm_sub_thread_consolidation_plan.md` 阶段 4。
> 阶段 5 的固定分片收包池只有本阶段的 wait-set 通过后才能打开。

## 0. 要做什么

一条收包线程要同时等很多条 route 的「有数据 / 断开」。现在每条 route 的 `rd_waiter_` 只能单路 `wait_if`（`src/libipc/waiter.h`），所以只能每条 route 一条 `subscribe_thread_`。

本阶段在 `libipc` 增加跨进程的 `recv_wait_set`。上层只调用这一套接口。Linux 用 `futex_waitv`，Windows 用 `WaitForMultipleObjects`。两边的唤醒语义必须一样。

不在本阶段做：ReceiveWorker 线程池、改 `subscribe_thread_` 的调用点、把 `rd_waiter_` 换成 `eventfd`/`epoll`/`libevent`。

## 1. 共同接口

建议落在 `include/libipc/recv_wait_set.h`，平台实现放在 `src/libipc/platform/linux/` 与 `src/libipc/platform/win/`。`ReceiveWorker` 不得 `#ifdef` 系统调用。

```cpp
class recv_wait_token {
public:
    bool valid() const noexcept;
};

class recv_wait_set {
public:
    /* 失败返回 false，不改变已有集合。同一 token 重复 add 是幂等成功。 */
    bool add(const recv_wait_token&);
    /* 未注册的 token：幂等成功。正在 wait 的集合里 remove 必须唤醒 wait，
     * 返回后该 token 不再出现在 consume_ready()。 */
    bool remove(const recv_wait_token&);
    /* true：至少一路就绪，或被 stop/remove 唤醒后调用方应 consume_ready。
     * false：超时。超时不是错误。
     * 系统调用失败：记日志并返回 false，调用方按「该后端不可用」处理，不得忙等。 */
    bool wait(std::chrono::milliseconds timeout);
    std::vector<recv_wait_token> consume_ready();
    /* 析构/关闭 worker 时调用。阻塞中的 wait 必须返回。 */
    void stop() noexcept;
};
```

`ipc::route` 增加 `recv_wait_token read_wait_token() const`。token 只标识「这条 route 的读等待字」，不拥有 route 生命周期。route 销毁前必须先 `remove`。

### 1.1 两边都必须满足

| 不变量 | 含义 |
|---|---|
| 消息到达能唤醒 | 发布端现有 `rd_waiter_.notify/broadcast` 路径（`src/libipc/ipc.cpp` 写完队列之后）必须同时敲醒 wait-set |
| 断开能唤醒 | `disconnect` / `quit_waiting` 同样敲醒 |
| generation 不丢 wakeup | 发布端先改共享 sequence，再敲内核对象。等待端以 sequence 为准，内核对象只是提示 |
| add/remove 与 wait 无竞态 | `remove` 或 `stop` 发生在 `wait` 内部时，`wait` 必须返回，不能睡过注销 |
| level-triggered | `wait` 返回后扫描 sequence。已经变化但这次没被内核点名的 route 也要出现在 `consume_ready()` |
| 能定位 route | token 能回到注册它的 route，不能只返回「有人醒了」 |
| 注销后无悬挂 | `remove` 返回后不再回调该 token；共享字可以还在，token 不能再进集合 |

一条 route 同一时刻只允许一个消费者：要么兼容后端的 `subscribe_thread_` 在 `recv()` 里等，要么进某个 `recv_wait_set`。禁止两路同时 `recv`。

### 1.2 共享 sequence

复用 `waiter::state_t::seq`（`src/libipc/waiter.h`，已在共享内存）。`wake()` 今天已经 `seq.fetch_add(1)` 再 `notify/broadcast` 条件变量。wait-set **另加**一次平台唤醒，不替换现有条件变量：兼容后端的单路 `recv` 还靠它。

等待端协议（两个平台相同）：

```text
记下每路 token 的 last_seq
wait 前先扫一遍：seq != last_seq 的直接算就绪，不必睡觉
睡觉时把「当前 seq」交给内核（值已经变了就立刻返回）
醒来后再扫全部 seq，收集就绪集合
consume_ready 把这些 token 交给调用方，并记下新的 last_seq
```

内核对象（futex 或 Event）丢唤醒或合并唤醒都允许，因为 sequence 才是事实来源。

## 2. 能力选择

进程启动或第一次创建 `recv_wait_set` 时探测一次，结果进程内缓存。

| 平台 | 探测 | 选用 |
|---|---|---|
| Linux，`futex_waitv` 可用 | 对一个本地 `uint32` 做一次 0 超时 `futex_waitv`，`ENOSYS`/`EINVAL` 视为不可用 | Linux 后端 |
| Linux，不可用（内核 < 5.16 或 syscall 号不对） | 同上 | **不启用线程池**。显式日志「wait-set 不可用，保持每 route 收包线程」。禁止改成 `try_recv` 轮询 |
| Windows | 始终有 `WaitForMultipleObjects` | Windows 后端 |
| 其它 | — | 同 Linux 不可用：显式不支持 + 每 route 线程 |

探测失败不得在运行中来回切换后端。

## 3. Linux：`futex_waitv`

### 3.1 内核与调用方式

- 最低内核：**5.16**（引入 `futex_waitv`）。实现里写明这个版本，并用 §2 的探测兜底，不要只靠 `#ifdef`。
- 一次调用上限：**128** 路（`FUTEX_WAITV_MAX`）。一个 `recv_wait_set` 超过 128 条 route 时 `add` 失败，由阶段 5 把 route 分到别的 worker。不要在一次 `wait` 里循环打多次 `futex_waitv` 假装支持无限路——空闲时会变成多段睡眠。
- glibc 不一定有包装函数。用 `syscall(__NR_futex_waitv, ...)`，头文件用 `<linux/futex.h>`。架构相关的 syscall 号走内核头，不要在业务代码里写死 `449`。
- **禁止** `FUTEX2_PRIVATE`。sequence 在共享映射里，发布进程和订阅进程的虚拟地址不同，内核按物理页匹配。`FUTEX2_PRIVATE` 会让对端 `FUTEX_WAKE` 唤醒不了。
- 字宽：`FUTEX2_SIZE_U32`，地址按 4 字节对齐。`state_t::seq` 已是 `atomic<uint32_t>`，映射基址保持自然对齐。

### 3.2 等待

```text
futex_waitv 的每一项：
  uaddr = 本进程映射里的 seq 地址
  val   = 调用前读到的 seq
  flags = FUTEX2_SIZE_U32
```

返回值：

| 结果 | 处理 |
|---|---|
| `>= 0` | 该下标被唤醒。仍要扫完全集（其它路可能也变了） |
| `EAGAIN` | 调用进入内核前已有一路 `*uaddr != val`。当作就绪，扫 sequence，**不是错误** |
| `ETIMEDOUT` | `wait` 返回 false |
| `EINTR` | 未到期且未 `stop` 则重试；已 `stop` 则返回 true 让调用方 `consume_ready` |
| `ENOSYS` | 探测阶段就该排除。若运行中出现：记一次错误，本进程以后走兼容后端 |

### 3.3 唤醒

在 `waiter::wake()` 里，`seq.fetch_add(1, release)` 之后增加：

```text
syscall(SYS_futex, &st->seq, FUTEX_WAKE, /*全部等待者*/ INT_MAX, ...)
```

同样禁止 `FUTEX_PRIVATE_FLAG`。没有 wait-set 等待者时，这次 `FUTEX_WAKE` 立即返回 0，开销是一次系统调用。阶段 4 接受这个代价；若测量证明空闲发布被它拖慢，再加「有 wait-set 引用才 WAKE」的计数，协议不变。

`quit_waiting()` / `broadcast()` 走同一条 `wake(true)`，断开因此能唤醒 wait-set。

### 3.4 `stop` / `remove`

集合内放一个**进程私有**的 `uint32` 停止字（不进共享内存）。`stop()` 和 `remove()` 把它加一后 `FUTEX_WAKE`。`wait` 把停止字放进 `futex_waitv` 数组的最后一项。这样注销不依赖「碰巧有消息」。

`remove` 与 `wait` 的锁序：

```text
wait 复制当前 token 列表和 seq 快照，然后放开注册锁再进 futex_waitv
remove 在注册锁内摘掉 token，再敲停止字
醒来后重新拿注册锁，丢掉已经 remove 的 token，再扫 seq
```

停止字被敲醒但 sequence 都没变：`consume_ready()` 为空，`wait` 仍返回 true（调用方区分「超时」和「被打断」靠返回值；空 ready 列表表示没有消息）。

## 4. Windows：`WaitForMultipleObjects`

Windows 没有 `futex_waitv`。`WaitOnAddress` 的文档范围是同一进程的线程，**不能**拿来等另一进程映射的 sequence。不要用它做跨进程 wait-set。

### 4.1 内核对象

每条 route 在现有 waiter 打开时额外创建一个**命名、手动重置** Event（`CreateEvent`，`bManualReset = TRUE`）。名字跟现有段名走，例如在 `rd_waiter_` 的名字后加 `_WAITER_EVT_`，保证发布进程 `OpenEvent` 能敲到订阅侧正在等的同一个对象。

`waiter::wake()` 在 `seq.fetch_add` 之后 `SetEvent`。现有信号量条件变量（`src/libipc/platform/win/condition.h`）保持不动，兼容后端继续 `SignalObjectAndWait`。

手动重置而不是自动重置：一次 `SetEvent` 要让「扫描全部 sequence」发生，不能被某一次 `WaitForMultipleObjects` 吃掉后丢掉同轮其它路的提示。扫描完、确认集合里每一路的 `last_seq` 都已追上之后，对仍属于本集合的 Event `ResetEvent`。扫描和 Reset 之间若 sequence 又变了，不 Reset，立刻当作下一轮就绪。

### 4.2 64 路上限

`WaitForMultipleObjects` 最多 64 个句柄（`MAXIMUM_WAIT_OBJECTS`）。其中一个留给本集合的停止 Event（自动重置即可，只用于打断 `wait`）。因此 **一个 wait-set 最多 63 条 route**。`add` 超过则失败。

阶段 5 在 Windows 上把 worker 的 route 上限配成 63，多出来的 route 进下一个 worker。不要为每条 route 造代理线程去合并 Event。那是 O(话题数) 线程，本阶段明确排除。

`stop()` / `remove()`：`SetEvent(停止 Event)`。`wait` 的句柄数组第 0 项固定是停止 Event，其余是各 route 的手动重置 Event。

### 4.3 等待返回后

`WaitForMultipleObjects` 只给出一个最低下标（或超时、`WAIT_FAILED`、`WAIT_ABANDONED`）。处理：

| 结果 | 处理 |
|---|---|
| `WAIT_OBJECT_0` 落在停止 Event | 重新读注册表，丢掉已 remove 的 token，再按 sequence 扫剩余路 |
| `WAIT_OBJECT_0 + i` | 同样扫**全部** sequence，不只处理下标 i |
| `WAIT_TIMEOUT` | 返回 false |
| `WAIT_FAILED` | 记 `GetLastError`，返回 false，不得紧循环重试 |
| `WAIT_ABANDONED` | 记日志。Event 不用 mutex，正常路径不应出现；出现则视为该后端故障，回退兼容后端 |

`wait` 进入内核前先做与 Linux 相同的 sequence 预扫描，避免 Event 已经 Reset、但 sequence 还没被消费的窗口被漏掉。

### 4.4 句柄所有权

发布进程只 `OpenEvent` + `SetEvent`，不 `CloseHandle` 掉「最后一个句柄」导致对象消失——谁 `CreateEvent` 谁负责在 `waiter::close()` 里 `CloseHandle`。引用计数沿用现有 shm handle 的 `ref()`：最后一个引用 `CloseHandle` 并可以 `SetEvent` 名字清理策略与现有 `clear_storage` 对齐（命名 Event 没有单独的 shm 段，关闭全部句柄即销毁）。

跨进程打开失败：`wake()` 记一次警告并返回，sequence 已经递增。订阅侧若 Event 还没创建，下一次 `wait` 的预扫描仍能看见 sequence。允许短暂漏掉内核唤醒，不允许漏掉 sequence。

## 5. 建议分工

| 项 | 产出 | 依赖 |
|---|---|---|
| A. 接口 + token 从 `route` 取出 | `recv_wait_set.h`、`route::read_wait_token()`，内部先能拿到 `state_t::seq` 的本进程地址 | 无 |
| B. `waiter::wake()` 增加平台敲醒 | Linux `FUTEX_WAKE`；Windows `SetEvent`。单路 `recv` 行为不变 | A 的共享字约定 |
| C. Linux `recv_wait_set` | §3 | A、B |
| D. Windows `recv_wait_set` | §4 | A、B |
| E. 探测与日志 | §2，不可用时一行明确日志 | C 或 D |
| F. 测试 | §6 | 对应后端 |

A、B 先合入。C 与 D 可以并行，测试用同一份用例（§6），平台宏只包实现文件。

## 6. 验收

两个后端同一组行为测试：

1. 2 条 route，只给其中一条发消息：`wait` 返回，`consume_ready` 只有这一条。
2. 两条同时有数据：一次 `wait` 之后 `consume_ready` **两条都在**（level-triggered），不能只剩下内核点名的那条。
3. 先改 sequence 再 `wait`：不得睡满超时（预扫描 / `EAGAIN`）。
4. `wait` 阻塞中 `remove` 其中一条：`wait` 返回，ready 列表不含它；另一条有数据时仍能返回。
5. `wait` 阻塞中 `stop`：`wait` 返回，不访问已销毁 route。
6. 发布端 `disconnect`：等待方被唤醒。
7. 64（Windows）或 128（Linux）加第 1 条：`add` 失败，已在集合内的 route 仍正常。
8. 与兼容后端互斥：同一 route 已在 wait-set 中时，不再有第二条线程阻塞在它的 `recv()` 上。本阶段测试可以只起 wait-set，不接 `subscribe_thread_`。
9. Linux 在内核不支持时：探测失败，日志可见，进程仍能用原每 route 线程收包。

跨进程：发布进程、订阅进程分开。同进程测试不能代替第 1–6 条。

## 7. 明确不做

- 不把 `rd_waiter_` 收成订阅进程里的 `eventfd`，再让每条 route 一个代理线程去写它。
- 不用 `try_recv` 轮询当任一平台的降级。
- 不用 `WaitOnAddress` 做跨进程等待。
- 不在本阶段实现 `ReceiveWorker`、route 哈希分片和批处理预算。那些属于阶段 5，只消费本接口。
- 不删除现有条件变量。单路 `recv(50)` 在 wait-set 未启用时必须保持原样。
