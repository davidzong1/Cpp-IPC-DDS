# 未修缺陷登记(待处理)

> **状态**: 全部**未修**。本文档只登记, 不动代码。
> **编写日期**: 2026-09-17
> **来源**: 2026-09-17 偶发 `SIGSEGV at 0` 排查(修复见
> [shm_defect_fixes.md](shm_defect_fixes.md) §7)过程中发现、但**没有**随那一轮修掉的问题,
> 加上同族扫描后新确认的几处。每条都是"当时判断不该顺手改", 不是"没看见"。
> **与 [shm_defect_fixes.md](shm_defect_fixes.md) 的分工**: 那份是**已修**条目的施工记录
> (每条都有行为判据 + 变异验证); 这份是**未修**条目的登记表 + 修法选项。两份都要各自维护,
> 修完一条就从这里删掉、到那份加一节。
> **怎么用**: 每条写「症状 → 成因(file:line) → 触发条件 → 为什么当时没修 → 修法选项 →
> 判据(怎么算修好了) → 复现方法」。第 6 节是共用的排查手段(分配失败注入 / ASAN 探针 /
> 残池清理), 第 7 节是建议的处理顺序。
> **引用约定**: 本文中单独出现的 **§7** 一律指 [shm_defect_fixes.md](shm_defect_fixes.md) 的
> 第 7 条(2026-09-17 那次偶发 SIGSEGV 的修复记录), 不是本文档自己的第 7 节。

---

## 0. 一览

| # | 问题 | 严重度 | 触发条件 | 状态 |
|---|---|---|---|---|
| 1 | `allocator_wrapper::allocate` 是 `noexcept` 且失败**返回 nullptr** → 任何分配失败都表现为"往地址 0 写"的 SIGSEGV | **高(放大器: 把一切内存问题都变成无诊断崩溃)** | 真实 OOM, 或调用方传入被破坏的巨大长度 | ⬜ 未修 |
| 2 | `ipc::shm::handle` 及同族 pimpl 类: `make()` 返回空后**全线解引用空指针**(`handle` 19 处、`udp` 9 处、三个同步原语各 9 处均无判空) | **高(构造期崩, 无诊断)** | 该尺寸档分配失败 | ⬜ 未修(仅 `buffer` 已加固) |
| 3 | 共享 chunk 池**没有崩溃回收**: 被杀的进程永久占走池槽, 残池污染后续所有进程 | **中高(时好时坏的性能悬崖 + 假回归)** | 任何未归还就死掉的持有者 | ⬜ 未修 |
| 4 | `dzIPC::StartShutdownMonitor` 让**库接管进程退出**: 覆盖应用信号处理器 + detached 线程里 `std::exit(0)` | **中(设计风险, 未实测成缺陷)** | 任何用到 IPC 的进程收到 SIGINT/SIGTERM | ⬜ 未修 |
| 5 | 已登记在别处的未修项(组地址碰撞、端口乘性冲突、DZFlat 默认 OFF 的灰度约束等) | 见各文档 | — | ⬜ 未修(见 §5) |

**严重度排序的理由**: 第 1 条是**放大器** —— 它把"分配失败"这个本来可以报错的状态统一翻译成
"只有一个 ip 的 SIGSEGV"。第 2 条是它在同一族里的第二个现成入口。两条都在**构造/析构路径**上,
调用方接不住。第 3、4 条不影响正确性, 影响的是"排障能不能做"与"行为能不能预期"。

---

## 1. `allocator_wrapper::allocate` 的 `noexcept`-nullptr 语义(放大器)

**症状**

任何一次"分配返回空"都不报错、不抛异常, 而是在**很久之后、别的地方**炸成
`SIGSEGV at 0`(写 NULL, `error 6`, in `libc.so.6`), 内核日志只留一个 ip, 现网完全看不出
"这是分配失败"。§7 那次崩溃里, 它就是把一个 use-after-free 放大成"写 NULL"的那一环。

**成因**

```cpp
// src/libipc/memory/allocator_wrapper.h:78-82
pointer allocate(size_type count) noexcept {
    if (count == 0) return nullptr;
    if (count > this->max_size()) return nullptr;   // ← 标准要求抛 std::length_error
    return static_cast<pointer>(alloc_.alloc(count * sizeof(value_type)));   // ← 标准要求抛 std::bad_alloc
}
```

