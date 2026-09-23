# SHM 订阅侧线程合并 · 可执行实现方案

> 状态：方案文档，产品代码未改。状态词表同 `unfixed_defects.md` §0.3.4：⬜未做 / 🔶进行中 / ✅已做 / ⛔不做 / ⏸️待决策。
> 范围：本机 SHM pub/sub。socket/UDP、ser/cli 不在首批实现范围内。
> 目标：在保持收包与用户取数异步、队列契约不变的前提下，把 SHM 控制面和收包侧从按话题扩张改造成进程级调度。

## 0. 结论和设计边界

本方案方向合理，但 P1 不能直接把“每话题线程”改成共享线程池。必须先解决：

1. `ipc::route` 的停止、generation 重建和析构生命周期；
2. `libipc` 的真正跨进程多路等待。

最终形态：

```text
进程内：
  1 个 SHM 控制面调度器
  N 个 SHM 收包 worker（N 默认 CPU 数或配置上限）
  若干应用线程

每个 shm_sub_ipc：
  不再拥有 subscribe_thread_
  不再拥有 sub_handshake_thread_
  持有一个 RouteSession
  向控制面调度器和收包调度器登记
```

必须保持不变：

- `get`、`try_get`、`get_clone`、`try_get_clone` API；
- `view_queue_`、`msg_queue_` 职责；
- DZFlat/TLV 分流、adopt 配额和 chunk 生命周期；
- nodelet 快路径直接投递 `msg_queue_`；
- 单 route 顺序和多片消息重组；
- 关闭、generation 重建、死连接回收语义。

## 1. 现状和关键约束

当前每个 `shm_sub_ipc` 创建：

- `subscribe_thread_`：`ipc::route::recv`、wire 分流、队列投递；
- `sub_handshake_thread_`：握手、peer 心跳、generation 重连。

每个 `shm_pub_ipc` 另有 `publish_thread_` 做 owner heartbeat 和死订阅回收。

当前收包路径在 `channel_mtx_` 内执行 `subscriber_->recv(50)`。这避免 route 被握手线程并发 `release/reset`，但会让重连等待最多约 50ms。

当前析构顺序是先 `running=false`、等待收包线程退出，最后才 `subscriber_->disconnect()`。将 `recv(50)` 改成长等待时，必须先 wakeup 再 join，否则可能永久阻塞。

`libipc::conn_info_head::recv_cache()` 使用 `thread_local` 分片缓存。因此同一 route 不能在 worker 之间随机迁移，否则多片消息的前后片段可能进入不同缓存而重组失败。

## 2. 规模估算

以 1000 个 SHM 订阅为例：

| 线程类型 | 空闲周期 | 约唤醒次数/秒 | 1000 话题条数 |
|---|---:|---:|---:|
| `subscribe_thread_` | `recv(50)` 超时 | ~2 万 | 1000 |
| `sub_handshake_thread_` | `sleep(10ms)` | ~10 万 | 1000 |
| `publish_thread_`（另有等量发布者时） | `sleep(50ms)` | ~2 万 | 1000 |

线程总数应区分：

- 1000 个订阅者：约 2000 条 SHM 线程；
- 1000 个订阅者 + 1000 个发布者：约 3000 条 SHM 线程；
- 不含应用线程和其他库线程。

首要瓶颈是空闲唤醒、上下文切换、缓存抖动和消息到来时的调度竞争；内存通常不是第一瓶颈。

## 3. 不采用的方案

- ⛔ 把 `get`、`get_clone` 或用户回调搬进收包线程；
- ⛔ 没有真正多路等待时，将 1000 路 `try_recv()` 轮询作为产品默认；
- ⛔ 默认给共享 worker 全部设置 `SCHED_FIFO`；
- ⛔ 仅复制 `shared_ptr<ipc::route>` 就移除 route 生命周期同步；
- ⛔ 每条 route 建辅助线程等待 `rd_waiter_` 再写 `eventfd`，这会重新引入 O(话题数) 线程；
- ⛔ 首批同时改造 socket/UDP、ser/cli 线程模型。

## 4. 分阶段实现

### 阶段 0：基线和观测 ⬜

不改变行为，量测：

- 订阅数量 1、100、1000；
- 1000 个空闲订阅；
- 1 个热话题 + 999 个冷话题；
- 大消息、多片消息；
- generation 重建；
- 订阅者关闭；
- nodelet 快路径；
- 多进程发布/订阅。

