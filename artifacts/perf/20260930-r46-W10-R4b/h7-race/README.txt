# H7 工装竞态（t43 采集期间发现并修复）

## 现象（修前，本目录 `before_double_free.log`）
```
handshake_confirm confirmed=1000/1000 batch_ms=86 failed=0
registered_count=1000/1000 threads_after_create=34 create_ms=659.2
pool_routes_alive=1000 ...
double free or corruption (!prev)
rc=134（SIGABRT）
```
在 **hotcold 拓扑 × n=1000 × hot-msgs∈{20000,40000}** 上 **2/2 复现**。

## 根因（工装侧，⛔ 非产品缺陷）
`Harness::samples_`（`std::vector<std::string>`）被**两个线程并发 push**：
- `hot_thread` → `publish_route()` → 写 tx 样本；
- 主线程 → `publish_route()`（999 条冷路）或 `drain_once()` → 写 tx/rx 样本。
`std::vector` 非线程安全 ⇒ 堆元数据被破坏 ⇒ glibc 报 `double free or corruption`。

为何 r25 时期没崩：r25 的 hotcold 臂**`publish_route()` 不写样本**（F6 缺陷），
即"少了一条并发写"；t37 修 F6 时让 `publish_route()` 也 `push_back` 样本，
**把潜伏竞态激活**了 ⇒ 属"修一个缺陷时暴露另一个"的典型。

## 修法（t43）
`samples_mtx_` + `push_sample()` 单点写入；三个写点（tx/rx/热路 tx）全部改走该函数。
复跑 **3/3 rc=0**（见 `../scale-capacity/hc-fixed-*.log`）。

## 为何必须留档
这是**工装自身的缺陷**，会让"规模运行失败"被误判成产品问题；按 t37 的 H1–H6 同例登记为 **H7**。