底层 `pool_alloc::alloc` 是 `static_alloc::alloc` = `std::malloc`(`src/libipc/memory/alloc.h:20-27`),
失败返回空、永不抛。于是 libstdc++ 拿不到异常, 直接 `basic_string::_M_construct` →
`memmove(nullptr, src, huge_len)` → 写地址 0。`ipc::string` / `ipc::unordered_map` 等全部走这个
分配器(`src/libipc/memory/resource.h:30` 的 `allocator` 别名 → `:52` `unordered_map` / `:66` `string`),
所以**任何**用它们的代码路径都具备这个性质。

**触发条件**

- 真实 OOM(分配失败);
- 或者: 调用方传进来的长度已经是垃圾值(§7 的 UAF 就是这种: `_M_string_length` 被 tcache
  的 fd/key 覆盖, 实测 135193289566176)。**这一条尤其重要** —— 它意味着"上游内存被写坏"这个
  错误, 在下游会伪装成"分配失败 → 崩在地址 0", 现场只能看到一个 ip。

**为什么当时没修**

它改的是**全局错误模型**: 把"返回空"改成"抛异常", 影响的是几十处调用方(既有代码大量按
"空指针 = 失败"写)。而且 §7 之后, 唯一已知的真实触发(垃圾长度)已经随 UAF 一起消失了 ——
剩下的只有真 OOM。在"没有异常安全审计"的前提下改它, 风险大于收益。

**修法选项**

1. **按标准抛**(`count > max_size()` → `std::length_error`, 分配失败 → `std::bad_alloc`)。
   本仓**没有** `-fno-exceptions`(CMakeLists 只加 `-O2/-O3/-DNDEBUG`), 所以异常可用。
   代价: 需要逐个审计"构造期间抛出"的路径, 确认不会留下半初始化对象/泄漏。
2. **只在越界那一支抛**(`count > max_size()` → `std::length_error`), 分配失败仍返回空但**打一次
   日志**。这是"低成本的一半修复": 它能覆盖 §7 那种"垃圾长度"的形态(那条路径正是走
   `count > max_size()` 或巨大 size 分配), 而 OOM 仍然只能崩。
3. **不动分配器, 只在入口加闸**: 凡是有 `count` 来自 wire/外部的地方, 先按上限校验再
   `resize`(生成的 TLV 反序列化头里已经有这种闸, 见 shm_defect_fixes.md「一条误报」), 把
   分配器留给"内部可信输入"。

**判据**

- 传一个 `count > max_size()` 进去: 不得再出现"地址 0 上的 SIGSEGV"; 必须能观测到明确的
  错误(异常或日志);
- 正常路径行为不变(现有测试全绿);
- 加一条常驻用例把判据钉住(例如用 `LD_PRELOAD`/链接期 wrap 让 `malloc` 在指定尺寸档返回空,
  断言"抛出/记录"而不是"崩")。

---

## 2. pimpl 类的 `make()` 失败 → 全线空指针解引用(同族六处, 只加固了一处)

**症状**

定向让某个尺寸档的 `malloc` 失败后, 进程崩在 `ipc::shm::handle` 的**析构**里
(`handle::release` → `impl(p_)->id_`), 同样是"只有一个 ip 的 SIGSEGV", 同样没有诊断。

**成因**

`pimpl<T>` 的"不舒服"分支(`sizeof(T) > sizeof(void*)`)是**堆分配**的:

```cpp
// src/libipc/utility/pimpl.h:52-56
template <typename T, typename... P>
IPC_CONSTEXPR_ auto make_impl(P&&... params) -> IsImplUncomfortable<T> {
    return mem::alloc<T>(std::forward<P>(params)...);   // ← 已判空返回 nullptr(见 pool_alloc.h)
}
```

```cpp
// src/libipc/shm.cpp:23-25
handle::handle()
    : p_(p_->make()) {   // ← ① 通过未初始化的 p_ 调静态成员(按标准是 UB, 实际不解引用)
}                        //    ② 分配失败 ⇒ p_ == nullptr, 而这里不检查
```

`mem::alloc<T>` 在 §7 已加"就地构造前判空"的守卫(`include/libipc/pool_alloc.h:87-97`),
所以现在**不再崩在构造**了 —— 代价是 `p_ = nullptr`, 而后续 **19 处** `impl(p_)->...`
全无判空(`src/libipc/shm.cpp`, `impl(p_)` 出现 19 次, 判空 0 处):

```cpp
handle::~handle() {
    release();          // → impl(p_)->id_  ⇒ 读地址 0
    p_->clear();        // → mem::free(nullptr) ⇒ 已安全
}
```

**同族现状**(`p_->make()` + 无判空的 `impl(p_)`):

