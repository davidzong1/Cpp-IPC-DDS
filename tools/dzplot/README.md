# dzplot — dzIPC Time-Series Visualization Dashboard

Web-based time-series visualization for dzIPC topic data. Supports offline `.bag` file playback and online topic sniffing.

## Quick Start

```bash
# Serve a .bag file replay
python3 tools/dzplot/main.py --bag /path/to/events.bag --port 8766

# Sniff live topics
python3 tools/dzplot/main.py --sniff --topic /test:StdRawMessage --transport shm

# Browser: open http://127.0.0.1:8766
```

## Features

| Feature | Description |
|---------|-------------|
| **Offline .bag replay** | Parse ROS bag v2.0 files, extract topic/message data, replay with timing |
| **Online sniffer** | Subscribe to live dzIPC topics via SHM or socket transport |
| **Time-series charts** | Canvas 2D charts with auto-scale, select fields per topic |
| **FPS control** | Left-side slider, configurable 1–120 fps (default 60) |
| **Batch frames** | Multiple samples aggregated into single WebSocket frame |
| **Adaptive backpressure** | Bounded queue with multi-zone watermark (>50% warn, >75% heavy, >90% emergency); progressive subsampling with hysteresis recovery |
| **Dynamic rendering** | Charts render on-demand; only active fields consume resources |
| **DZFlat / 旁路借样** | 订阅腿与嗅探腿都识别 DZFlat 平坦段(含 SHM 借样段), 按生成的 schema 解码; 样本带 `wire` / `dzflat_borrowed` 便于确认借样是否生效 |

## Architecture

```
Browser (Canvas 2D charts)
  ↕ WebSocket (JSON frames, batched)
main.py (asyncio server)
  ├── PlotHub (client management, command dispatch)
  ├── BagReplaySource (offline .bag → BoundedPubQueue)
  ├── LiveSniffSource (online dzIPC → BoundedPubQueue)
  └── Shared modules:
      ├── transport/bounded_queue.py  (BoundedPubQueue)
      ├── component/rate_controller.py (RateController)
      └── transport/backpressure.py   (BackpressureController)
```

## CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--host` | 127.0.0.1 | Listen address |
| `--port` | 8766 | Listen port |
| `--bag` | none | Path to .bag file for offline replay |
| `--bag-speed` | 1.0 | Replay speed multiplier |
| `--bag-loop` | false | Loop bag replay |
| `--sniff` | false | Enable online sniffer mode |
| `--topic` | none | Topic:MsgType for sniffer (repeatable) |
| `--transport` | shm | Sniffer transport (shm/socket) |
| `--domain` | 0 | Domain ID |
| `--fps` | 60 | Max broadcast FPS (1–120) |

## Backpressure Design

Reuses shared modules from `tools/dzviz/`:

- **BoundedPubQueue** (`transport/bounded_queue.py`): Thread-safe capacity-bounded queue with multi-zone watermark tracking (normal <50%, warn 50–75%, heavy 75–90%, emergency >90%)
- **RateController** (`component/rate_controller.py`): Dynamic poll interval adjustment with hysteresis (α_down=0.8, α_up=0.9, confirm_ticks=3)
- **BackpressureController** (`transport/backpressure.py`): Per-zone downsampling policies

Input cap: 1000 Hz. Output cap: configurable (default 60 fps). Watermark >50% triggers progressive backpressure; drops below 25% triggers recovery.

## Tests

四个入口, 按**是否需要 dzipc 绑定**分两类 —— 绑定是 CPython **3.10** 的构建产物
(`_dzipc_core.cpython-310-*.so`, 且 `*.so` 不入库), 在默认 `python3`(常为 3.12)下
`import dzipc` 必失败。**解释器不能随手指**:

| 脚本 | 解释器 | 退出码 | 查什么 |
|------|--------|--------|--------|
| `test/test_dzplot.py` | 任意 CPython3 | 0/1 | 静态规则 + 纯 Python 逻辑(自带 runner, 不需要 pytest) |
| `test/verify_segment_naming.py` | **3.10** | 0/1/2 | 推导出的段名是不是传输层**真建出的那一个** |
| `test/verify_runtime_no_garbage.py` | **3.10** | 0/1/2 | 运行时有没有在 `/dev/shm` 多建段 |
| `test/integration_pub_restart.py` | **3.10** | 0/1/2 | 发布端重启 → generation 变化 → 重挂 |

```bash
# 无绑定层(任意 python3)
python3 tools/dzplot/test/test_dzplot.py

# 需绑定层(必须 3.10; 绑定未构建先 python3.10 -m pip install ./python)
python3.10 tools/dzplot/test/verify_segment_naming.py
python3.10 tools/dzplot/test/verify_runtime_no_garbage.py
python3.10 tools/dzplot/test/integration_pub_restart.py
```

后三者共用退出码约定 **0=全过 / 1=有失败 / 2=缺 dzipc 绑定(跳过)**。
⛔ **`2` 在 CI 里必须按失败处理** —— `SKIP` 和 `PASS` 在 CI 面板上一样是绿的,
当成通过就等于门是摆设。

一次跑全部(失败即停, 并做绑定可导入前置检查):

```bash
bash scripts/ci_check.sh              # 全量
bash scripts/ci_check.sh --no-binding # 只跑无绑定层
```

⚠️ 后三条会起**真实 SHM 段**, 不可并行 —— 同机同时跑两个, 或一边跑它们一边跑
C++ gtest, 会互相干扰出假红。详见 [docs/ci.md](../../docs/ci.md)。