记录线程数、`nvcsw`、`nivcsw`、CPU、收包 p50/p99、关闭延迟、丢失/重复/乱序，以及 chunk/adopt 占用和归还。结果作为后续回归基线。

### 阶段 1：进程级控制面调度器 ⬜

新增 `ShmControlScheduler`，替代订阅和发布握手线程，暂不改变数据面：

```cpp
class ShmControlScheduler {
public:
    using EntryId = uint64_t;
    EntryId register_subscriber(std::shared_ptr<SubControlState>);
    EntryId register_publisher(std::shared_ptr<PubControlState>);
    void unregister(EntryId);
};
```

注册项使用 RAII 生命周期令牌，调度器不得保存裸 `this`。注销必须同步完成：

```text
标记 inactive -> 唤醒调度器 -> 等待正在执行的 tick -> 返回
```

第一版保持现有时间语义：

- 订阅 heartbeat：10ms；
- 发布 owner heartbeat/stale 扫描：50ms；
- peer 死连接判定：2s。

第一版的收益是减少线程唤醒和上下文切换；heartbeat 写入总量仍可能为 O(N)，需要记录扫描耗时和最大抖动。发布端即使没有 peer，也不能停止 owner heartbeat；可仅在 `peer_count() > 0` 时跳过 stale 扫描。

验收：

- 订阅握手线程从 O(N) 降为 0；
- 发布握手线程从 O(N) 降为 0；
- 控制面、generation 重建和 stale peer 回归通过；
- 注销后不再回调已析构对象。

### 阶段 2：RouteSession 生命周期协议 ⬜

在移除 `channel_mtx_` 外层等待前，为每个订阅 route 引入生命周期封装：

```cpp
class RouteSession {
public:
    struct ReceiveLease {
        std::shared_ptr<ipc::route> route;
        uint32_t generation;
    };

    std::optional<ReceiveLease> acquire_receive();
    void begin_rebuild(uint32_t new_generation);
    void stop_and_wake();
    void wait_quiescent();
};
```

内部至少维护：

```cpp
std::mutex mtx_;
std::condition_variable cv_;
std::shared_ptr<ipc::route> route_;
uint32_t generation_{0};
size_t receive_inflight_{0};
bool stopping_{false};
bool rebuilding_{false};
```

收包协议：

```text
acquire_receive()
  -> receive_inflight_++
  -> 取得 route lease
  -> 解锁
  -> route->recv()
  -> receive_inflight_--
```

generation 重建：

```text
禁止新的 receive lease
  -> 对旧 route disconnect()/wakeup
  -> 等待 receive_inflight_ == 0
  -> release/reset 旧 route
  -> 创建新 route
  -> 发布新的 generation
```

不能仅依赖 `shared_ptr`：它不能阻止另一线程对同一个 route 执行 `release()` 或修改内部句柄。

析构顺序：

```text
1. 从 LocalPubSubRegistry 注销
2. 从收包调度器注销
3. 从控制面调度器注销
4. 标记 RouteSession stopping
5. disconnect/wakeup 当前 recv
6. 等待 worker 不再使用 route
7. release/reset route
8. 释放队列和其他资源
```

### 阶段 3：提取收包分流逻辑 ⬜

将现有订阅 lambda 中从 `buff_t raw_data` 开始的代码提取为：

```cpp
void process_received_buffer(
    const std::shared_ptr<SubState>& state,
    ipc::buff_t&& raw_data);
```

继续复用现有 DZFlat/TLV 判断、msg_id/schema 校验、`view_queue_`、`msg_queue_`、`AcceptWire()`、adopt 配额、evict 回调和统计逻辑。本阶段只改变调用者，不改变解码和队列语义。

### 阶段 4：真正的跨进程多路等待 ⬜

现有 `rd_waiter_` 是共享内存条件变量等待器，不是 fd。发布进程不能直接写订阅进程创建的 eventfd，因此 `eventfd/epoll` 不能直接作为默认方案；否则需要代理线程，又会回到 O(N) 线程。

建议在 `libipc` 增加：

```cpp
class recv_wait_token {
public:
    bool valid() const;
};

class recv_wait_set {
public:
    void add(const recv_wait_token&);
    void remove(const recv_wait_token&);
    bool wait(std::chrono::milliseconds timeout);
    std::vector<recv_wait_token> consume_ready();
};
```

