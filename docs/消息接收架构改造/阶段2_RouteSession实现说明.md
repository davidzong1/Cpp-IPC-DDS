# 阶段 2 · RouteSession 实现说明

> 状态：实现说明，代码未改。供团队落地订阅 route 的生命周期，不包含线程池。
> 配套：`docs/消息接收架构改造/事件驱动线程池需求.md` §4、§8、§9；`docs/消息接收架构改造/shm_sub_thread_consolidation_plan.md` 阶段 2。
> 前置：阶段 1 可以未接入。本阶段仍由现有 `subscribe_thread_` 收包。
> 后置：阶段 4/5 才能把 `recv` 交给别的 worker。本阶段不引入 `recv_wait_set`。

## 0. 要解决的问题

`subscribe_thread_` 在 `channel_mtx_` 里调用 `subscriber_->recv(50)`（`src/dzIPC/shm_pub_sub_ipc.cc` 订阅循环）。握手线程要 `release`/`reset`/`disconnect` 时也拿这把锁（同文件重建、`add_peer` 失败、`connected_id`、未 Ready 分支）。

后果有两条：

1. `shared_ptr<ipc::route>` 拷走也不够。另一线程仍可对同一个对象调用 `release()`，改掉内部句柄。锁是为了挡住这件事，不是为了挡住指针本身。
2. 重建线程会堵在锁上，直到这次 `recv(50)` 返回。控制面若已迁到阶段 1 的单线程调度器，这 50ms 会推迟同进程其它话题的心跳。

本阶段用 `RouteSession` 把「谁可以 `recv`」和「何时允许 `release`」写成协议，然后删掉收包路径上的 `channel_mtx_`。对外的 `get`/`try_get`/`get_clone` 不变。wire 分流代码本阶段不搬（那是阶段 3）。

## 1. 落点

| 文件 | 动作 |
|---|---|
| `include/dzIPC/shm_route_session.h` | 新增 `RouteSession` |
| `src/dzIPC/shm_route_session.cc` | 协议实现。放在 `src/dzIPC/` 下时会被现有 `aux_source_directory` 收编 |
| `include/dzIPC/shm_pub_sub_ipc.h` | `shm_sub_ipc` 增加 `RouteSession` 成员，删除仅服务于收包互斥的 `channel_mtx_` |
| `src/dzIPC/shm_pub_sub_ipc.cc` | 订阅循环改为 lease + `recv`；`sub_handshake()` 里五处 `channel_mtx_` 改为 `begin_rebuild` / `stop_and_wake` |

不改 `libipc` 的 `recv` 实现，不改队列，不改 nodelet 注册键。

## 2. 接口

```cpp
class RouteSession {
public:
    struct ReceiveLease {
        std::shared_ptr<ipc::route> route;
        uint32_t generation{0};
    };

    /* stopping 或 rebuilding 或当前没有 route：返回空。
     * 成功则 receive_inflight_ + 1，返回的 shared_ptr 在 recv 期间保持对象存活。 */
    std::optional<ReceiveLease> acquire_receive();

    /* 每次成功 acquire 必须配对一次，包括 recv 抛出前的所有出口。 */
    void release_receive() noexcept;

    /* 按 §3 的顺序换成新 route。可在已有 route 为空时调用（首次建立）。
     * 返回后 acquire_receive 才能拿到新 generation。 */
    void begin_rebuild(uint32_t new_generation,
                       const std::function<std::shared_ptr<ipc::route>()>& create);

    /* 拒绝新 lease，disconnect/wakeup 当前 route，唤醒卡在 wait_quiescent 的线程。 */
    void stop_and_wake() noexcept;

    /* 等到 receive_inflight_ == 0。stop_and_wake 之后调用。 */
    void wait_quiescent();

    std::shared_ptr<ipc::route> current_route() const;
    uint32_t generation() const;
};
```

内部状态：

```cpp
std::mutex mtx_;
std::condition_variable cv_;
std::shared_ptr<ipc::route> route_;
uint32_t generation_{0};
std::size_t receive_inflight_{0};
bool stopping_{false};
bool rebuilding_{false};
```

`create` 由调用方提供：现有代码是 `std::make_shared<ipc::route>(topic_name_.c_str(), ipc::receiver, verbose_)`。`RouteSession` 不拼段名。

## 3. 收包协议

`subscribe_thread_` 里原来的持锁 `recv` 改成：

```text
lease = acquire_receive()
若无 lease：sleep 与现在未握手时相同（50ms），continue
在不持有 RouteSession 锁的情况下：lease.route->recv(50)
release_receive()          // 无论 raw_data 是否为空
raw_data 为空：continue
否则：沿用现有分流（阶段 3 之后改为调用 process_received_buffer）
```

`recv` 返回的非空 `buff_t` 必须送进现有分流。不得因为 `lease.generation` 在 `recv` 返回后已经不是 session 的当前 generation 就丢掉这块 buffer：字节已经从那条旧 route 弹出，丢掉就是丢消息。generation 只用于阻止**新的** `recv` 和决定何时 `release()` 对象。

`release_receive` 在锁内把 `receive_inflight_` 减一并 `notify_all`。

## 4. 重建协议

`begin_rebuild` 严格按这个顺序，不能并步：

```text
1. 锁内：rebuilding_ = true，禁止新的 acquire_receive
2. 拷出旧 route 的 shared_ptr，放开锁
3. 对旧 route disconnect()（内部会 quit_waiting，卡住的 recv(50) 返回）
4. 锁内：等待 receive_inflight_ == 0
5. 仍在锁内或确认没有 lease 持有旧对象之后：旧 route release()，再 reset 指针
6. 调用 create() 得到新 route
7. generation_ = new_generation，route_ = 新对象，rebuilding_ = false，notify_all
```