Covers (test_dzplot.py): BagReader parsing, BoundedPubQueue zone transitions/overflow, PlotHub commands and state management,
TransportPacket roundtrip, sniffer poll scheduling, decode_payload fallback, input rate cap, backpressure zones, 段名/sanitize 规则核对。

## Message Deserialization

dzplot can extract structured fields from dzIPC messages for both bag replay and live sniffer:

- **Bag replay**: Each bag message is a `dzipc_log/TransportPacket` envelope (timestamp + topic + type + domain + msg_id + transport + role + event + payload). dzplot parses this envelope, then calls `create_message(type_name).deserialize(inner_payload)` to produce typed fields (scalars, arrays, nested messages).
- **Live sniffer (嗅探腿)**: Uses `_decode_payload(msg_type, raw_data)` with the user-configured `msg_type`; DZFlat段按 magic 识别后交给 `dzipc.dzflat`(纯 Python)解码。
- **Live subscriber (订阅腿)**: `_extract_fields()` 是两种 wire 的**唯一分派点** —— TLV 走 `field_count()/get_*`, DZFlat 走 schema 解码(见下一节)。
- **Fallback**: Unknown types or missing dzIPC bindings degrade gracefully to base64-encoded `data` field — the frontend always gets something.

## DZFlat 与旁路借样(dzflat)

发布端开启 DZFlat(`EnableDzFlat(true)` + 生成的 schema)后, SHM 通道上传的是**平坦段**
而不是 TLV; 订阅腿对 schema-less 话题(本工具的模板恒为 `GenericMessage`)会把段**借样**
收下(dzflat_adopt, 不拷整段), 此时消息对象里 `field_count() == 0`。

⚠️ **不能直接调 `_serialize_message_fields()`** —— 它靠 `field_count()` 枚举字段, 对借样
消息会**静默返回空 dict**: 图上看不到任何东西、也不报错。这正是
[dzflat_known_issues.md §9.6](../../docs/dzflat_known_issues.md) 记的那个坑(症状是"通道
看着通、数据是空的")。dzplot 现在的处理:

```
msg_obj.has_dzflat()?
  ├── 是 → msg_cls.from_generic()            # 与 TLV 路径同形(嵌套 → dict、数组 → list)
  │        └─ 失败 → dzipc.dzflat.decode_generic(zero_copy=True)   # 直读借样视图
  │                 └─ 再失败(对端 schema 不在本进程) → base64 + 一次性告警
  └── 否 → _serialize_message_fields()       # TLV, 行为与修复前完全一致
```

样本里因此多两个字段: `wire`(`"tlv"`/`"dzflat"`)与 `dzflat_borrowed`(仅 DZFlat 时出现)。
借样是否生效、走的是哪种 wire, 不必再靠猜; 首次见到 DZFlat 时也会在 stdout 打一行说明。

几条边界(都是有意为之, 不是缺陷):

1. **dzplot 不需要打开任何开关**: 接收侧恒双 wire(按段首 magic 判别), 是否发平坦段由
   **发布端进程**的 `EnableDzFlat` 决定(库级默认关)。
2. **schema 必须在本进程**: 解码靠生成器产出的 schema(`python/dzipc/gen_msgs/_dzflat_schema.py`)。
   对端改了 `.msg` 而本机没重跑 generator ⇒ 段解不出, dzplot 回退 base64 并告警一次。
3. **嗅探腿有前提**: 嗅探器不是接收方, 而发布端在"无接收方"时必须送 TLV(否则按旧 TLV
   解析的 sniffer 会读错), 所以**只被嗅探、无人订阅**的话题看到的是 TLV。有真订阅者时,
   嗅探腿照样能读到平坦段(已实测)。
4. **借样只在 SHM 上是真零拷贝**: UDP 腿借的是去帧后的独立块(分帧格式固有的一次拷贝)。
   语义两边一致, 别拿 UDP 侧论证零拷贝收益。
5. **数组字段每个样本最多 100 个元素**(两条 wire 相同, 给 WebSocket 帧留预算) —— 图像类
   话题请用 width/height/step/encoding 判断通道是否正常, 不要数 `data` 长度。

## Sniffer Binding (python/src/interface.cc)

Passive `ipc::sniffer` pybind11 binding is available as `dzipc.Sniffer`. It attaches to the SHM channel (name = `dz_ipc_<sanitized_topic>_topic`, topology = route) without registering as a receiver — publishers are unaffected. Falls back to `SubscriberIPCPtrMake` if `_dzipc_core.so` is not rebuilt after the interface.cc change.

## Known Limitations

1. **Deserialization requires dzIPC Python bindings**: Structured field extraction needs `_dzipc_core.so` built and importable. Without it, only base64-encoded raw data is available. Run `scripts/install.sh` first.
2. **No field auto-discovery for bag mode**: Bag connection records carry `dzipc_log/TransportPacket`; the real type is inside each message envelope. Users manually enter field names or use the quick-field buttons.
3. **Single-process server**: The asyncio server runs in one process. For production multi-client use, deploy behind a reverse proxy.
4. **Chart memory**: Each chart holds up to 600 data points (~10s at 60fps). Long sessions may need periodic page refresh.
5. **DZFlat 依赖本进程的 schema**: 段能收到、但 schema 指纹在本进程注册表里找不到时, 该样本退化成 base64 `data`(并打一次告警) —— 不会静默空字段, 也不会错解。详见上面「DZFlat 与旁路借样」节。