| 类 | 文件 | `impl(p_)` 代码处数 | 对 `p_` 判空 |
|---|---|---|---|
| `shm::handle` | `src/libipc/shm.cpp:23` | 19 | **0**(其中 6 行带 `nullptr` 的判的是 `id_`, 不是 `p_`) |
| `socket::UDPNode` | `src/libipc/socket/udp.cpp:24` | 9 | **0** |
| `sync::mutex` | `src/libipc/sync/mutex.cpp:27` | 9 | **0** |
| `sync::condition` | `src/libipc/sync/condition.cpp:27` | 9 | **0** |
| `sync::semaphore` | `src/libipc/sync/semaphore.cpp:28` | 9 | **0** |
| `buffer` | `src/libipc/buffer.cpp:37` | 4 | ✅ 全部(`:81-99`, 经局部量 `ip` 判空, §7 已加固) |

`buffer` 是已加固的样板 —— 它的访问器现在统一写成 `auto ip = impl(p_); return (ip == nullptr) ? … : …;`。
其余五个类的入口(尤其是**析构函数**和 `valid()`)应当照此收口。

**触发条件**

对应尺寸档的分配失败(真实 OOM, 或 §1 那种"长度已被写坏"的传导)。两者都需要 OOM 级事件才触发,
与 §7 那个"只需要析构顺序"的 UAF 不同 —— 这也是当时没有一起修的理由: **不能复现的东西很难验证
修好了**, 只能靠注入。

**为什么当时没修**

它是**真实 OOM 才触发**的路径, 而修它要动 5 个类的公共入口语义(尤其 `handle` 的构造函数没有
失败通道)。§7 那一轮的原则是"只修有确定复现路径的缺陷", 于是只就地加固了当天能复现的三处
(`mem::alloc<T>` / `make_cache` / `cache_t::append`)与 `buffer` 的访问器。

**修法选项**

1. **语义可兼容的收口**(推荐): 给 `handle` 这类类定义"失效态"语义 —— `p_ == nullptr` 时
   `valid()` 返回 false, 其余方法安全空转(析构不做事)。这与"`shm::acquire` 失败时 handle 本就
   是 invalid"的现状**一致**, 不新增 API。
   工作量 = 每个类加一个 `impl_or_null` 辅助 + 在公共入口(构造后相关入口 + 析构 + `valid()`)
   判空; 内部函数可以继续假定"已判过", 不必函数内四处铺开。
2. **让 `pimpl::make` 失败时抛 `bad_alloc`**: 一行改完, 但等于把 §1 的错误模型改造提上来
   (构造期间抛出的异常安全要审计), 且会把"分配失败"从"静默降级"变成"terminate"。
3. **消除堆分配**: `pimpl` 的"不舒服"分支之所以存在, 是为了不在头文件里暴露实现大小。
   若某个类可以接受把实现内联进对象(或改成 `std::unique_ptr` + 明确的构造失败通道), 就直接
   没有这条路径了。对 `buffer` 这种被大量复制的小对象不划算, 对 `handle` 值得评估。

**判据**

- 定向注入(§6.1)让该尺寸档分配失败后: 进程**不崩**;
- `handle` 的 `valid()` 返回 false, 析构安全, `acquire/release/detach` 空转;
- 不注入时全部现有测试(`test_shm`、`test_ipc`、`test_dzipc*`、`test_sync`)行为不变;
- 把"注入 + 不崩"固化成一条用例(否则这条修完没法回归)。

---

## 3. 共享 chunk 池没有崩溃回收 → 残池污染后续**所有**进程

**症状**

两类, 都很难反推成因:

1. **性能悬崖**: 大消息本该走 storage 路径(直接借 chunk), 却退化成 64 字节分片 ——
   4MB 消息变成 65536 次 push 打进 256 槽的环。表现是"时好时坏"的吞吐, 不是错误。
2. **假回归**: `test_chunk_hold` 这种"按容量计数"的用例会稳定失败(实测 **20 条只有 10 条**能进环)。
   把§7 修复前的代码换回来在同一个残池上跑, **同样是 10/20** —— 也就是说, 一个**已经修好**的
   东西会看起来像"被新改动搞坏了"。

**成因**

chunk 池的可用位是**共享段里的 free-list**, 且没有任何 owner 信息:

```cpp
// src/libipc/ipc.cpp:258-262
struct chunk_info_t
{
    ipc::id_pool<> pool_;    // ← 位于共享段内, 跨进程共享
    ipc::spin_lock lock_;
};
```

