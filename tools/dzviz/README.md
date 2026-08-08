# DZIPC Visualizer — 使用说明

> 基于 Three.js 的 3D Web 可视化面板，用于实时查看 dzIPC 消息数据（位姿轨迹、点云、图像、机器人状态）。

---

## 目录

- [快速开始](#快速开始)
- [Demo 快速体验](#demo-快速体验)
  - [1. 点云](#1-点云)
  - [2. 图像](#2-图像)
  - [3. 机器人状态](#3-机器人状态)
- [界面概览](#界面概览)
- [配置说明](#配置说明)
  - [配置文件格式](#配置文件格式)
  - [配置字段说明](#配置字段说明)
- [Display 类型](#display-类型)
- [3D 视图操作](#3d-视图操作)
- [桥接启动参数](#桥接启动参数)
- [自定义 URDF 机器人](#自定义-urdf-机器人)
- [故障排查](#故障排查)

---

## 快速开始

```bash
# 1. 启动 Web 桥接服务
python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765

# 2. 浏览器打开
# http://127.0.0.1:8765
```

桥接服务负责在 dzIPC 消息总线和浏览器之间中转消息（WebSocket）。

---

## Demo 快速体验

Demo 脚本位于 `tool/visualizer/demo/`，提供三种数据类型的一键发布示例。

### 1. 点云

```bash
# 终端 1: 启动桥接
python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765

# 终端 2: 启动点云发布者（默认 100000 点，shm 传输，height 着色）
python3 tool/visualizer/demo/send_point_cloud_demo.py \
  --topic /demo/point_cloud --domain-id 0 --transport shm

# 浏览器操作:
# 打开 http://127.0.0.1:8765 → 点击 "+ Add Display" →
#   Topic:     /demo/point_cloud
#   Type:      PointCloud
#   Domain:    0
#   Transport: shm
# → 点云实时显示在 3D 视图中
```

#### 点云参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--points` | 100000 | 每帧点数量 |
| `--mode` | walls | 分布模式：`cube` / `sphere` / `walls` |
| `--colors` | height | 着色模式：`none` / `height`（按高度）/ `random` |
| `--range` | 20.0 | 点云散布半径（米） |
| `--drift` | 0.08 | 每帧整体漂移量 |
| `--period` | 0.05 | 发布间隔（秒），即 20 FPS |

### 2. 图像

```bash
# 终端 2: 启动图像发布者（默认读取 demo/images/DepthFigure.jpeg）
python3 tool/visualizer/demo/send_image_demo.py \
  --topic /demo/image --domain-id 0 --transport shm

# 浏览器操作:
# 打开 http://127.0.0.1:8765 → 点击 "+ Add Display" →
#   Topic:     /demo/image
#   Type:      Image
#   Domain:    0
#   Transport: shm
# → 图像浮窗显示在 3D 视图右上角（可拖拽、可缩放）
```

#### 图像参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--image` | `demo/images/DepthFigure.jpeg` | 图片路径，支持 JPEG/PNG |
| `--synthetic-rgb8` | false | 使用程序生成的 RGB 渐变色图 |
| `--width / --height` | 自动检测 | 手动指定图片尺寸 |
| `--encoding` | 自动推断 | 强制指定编码：`jpeg` / `png` / `rgb8` / `mono8` 等 |
| `--period` | 1/60 | 发布间隔（秒），即 60 FPS |

### 3. 机器人状态

```bash
# 终端 2: 启动机器人状态发布者（使用内嵌的 2 自由度机械臂 URDF）
python3 tool/visualizer/demo/send_robot_state.py \
  --topic /demo/robot_state --domain-id 0 --transport shm

# 浏览器操作:
# 启动桥接时加载配置（推荐）:
python3 tool/visualizer/dzipc_web_bridge.py --host 127.0.0.1 --port 8765 \
  --config tool/visualizer/demo/robot_state_config.json --load-config
# → 机械臂模型自动加载，关节实时运动

# 或手动操作:
# 打开 http://127.0.0.1:8765 → 点击 "+ Add Display" →
#   Type:      Robot
#   Topic:     robot
#   URDF Path: tool/visualizer/demo/robots/tiny_2dof_arm.urdf
# → 再次点击 "+ Add Display" →
#   Type:      RobotState
#   Topic:     /demo/robot_state
#   Target Robot: demo_robot
# → 机械臂关节按正弦波实时摆动，同时 base_link 沿 XY 平面平动
```

#### 机器人状态参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--urdf` | 自动搜索 | 指定 URDF 文件路径 |
| `--robot-id` | demo_robot | 机器人标识 |
| `--state-id` | demo_robot_state | 状态 display 标识 |
| `--frame-id` | world | 参考坐标系 |
| `--period` | 0.03 | 关节状态发布间隔（秒），约 33 FPS |
| `--seed` | 7 | 关节运动随机种子 |
| `--urdf-every` | 20 | 每 N 帧重新发布一次 URDF |
| `--config-out` | `demo/robot_state_config.json` | 自动生成的配置文件输出路径 |
| `--prepare-only` | false | 仅生成配置和准备 URDF，不发布 |
| `--no-download` | false | 仅使用本地 URDF，不从网络下载 |

---

## 界面概览

```
┌──────────┬─────────────────────────────────────────┐
│ Sidebar  │  Viewport                                │
│          │  ┌ Toolbar ───────────────────────────┐  │
│ Brand    │  │ 标题 │ 操作提示  │ Zoom │ Grid │ N │  │
│          │  └────────────────────────────────────┘  │
│ + Add    │                                          │
│ Display  │  ┌─ 3D Scene ──────────────┐ ┌────┐    │
│          │  │                         │ │FPS │    │
│ Displays │  │    (Three.js Canvas)    │ └────┘    │
│ ──────── │  │                         │            │
│ topic_1  │  │                         │           │
│ topic_2  │  │                    [XYZ]│ ← 右下角  │
│ ...      │  │                         │   坐标轴  │
│          │  └─────────────────────────┘            │
│ Config   │  ┌ Image ──────────────────┐ (可拖拽) │
│ (折叠)   │  │ 图像浮窗                │           │
│          │  └─────────────────────────┘            │
│ Status   │                                          │
│ ──────── │                                          │
│ 事件日志 │                                          │
└──────────┴─────────────────────────────────────────┘
```

| 区域 | 功能 |
|------|------|
| **Sidebar / Displays** | 已订阅的 topic 列表，勾选 "Visualize" 使数据出现在 3D 视图中 |
| **Sidebar / Config** | （折叠）导入/导出 JSON 配置文件 |
| **Sidebar / Status** | WebSocket 连接状态、消息事件日志 |
| **Toolbar** | 当前活跃 topic 名称与元信息，Zoom 距离、Grid 刻度、Display 总数 |
| **3D Scene 右下角** | 参考坐标轴（红=X, 绿=Y, 蓝=Z），随视角同步旋转 |
| **3D Scene 右上角** | 实时 FPS |
| **Image 浮窗** | 图像 Display 的可拖拽/可缩放浮窗 |

---

## 配置说明

### 配置文件格式

启动桥接时可通过 `--config` 和 `--load-config` 加载 JSON 配置，自动创建 Displays 和机器人模型：

```bash
python3 tool/visualizer/dzipc_web_bridge.py \
  --host 127.0.0.1 --port 8765 \
  --config tool/visualizer/demo/robot_state_config.json --load-config
```

完整配置结构：

```jsonc
{
  "domain": 0,           // 全局 domain ID
  "transport": "shm",    // 全局传输方式 (shm / socket)
  "queue": 10,           // 全局队列大小
  "poll": 0.03,          // 全局轮询间隔（秒）
  "extra": "",           // 全局 InitChannel extra 参数
  "verbose": false,      // 全局详细日志

  "topics": [],           // 普通 topic 订阅列表
  "robot_displays": [],   // 机器人模型列表（URDF）
  "robot_state_displays": []  // 机器人实时状态订阅列表
}
```

### 配置字段说明

#### `topics` — 普通消息 Topic

```jsonc
{
  "topic": "/demo/point_cloud",   // topic 名称
  "msg_type": "PointCloud",       // 消息类型，见 [Display 类型](#display-类型)
  "domain": 0,
  "queue": 10,
  "transport": "shm",
  "poll": 0.03,
  "extra": "",
  "verbose": false
}
```

#### `robot_displays` — 机器人模型

```jsonc
{
  "id": "demo_robot",                    // 唯一标识
  "name": "tiny_2dof_arm",              // 显示名称
  "urdf_path": "tool/visualizer/demo/robots/tiny_2dof_arm.urdf",  // URDF 文件路径（相对仓库根目录）
  "urdf": "<?xml version=\"1.0\"?>...", // 可选：内联 URDF 内容（优先级高于 urdf_path）
  "fixed_frame": "world",               // 固定坐标系名称
  "visible": true                       // 是否在 3D 视图中显示
}
```

#### `robot_state_displays` — 机器人实时状态

```jsonc
{
  "id": "demo_robot_state",           // 唯一标识
  "name": "demo_robot_state",         // 显示名称
  "topic": "/demo/robot_state",       // 订阅的 RobotState topic
  "target_robot_id": "demo_robot",    // 关联的 robot_display id
  "msg_type": "RobotState",           // 固定为 RobotState
  "domain": 0,
  "queue": 10,
  "transport": "shm",
  "poll": 0.03,
  "extra": "",
  "visible": true
}
```

> **注意**：`robot_state_displays` 必须关联一个已存在的 `robot_displays` 条目（通过 `target_robot_id` 字段匹配 `id`）。

---

## Display 类型

通过 "+ Add Display" 按钮或配置文件中的 `topics`/`msg_type` 字段可以创建不同类型的 Display：

| 类型 | 可视化效果 | 说明 |
|------|-----------|------|
| **Pose** | 3D 轨迹线 + 球体标记 | 位姿 `{x, y, z}`，自动检测 `pose.x/y/z` 或 `position.x/y/z` 等嵌套路径 |
| **Path** | 3D 轨迹线 | 路径点序列 |
| **PointCloud** | 3D 离散点 + 渐进式点云地图 | 支持按高度/随机着色，自适应 GPU 降采样以保持帧率 |
| **Marker** | 3D 标记点 | 标注标记 |
| **Image** | 浮窗显示 JPEG/PNG/RGB8/Mono8 等 | 自动检测编码，可拖拽、缩放、关闭 |
| **RawMessage** | 仅侧边栏数据显示 | 不做 3D 可视化，仅展示原始消息元信息 |
| **Robot** | URDF 机械臂 3D 模型 | 显示 link 框架、joint 轴线和球体标记、连杆线段 |
| **RobotState** | 驱动机械臂关节运动 | 接收 `joint_state`（关节角度），驱动关联的 Robot display 实时运动 |

---

## 3D 视图操作

| 操作 | 功能 |
|------|------|
| 滚轮 | 缩放（Zoom） |
| 鼠标左键拖拽 | 轨道旋转（Orbit） |
| 鼠标右键拖拽 | 平移（Pan） |
| 侧边栏可拖拽 | 拖拽 `.sidebar-resize-handle` 灰色手柄调宽度（200~600px） |
| 面板折叠/− | 点击 Config / Status 面板右侧 `−` 折叠面板 |
| 右下角坐标轴 | 始终跟随主相机视角，红=X、绿=Y、蓝=Z |

---

## 桥接启动参数

```bash
python3 tool/visualizer/dzipc_web_bridge.py --help
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--host` | 127.0.0.1 | 监听地址 |
| `--port` | 8765 | 监听端口 |
| `--config` | 无 | 配置文件路径（JSON） |
| `--load-config` | false | 启动时自动加载 `--config` 指定的配置文件 |
| `--transport` | socket | 全局默认传输方式 |
| `--domain` | 1 | 全局默认 domain ID |

---

## 自定义 URDF 机器人

### 方式一：内联 URDF（推荐）

在配置文件的 `robot_displays` 中直接嵌入完整 URDF XML：

```jsonc
{
  "robot_displays": [{
    "id": "my_robot",
    "name": "my_robot",
    "urdf": "<?xml version=\"1.0\"?>...",
    "visible": true
  }]
}
```

### 方式二：路径引用

```jsonc
{
  "robot_displays": [{
    "id": "my_robot",
    "name": "my_robot",
    "urdf_path": "path/to/my_robot.urdf",
    "visible": true
  }]
}
```

### 方式三：使用 demo 脚本自动生成

`send_robot_state.py` 支持自动发现仓库内的 URDF 文件，或从网络下载示例：

```bash
# 使用本地 URDF
python3 tool/visualizer/demo/send_robot_state.py \
  --urdf path/to/robot.urdf --prepare-only

# 从 GitHub 下载示例 URDF
python3 tool/visualizer/demo/send_robot_state.py \
  --force-download --prepare-only
```

生成的配置文件路径由 `--config-out` 指定（默认 `tool/visualizer/demo/robot_state_config.json`）。

### URDF 要求

- 必须是合法的 URDF XML 格式
- 至少包含一个 `<link>` 和一个 `<joint>`
- 支持的 joint 类型：`fixed`（灰色）、`revolute`/`continuous`（橙色）、`prismatic`（绿色）
- 每个 joint 必须指定 `parent` 和 `child` link

---

## 故障排查

| 问题 | 排查步骤 |
|------|---------|
| 桥接启动失败 | 确认 Python3 已安装，网络端口 8765 未被占用 |
| 页面空白 | 确认浏览器支持 ES Module imports（Chrome 61+ / Firefox 60+）；检查浏览器控制台是否有 CDN 加载错误 |
| 连不上 WebSocket | 确认桥接服务已启动；查看 Status 面板事件日志 |
| 点云不显示 | 确认已勾选 topic 的 "Visualize" 复选框；确认 transport 和 domain 参数与发布端一致 |
| 图像不显示 | 确认图像编码格式受支持（JPEG/PNG/RGB8/BGR8/RGBA8/BGRA8/Mono8） |
| 机器人不显示 | 确认已先创建 Robot display（URDF 加载成功），再创建 RobotState display 并关联正确的 target_robot_id |
| 机器人关节不更新 | 确认 RobotState 消息中包含 `joint_state.position` 数组，且长度与 URDF 定义的 movable joints 数量匹配 |
| dzIPC 导入失败 | 执行 `scripts/install.sh` 编译安装本地 Python 绑定；demo 脚本会自动搜索 `local/lib/python/dzipc` 或 `python/dzipc` |
