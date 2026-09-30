# W04 交付：SHM 接入契约复审（证据与复现件）

> 工作包 **W04**（任务 `t5`，attempt `bc681ea7-9ee2-4126-a5cc-633635060a2f`）；负责人：共享层负责人。
> 主交付：`../接口与生命周期.md`（交付目录根级，按任务书指定路径）。本目录只放**证据与复现件**。
> 依据：冻结契约 `ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md` + 勘误 `…errata-d8f45cfa45bb.md`（冲突按勘误）；方案 §4 W04、§10.2–§10.5；W00《基线与门槛.md》。

## 1. 文件清单

| 文件 | 说明 | 复现命令 |
|---|---|---|
| `w04_repro_generation_switch.cpp` | **W04-F2** 复现件：已注册 route 上直接 generation 重建 ⇒ `recv_wait_set::wait` SIGSEGV | `g++ -std=c++17 -O1 -I include -I src -I 3rdparty <本文件> -o build/w04/w04repro -L build/lib -lipc -lpthread -Wl,-rpath,$PWD/build/lib && ./build/w04/w04repro {1\|2\|9}` |
| `w04_repro_matrix.out.txt` | 上述复现矩阵原始输出（mode 1/2/9 × 6 轮，含时间戳与 rc） | — |
| `w04_probe_fruitless_spin.cpp` + `w04_probe_fruitless_spin.out.txt` | **W04-F3**：socket「假就绪兜底」不限制循环速率（实测 1.3 M 次/s）；同 worker 真邻居不饿死 | `./build/w04/w04probe10` |
| `w04_probe_same_thread_remove.cpp` | **W04-F1**：同线程 `remove_route` 会等满 2000 ms 打诊断（头文件承诺「不等待」） | `./build/w04/w04probe_same_thread` |
| `w04_probe_pool_first_start.cpp` | **R-16**：并发 `pool.start(3)` vs `start(9)` ⇒ 恰好一个胜出（3/3） | `./build/w04/w04probe_pool_first_start` |
| `w04_probe_safe_order.cpp` | 安全顺序正例（`remove_route → rebuild → add_route`） | `./build/w04/w04probe9` |
| `w04_probe_token_address_stability.cpp` | 补充：`clear_storage` 后同段名重建，sequence 地址**逐次相同** ⇒ 不能靠「地址未变」侥幸 | `./build/w04/w04probe_token_addr` |
| `w04_probe_extra.out.txt` | 上面四个探针的原始 stdout/stderr 汇总（含库指纹前缀） | — |

> `build/w04/` 下的二进制与输出是**不入库**的运行产物（`build/**` 已被 `.gitignore` 忽略）；本目录的 `.out.txt` 是它们的**持久副本**。
> 复现前请先按主交付 §2 重跑 `cmake -S . -B build && make -C build -j$(nproc)` 并核对 `libipc.so` 指纹（防陈旧 build 假绿）。

## 2. 结论摘要（详版见主交付 §0/§4.4/§7.3）

| 项 | 结论 |
|---|---|
| W04-F2（high） | generation 重建**必须先把 route 从 worker 等待集合同步摘除**；反例 6/6 与 4/6 次 SIGSEGV（栈顶 worker 线程内 `recv_wait_set::wait`），安全顺序 6/6 rc=0 |
| W04-F1（high） | 头文件「同线程 remove 不等待」的承诺在实现里**不存在**（`grep -c "this_thread::get_id"` = 2，仅 stop/dtor）；实测等满 2000 ms |
| W04-F3（medium） | socket 假就绪兜底实际是 1.3 M 次/s 忙转（契约称「有界」不成立）；邻居不饿死 |
| 容量偏斜 | 127 可用 / 128 ⇒ `wait_set_full`；构造性偏斜 128/128 落同一 worker；顺序命名 1000 路 min 28 / max 34 |
| 首次初始化责任方 | 首个成功 `start()` 的调用方；后到者 false 且不改写；模块 `pool.stop` 计数 = 0 |

## 3. 未在本目录覆盖（交 W10/外部门槛）

- 混合进程（SHM pub/sub + SHM ser/cli + socket 同进程）的池实例/线程数核对与逐个销毁；（方案 §10.4）
- 千路端到端「注册成功 = 逐 route 有效收发」矩阵；（§13.2）
- Windows 路径未验证（契约 §8 重申）；
- **非实现者独立评审**（本包不得自证）：签认模板见主交付 §11.4。