```cpp
// src/libipc/utility/id_pool.h:76-86
storage_id_t acquire() {
    if (empty()) return -1;
    storage_id_t id = cursor_;
    cursor_ = next_[id];     // 从空闲链表里摘出
    return id;
}
bool release(storage_id_t id) {   // ← 唯一的归还路径
    if (id < 0) return false;
    next_[id] = cursor_;
    cursor_ = static_cast<uint_t<8>>(id);
    return true;
}
```

- 容量每尺寸档 **32** (`large_msg_cache = 32`, `include/libipc/def.h:44`);
- **只有"持有者自己归还"这一条路径**把 id 放回池子: `release_storage` / `recycle_storage`
  (`src/libipc/ipc.cpp:461/513`)。池里没有 pid、没有时间戳、没有心跳、没有进程存活检查;
- 所以进程被 `SIGKILL`/`SIGSEGV` 杀掉(或退出前没走完归还), 它占的 id **永久出列**。池段本身
  还活着(还有别的进程在映射), 于是**之后每个进程**看到的都是"少 N 个槽"的残池。

这一条与 §7 是同一场景的两面: §7 那次崩溃(段错误)之后的残池就是它造成的。

**触发条件**

任何"持有 chunk 后没有归还就死掉"的进程 —— 包括所有崩溃、所有被 `kill -9` 的进程。
一旦池被啃到 0, 之后**所有**大消息永久走分片路径(直到有人清掉段)。

**为什么当时没修**

修它要改**共享段的布局**(给 id 加 owner + 存活判定, 或换一种池结构) —— 属于"与旧版本进程
不互通"的改动, 需要和发布节奏一起决策, 与§7 那个"只改一个析构路径"的局部修复不同量级。

**修法选项**

1. **owner + 存活判定**(根治): 每次 `acquire` 记下 owner(pid + 进程启动时间, 或一个"心跳槽"),
   `acquire` 在空闲链为空时顺带扫一遍"owner 已死"的 id 并回收。要求: 判定必须廉价且不能误判
   (pid 复用 → 必须配启动时间或 `/proc/<pid>/stat` 的 starttime)。
2. **显式清理工具**(低成本兜底): 提供一个"确认无活进程映射后清池"的命令(见 §6.3 的手工版),
   并把它写进排障手册。它治不了"生产里静默退化", 但能让测试与开发环境不再被残池坑。
3. **把池做成可重建**: 段名带上"池版本/纪元", 崩过一次就换新段(旧段留给内核回收)。代价是
   内存短时翻倍, 且需要一个"何时判定该换"的信号。

**判据**

- 造一个"占满 chunk 后 `kill -9`"的进程, 再起一个新进程发大消息: 必须走 storage 路径
  (可用 `acquire_storage` 成功率或既有 chunk 计数观测), 不得永久退化;
- 不引入误回收: 正常持有者(长时间持有)的 chunk 不得被别的进程抢走 —— 这是本条的**主要风险**,
  必须有并发压力用例。

---

## 4. `dzIPC::StartShutdownMonitor`: 库接管进程退出(设计风险, 未实测成缺陷)

**事实**

首个 `ServerIPCPtrMake` / `ClientIPCPtrMake` / `PublisherIPCPtrMake` / `SubscriberIPCPtrMake`
都会 `EnsureShutdownMonitorStarted()`, 于是:

```cpp
// src/dzIPC/dzipc.cc:91-113
void StartShutdownMonitor()
{
    std::call_once(shutdown_once, []() {
        std::signal(SIGINT,  SignalHandler);
        std::signal(SIGTERM, SignalHandler);
        shutdown_thread = std::thread([]() {
            while (!shutdown_requested.load(std::memory_order_relaxed)) sleep(100ms);
            dzIPC::logger::StopDzipcLog();
            CleanupIpcInstances();
            std::exit(0);                  // ← 从 detached 线程里结束整个进程
        });
        shutdown_thread.detach();
    });
}
```

**为什么登记它**(如实标注: 以下都是**风险**, 不是已观测到的缺陷)

1. **覆盖率**: 库**覆盖应用自己的** `SIGINT`/`SIGTERM` 处理器, 且**没有 opt-out API**(只有
   `RequestShutdown()` / `IsShutdownRequested()`, 没有"别装处理器")。应用若自带优雅退出逻辑,
   会被这段代码抢先 `std::exit(0)` 掉。