`ipc::route` 暴露读等待 token。Linux 首选基于共享内存 sequence 和 `futex_waitv` 或等价机制实现，并明确内核版本要求。

wait-set 必须保证：

- 消息到达和断开均可唤醒；
- generation 切换不丢 wakeup；
- 注册/注销与等待无竞态；
- 支持 level-triggered 重新检查；
- 返回后可定位 ready route。

不支持真正多路等待的平台必须回退原有每 route 线程模型或显式报告不支持，不能静默退化成高 CPU 轮询。

### 阶段 5：固定 route 归属的收包线程池 ⬜

只有阶段 2、4 完成后才实现 worker pool。

一个 route 在生命周期内固定绑定一个 worker，不能随机迁移。建议初始分配：

```text
worker_id = hash(route_name) % worker_count
```

worker 循环：

```text
wait_set.wait()
  -> 获取 ready route
  -> 按 route 限制消息数、字节数和处理时间
  -> route->recv()
  -> process_received_buffer()
  -> 返回 wait_set
```

每个 route 至少设置：

- `max_messages_per_route`；
- `max_bytes_per_route`；
- `max_processing_time_per_route`。

这样避免热话题长期占用 worker。默认 worker 使用普通调度；需要绑核/FIFO 的话题提供 opt-in 独占收包线程逃逸通道。

### 阶段 6：可选单线程统一收包 ⏸️

仅在 wait-set 可用且产品明确需要一个统一 spin 时实施。仍必须保留 route 单消费者、消息顺序、多片重组和单话题 decode 预算。如果重解码会造成全局队头阻塞，应明确该模式不适用于重负载话题。

## 5. nodelet 快路径约束

`LocalPubSubRegistry` 不纳入 SHM 收包 wait-set。

初始化顺序：

```text
1. 构造队列和 RouteSession
2. 注册 LocalPubSubRegistry
3. 注册控制面调度器
4. 注册收包调度器
```

析构顺序：

```text
1. 注销 LocalPubSubRegistry
2. 注销收包调度器
3. 注销控制面调度器
4. 停止 RouteSession
```

这样可避免发布端快路径继续向销毁中的 `msg_queue_` 投递。

## 6. 风险和未决事项

以下未裁决前，P1 不得合入主路径：

- wait-set 采用 `futex_waitv`、其他平台机制，还是保留每 route 线程回退；
- route 固定 worker 分配及 worker 数量；
- 分片缓存保持线程局部，还是迁移到 route 状态；
- RouteSession stop/wakeup/rebuild 顺序；
- 扫描周期、最大抖动与 `kPeerDeadTimeoutNs` 的关系；
- 独占收包线程逃逸通道是否向用户暴露；
- worker 的消息数、字节数和时间预算；
- scheduler 注册令牌和析构同步；
- nodelet 登记与 route 生命周期的并发边界。

## 7. 推荐合入顺序

```text
1. 阶段 0：观测指标和 1000 话题基线
2. 阶段 1：ShmControlScheduler，替代握手线程
3. 阶段 2：RouteSession 生命周期和关闭唤醒
4. 阶段 3：提取 process_received_buffer()
5. 阶段 4：libipc recv_wait_set
6. 阶段 5：固定 route 的收包 worker pool
7. 阶段 5：实时话题独占收包线程逃逸通道
8. 阶段 6：可选单线程 spin 模式
```

每一阶段都应可独立回滚。

## 8. 最低验收标准

- 1000 个订阅空闲时，线程数不再按话题数线性增长；
- 1000 个订阅 + 1000 个发布者时，发布端握手线程也不再按话题数增长；
- 关闭延迟在预算内，且不存在永久等待；
- generation 重建无不可解释的丢包、重复或乱序；
- 多片大消息在 worker 模式下完整重组；
- 单 route 消息顺序保持；
- 热话题不会长期饿死冷话题；
- DZFlat 借样、adopt 配额、队列驱逐和 chunk 归还不变；
- nodelet 快路径不变；
- 多进程 SHM 场景通过；
- 不支持多路等待的平台不会静默退化成高 CPU 轮询；
- 相对阶段 0 基线，线程数、上下文切换和空闲 CPU 有可复现改善。

闭环标准：1000 话题空闲时调度税可接受，对外取数 API、队列契约、DZFlat/TLV 分流、配额语义、握手和死连接回收均保持正确。
