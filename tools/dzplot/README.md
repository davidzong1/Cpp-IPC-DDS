# dzplot — dzIPC Time-Series Visualization Dashboard

Web-based time-series visualization for dzIPC topic data. Supports offline `.bag` file playback and online topic sniffing.

## Quick Start

```bash
# Serve a .bag file replay
python3 tools/dzplot/dzplot.py --bag /path/to/events.bag --port 8766

# Sniff live topics
python3 tools/dzplot/dzplot.py --sniff --topic /test:StdRawMessage --transport shm

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

## Architecture

```
Browser (Canvas 2D charts)
  ↕ WebSocket (JSON frames, batched)
dzplot.py (asyncio server)
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

```bash
python3 tools/dzplot/test/test_dzplot.py
```

Covers: BagReader parsing, BoundedPubQueue zone transitions/overflow, PlotHub commands and state management,
TransportPacket roundtrip, sniffer poll scheduling, decode_payload fallback, input rate cap, backpressure zones.

## Message Deserialization

dzplot can extract structured fields from dzIPC messages for both bag replay and live sniffer:

- **Bag replay**: Each bag message is a `dzipc_log/TransportPacket` envelope (timestamp + topic + type + domain + msg_id + transport + role + event + payload). dzplot parses this envelope, then calls `create_message(type_name).deserialize(inner_payload)` to produce typed fields (scalars, arrays, nested messages).
- **Live sniffer**: Uses `_decode_payload(msg_type, raw_data)` with the user-configured `msg_type`.
- **Fallback**: Unknown types or missing dzIPC bindings degrade gracefully to base64-encoded `data` field — the frontend always gets something.

## Sniffer Binding (python/src/interface.cc)

Passive `ipc::sniffer` pybind11 binding is available as `dzipc.Sniffer`. It attaches to the SHM channel (name = `dz_ipc_<sanitized_topic>_topic`, topology = route) without registering as a receiver — publishers are unaffected. Falls back to `SubscriberIPCPtrMake` if `_dzipc_core.so` is not rebuilt after the interface.cc change.

## Known Limitations

1. **Deserialization requires dzIPC Python bindings**: Structured field extraction needs `_dzipc_core.so` built and importable. Without it, only base64-encoded raw data is available. Run `scripts/install.sh` first.
2. **No field auto-discovery for bag mode**: Bag connection records carry `dzipc_log/TransportPacket`; the real type is inside each message envelope. Users manually enter field names or use the quick-field buttons.
3. **Single-process server**: The asyncio server runs in one process. For production multi-client use, deploy behind a reverse proxy.
4. **Chart memory**: Each chart holds up to 600 data points (~10s at 60fps). Long sessions may need periodic page refresh.