2. **退出码恒 0**: 被 `SIGTERM` 结束的进程也以 0 退出, 上层(systemd / 脚本 / CI)无法区分
   "正常结束"与"被终止", 也拿不到"信号导致"的信息。
3. **非主线程 `std::exit`**: 会在**其他线程仍在运行**时执行静态对象析构(以及 atexit 链)。
   这是数据竞争与析构顺序未定义的经典来源。§7 排查时它一度是首要嫌疑(现象"发生在所有断言
   通过之后的退出期"), 后来由 ASAN 排除了因果关系 —— 但**风险本身没有被证伪**。
4. **`CleanupIpcInstances()` 名不副实**: 它只是 `clear()` 掉四个 `weak_ptr` 容器
   (`src/dzIPC/dzipc.cc:71-89`), 既不 `stop()` 也不销毁 IPC 实例(实例由用户的 `shared_ptr`
   持有) —— 名为 cleanup, 不保证"干净停止"。
5. **处理器安装是懒触发的**: 应用在创建第一个 IPC 对象**之前**设的处理器会被覆盖, 之后设的
   会覆盖库的 —— 最终行为依赖调用顺序, 这是最难解释的一类现象。

**判据(先做实验, 再决定改不改)**

- 两线程程序(一线程建 IPC 后阻塞, 另一线程忙转 + 建/销对象), 发 `SIGTERM`:
  观察是否有退出期崩溃、日志截断、或析构期数据竞争(ASAN/TSAN 各跑一遍);
- 断言"`SIGTERM` 下退出码不是 0"(当前会失败 —— 那就把现状记下来再讨论要不要改);
- 断言"应用自己的 `SIGTERM` 处理器会被调用"(当前会失败)。

**修法选项**

1. **默认不装处理器**: 只提供 `RequestShutdown()`, 由应用决定何时退出(把 `std::exit(0)`
   从库里拿掉)。最干净, 但**改变现有应用的退出行为**(dzviz / dzplot 是否依赖它需要先查)。
2. **保留默认 + 提供关闭开关**(`DisableShutdownMonitor()` / `OnShutdown(callback)`):
   向后兼容, 且给应用接管的机会。`std::exit(0)` 改成"调用回调, 没有再退"。
3. **只修细节**: 退出码用 `128+signo` 区分、`CleanupIpcInstances` 改名或真的停链路、
   统一"先 `RequestShutdown()` 再由主线程退出"。不改语义, 但至少让行为可预期。

---

## 5. 已登记在别处的未修项(不在此重复内容)

| 项 | 位置 | 状态 |
|---|---|---|
| 组播**组地址碰撞**: 8192 topic 下 60% 共组, 默认 `msg_id = 0` 时静默错收 | [shm_defect_fixes.md](shm_defect_fixes.md) §5 | 已实测, 修法已定, **待决策**(改端口公式 ⇒ 与旧版本不互通) |
| 端口公式的乘性冲突(`domain*hash` 可撞同一端口) | [shm_defect_fixes.md](shm_defect_fixes.md) §3 末尾"未处理但已登记" | 未修(归入上一条的重设计) |
| DZFlat 库级默认 **OFF** 的灰度约束: 未升级订阅方会静默丢段, schema 演进从"向前兼容"变"硬同步" | [dzflat_shm.md](dzflat_shm.md) §9 | 设计契约, 未改(现走"应用层选择性开启") |
| 无 per-topic / per-publisher 的 DZFlat 开关(只有进程级 `EnableDzFlat`) | [dzflat_shm.md](dzflat_shm.md) §9 | 未实现(全局开关 + 类型级 + 时序级是今天的替代) |
| 订阅侧**没有**开关(恒双 wire): 无法表达"这个订阅者不要借样" | 同上 | 设计如此, 未实现反向控制 |
| 同机多订阅者共享一个 SHM 入口 | [local_shm_fanout.md](local_shm_fanout.md) | 首行即"设计文档, **未实施**" |
| Python 发布端仍有**一次 memcpy**(段在 Python 地址空间生成) | [dzflat_shm.md](dzflat_shm.md) §9.7 | 真零拷贝写 = 下一个里程碑 |

---

## 6. 复现手段(共用手册)

### 6.1 定向分配失败注入

目的是把"真实 OOM"变成可重复的实验。两个办法:

1. **`LD_PRELOAD` 拦截 `malloc`**: 按尺寸档(或调用计数)返回空。注意要**放过初始化期**的分配,
   否则进程根本起不来; 建议"从第 N 次、尺寸落在 [lo, hi) 的分配开始失败"。
   实测点: `handle_`(pimpl 的"不舒服"分支)与 `conn_info_t`(224 字节, 走 tcache 尺寸类) ——
   两个尺寸档各是一个崩点。
2. **链接期 wrap**: `-Wl,--wrap=malloc` 或直接替换 `libipc` 里的 `pool_alloc::alloc` —— 更可控,
   但需要重建库(见 §6.2 的单建法)。

### 6.2 ASAN 单建 libipc + 探针(拿"因果", 不只是"现象")

不必重 configure 整仓 —— libipc 只有 12 个 TU(注意 `a0_*` 是 C, 要用 `gcc` 先编成 `.o`):

```bash
gcc -fsanitize=address -g -I src -I src/libipc/platform -I src/libipc/platform/linux -I include \
    -c src/libipc/platform/platform.c -o /tmp/platform.o
g++ -std=c++17 -fsanitize=address -g -DLIBIPC_LIBRARY_SHARED_USING__ \
    -I include -I src -I src/libipc/platform -I src/libipc/platform/linux -I . \
    probe.cpp src/libipc/{buffer,ipc,pool_alloc,shm,sniffer}.cpp src/libipc/socket/udp.cpp \
    src/libipc/sync/*.cpp src/libipc/platform/posix/shm_posix.cpp /tmp/platform.o \
    -o probe_asan -lpthread -lrt
```

### 6.3 残池清理(§3 的手工版)

**先确认没有任何活进程映射这些段**, 再删:

```bash
ls /dev/shm | grep CHUNK_INFO                     # 池段: __IPC_SHM__...CHUNK_INFO__<size>
grep -l CHUNK_INFO /proc/*/maps 2>/dev/null       # 有输出 ⇒ 有活进程在用, 不要删
ls /dev/shm/__IPC_SHM__*CHUNK_INFO__* | xargs -r rm -f
```

清理前请先接受一件事: 清完之后, **之前那些"稳定失败"的用例会突然变绿**。那不是修好了,
是环境干净了 —— 反过来, 看到"某用例从绿变红"时也请先查一遍是不是刚崩过进程。

### 6.4 "只有一个 ip 的 SIGSEGV" 三步收敛法

1. **守门人**: 预加载一个 `SIGSEGV`/`SIGBUS` 处理器, 打印寄存器 + `backtrace_symbols_fd`,
   然后循环跑真实复现脚本(单次运行抓不到的是堆运气, 多跑)。
2. **符号化**: `addr2line -f -C -i -e build/lib/libipc.so <偏移>` 把栈落成函数名。
3. **坐实**: §6.2 的 ASAN 探针给出 alloc/free 双方栈 —— 这一步才叫"因果", 前两步只是"位置"。

---

## 7. 建议的处理顺序

1. **§1(放大器)先做**, 因为它让后面所有"分配失败"从"无诊断的崩溃"变成"可观测的错误"。
   最小形态是选项 2(越界抛 + 失败记日志), 不必一次做到全标准语义。
2. **§2(同族空指针收口)** 紧随其后: `buffer` 已有样板, 照抄到 `handle` / `udp` / 三个同步原语;
   判据必须含"注入后不崩"的用例, 否则修完没法回归。
3. **§4(退出语义)** 排第三: 它影响**所有**应用的排障体验(退出码、信号、析构期), 而且是
   先"做实验记录现状"就能推进的一项, 不必等设计定稿。
4. **§3(池回收)** 最后: 它需要改共享段布局, 与"是否与旧版本进程互通"的发布决策绑定;
   在决策之前, 至少把 §6.3 的手工清理写进排障手册(已经写在本文件里)。
5. **§5 里的各项** 按各自文档的结论推进(组地址碰撞是其中最值得优先决策的: 规模相关 + 静默错收)。

---

## 附: 一条读法提醒

本文档每条都区分了**"已实测"**与**"推理"**:

- §1 的症状有实测(§7 的两次内核日志 + ASAN 因果链), 触发条件是推理;
- §2 的崩溃有实测(定向注入), 影响面是推理(只验了 `handle`);
- §3 有实测(10/20 残池), 修法风险的评估是推理;
- §4 **全部是推理**, 只有代码事实(第 91-113 行)是确定的 —— 所以它的第一项工作是做实验,
  而不是改代码。

不要把本文档里的"推理"当成"实测"。§7 那次排查最贵的教训就是: 症状相同的两个缺陷(§1 与 §2)
在**同一个内核签名**下长得一模一样, 而真正的修法在完全不同的地方 —— 只能靠实验区分。