第 5 步之前禁止 `release()`。`shared_ptr` 的引用计数只能保证 C++ 对象还在，保证不了 `release()` 与 `recv()` 不并发。

`create()` 失败：保持 `route_` 为空、`rebuilding_ = false`，返回失败。调用方走现有「不置 `handshake_completed`、稍后重试」的路径，不要留下 `rebuilding_ == true`。

握手线程里现在持 `channel_mtx_` 的五处，改完后对应关系：

| 现状 | 改为 |
|---|---|
| Ready 且 generation 变化：锁内 `release` + 新建 `route` | 一次 `begin_rebuild(generation, create)` |
| `add_peer` 失败：锁内 `disconnect` + `reset` | `stop_and_wake` + `wait_quiescent`，然后 `release` 当前 route，不创建新对象 |
| 读 `connected_id()` | `begin_rebuild` 返回之后 `current_route()->connected_id()`。此时没有并发 `release` |
| 控制面离开 Ready：锁内 `disconnect` + `reset` | 与 `add_peer` 失败相同 |

`cc_id == 0`（连接位耗尽）时仍然不置 `handshake_completed`，行为与现在一致。

## 5. 析构顺序

在现有析构（先注销 `LocalPubSubRegistry`，再停线程）上收紧。阶段 5 之前没有收包调度器，第 2 步是空操作，但顺序先写死，避免以后插错。

```text
1. 注销 LocalPubSubRegistry（必须仍在停收包之前，避免 nodelet 快路径写入正在销毁的 msg_queue_）
2. 注销收包调度器（阶段 5 之前：无操作）
3. 注销控制面（阶段 1 未接入时：等价于让 sub_handshake 循环看到 running == false）
4. RouteSession::stop_and_wake()
5. 等待 subscribe_thread_ 退出并 join
6. RouteSession::wait_quiescent()
7. release/reset route
8. 再释放队列
```

`running = false` 只能让循环在 `recv` 返回后退出。卡在 `recv(50)` 里时必须靠第 4 步的 `disconnect`/`quit_waiting` 把它叫醒，不能只靠 50ms 超时。

析构线程不得在持有 `RouteSession` 锁时 `join` 收包线程。收包线程的 `release_receive` 要拿这把锁。

## 6. 并发规则

- `acquire_receive` / `release_receive` / `begin_rebuild` / `stop_and_wake` / `wait_quiescent` 可以来自不同线程。阶段 2 的实际调用方只有 `subscribe_thread_` 和握手线程（或阶段 1 调度线程上的握手回调）。
- `begin_rebuild` 等待 `receive_inflight_ == 0` 时持有 `mtx_`。`release_receive` 必须在同一把锁里减计数并通知，不能先要求重建方放锁。
- `disconnect`/`quit_waiting` 放在锁外，避免 `recv` 的唤醒路径回头再要 `mtx_`。
- 回调里（阶段 1 的控制面 tick）若调用 `begin_rebuild`，允许阻塞到当前 `recv` 结束。这是阶段 2 要消掉的「锁被 `recv(50)` 占住」；改完后阻塞的是 `RouteSession` 自己的等待，上限仍是一次 `recv` 被 `disconnect` 叫醒的时间，而不是整个 50ms 超时再加锁竞争。
- 不在本阶段把 `recv` 换线程。`recv_cache()` 是 `thread_local`，换线程属于阶段 5，而且必须整条 route 固定在同一个 worker。

## 7. 分工

| 项 | 产出 |
|---|---|
| A. `RouteSession` 与单元测试 | 不链接完整握手。覆盖 acquire/release、重建期间 acquire 为空、inflight 未归零时不调用 `release()`、`stop_and_wake` 能结束一次阻塞 `recv` |
| B. 订阅循环改 lease | 只改 `shm_sub_ipc` 收包循环的锁范围，分流函数体不动 |
| C. 握手五处改调用 | `sub_handshake()` 不再使用 `channel_mtx_` |
| D. 析构顺序 | §5。确认 nodelet 注销仍在 `stop_and_wake` 之前 |

A 先合入。B 与 C 一起合入，否则会出现一边已经不持锁、另一边仍对裸 `subscriber_` 调用 `release`。

## 8. 验收

1. 发布/订阅、generation 重建、`add_peer` 失败重试、连接位耗尽（`cc_id == 0`）、控制面离开 Ready，行为与改前一致。
2. 重建与 `recv` 重叠：测试线程卡在 `recv` 时触发 `begin_rebuild`。`release()` 的调用必须发生在 `recv` 返回且 `release_receive` 之后。可以用测试替身计数，不能只看功能测「最终又能收到消息」。
3. 析构时 `recv` 正阻塞：`stop_and_wake` 之后 join 能返回，不依赖 50ms 超时，不访问已析构的 `shm_sub_ipc`。
4. 重建过程中已经弹出的一条消息仍进入 `view_queue_` 或 `msg_queue_`（§3 的不丢包约定）。
5. `LocalPubSubRegistry` 在收包停止前注销。快路径发布端在析构开始后不能再 `push` 到这个 `msg_queue_`。
6. 现有 SHM 收包回归通过。本阶段不要求线程数下降。

## 9. 明确不做

- 不提取 `process_received_buffer`（阶段 3）。
- 不实现 `recv_wait_set`，不把多条 route 放进一个 worker（阶段 4/5）。
- 不把 `recv(50)` 改成无限等待。无限等待要等 `stop_and_wake` 的叫醒在目标平台上测过之后再改。
- 不把用户 `get`/`get_clone` 挪进收包线程。
- 不改发布端 `publish_thread_` / 阶段 1 调度器的心跳周期。
