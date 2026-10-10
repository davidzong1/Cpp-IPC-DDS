# dzplot 统一工作区验收

2026-10-10，本机完成界面、HTTP / WebSocket、绑定解码和实际 SHM 数据验证。使用方法见 [dzplot 说明](../../tools/dzplot/README.md)。

一个服务同时提供曲线主页面 `/`、3D 子页面 `/viz/` 与两条 WebSocket 通路。曲线、3D、分栏标签切换保留状态；回放和嗅探的 3D 显示复用工作区样本。布局按 `ref_ui.jpg` 调整为浅色工具栏、左侧数据集、中央工作区、右侧工具与底部播放条。

曲线默认在一个铺满绘图区的窗口中叠加显示。点击窗口后勾选话题 / 字段，或把话题 / 字段拖到指定窗口；话题操作添加全部已发现的数值字段。窗口支持左右、上下和嵌套分裂，分隔条可拖动或用方向键调整。各窗口独立选择曲线，同一字段共用数据缓冲；关闭窗口后相邻窗口填充，合并时保留全部曲线。刷新恢复布局与曲线归属，离线清空后的刷新 / 重连也保留空窗口。

文件目录也已合并：3D 后端位于 `tools/dzplot/visualizer.py`，组件和传输模块位于 `component/`、`transport/`，网页及离线依赖位于 `web/viz/`，发布示例与模型位于 `demo/`，测试统一位于 `test/`；原 dzviz 目录和启动入口已删除。`/viz/`、`/viz/ws`、`/robot_assets/` 继续由 `dzplot/main.py` 提供。两份默认配置已合并，示例配置使用新的 URDF 路径并与发布端话题保持一致。

```bash
python3 tools/dzplot/main.py --demo
# 打开 http://127.0.0.1:8766/，分栏视图为 /#split
```

## 截图

| 场景 | 截图 |
|---|---|
| 默认单窗口、多曲线与字段列表 | [main.png](main.png) |
| 左右 / 上下嵌套分裂与独立曲线选择 | [windows.png](windows.png) |
| 曲线和 3D 分栏 | [split.png](split.png) |
| 3D 标签与显示列表 | [3d.png](3d.png) |
| 390×844 窄屏布局 | [mobile.png](mobile.png) |
| 上一轮工作区验收：真实 SHM 订阅，1200 点点云、80×60 RGB 图像、宽度曲线 | [live.png](live.png) |
| 上一轮工作区验收：两个被动嗅探器，250 点点云、完整 RGB 图像、宽度曲线 | [sniff.png](sniff.png) |

浏览器验证阻止外部网络，确认 Three.js 从同一个服务加载；场景实际生成 3D 对象。真实订阅和嗅探均检查图像为 14400 字节，浏览器脚本错误为 0。

## 自动检查

| 检查 | 结果 |
|---|---|
| dzplot 既有回归 | 129 通过、0 失败 |
| 工作区集成、回放和浏览器 | 11 通过、0 失败 |
| 工作区集成、回放和实际绑定 | 12 通过、0 失败 |
| 3D 组件、集成和浏览器 | 43 通过、0 失败 |
| 传输、速率控制与运动学 | 85 通过、0 失败 |
| 实际 SHM：DZFlat 默认开启 / 关闭、TLV 图像、RobotState | 4 个场景通过 |
| `ci_check.sh --no-binding` | L1 三项通过 |
| Python 编译、JavaScript / shell 语法、差异空白检查 | 通过 |

工作区检查覆盖同端口资源、两条 WebSocket、曲线与 3D 双向共享、避免重复订阅、二进制帧、配置保存加载、机器人状态来源、暂停继续、速度、进度、停止和重播。绑定检查使用实际 TLV / DZFlat 原始字节确认完整图像和 1200 点点云，并验证录包 TransportPacket 从文件回放至曲线和 3D。

多窗口验收包含多条曲线的实际 Canvas 像素、嵌套分裂的区域填充、按窗口勾选、字段 / 话题 / 图例原生拖拽、共享字段与颜色、鼠标及键盘调整比例、清空 / 关闭 / 合并、刷新恢复、窄屏和重连时空选择的保留。目录合并后重新运行表中的前五项检查；新增验收确认 HTTP 返回新目录中的完整网页 / Three.js 资源，并实际读取机器人示例与 Go2 网格。迁移清单逐项核对了 58 个源文件，其中 27 个模型、图像及第三方资源哈希保持一致。

迁移后的图像、点云、机器人发布端均已用 CPython 3.10 实际启动；三个示例的脚本入口和模块入口、离线机器人配置生成、从其它工作目录启动统一服务也通过检查。真实 C++ 发布端与迁移后的 Python 3D 订阅端完成四个端到端场景，图像载荷逐字节一致，RobotState 的位姿、轨迹和关节状态均正确。

```bash
python3 tools/dzplot/test/test_dzplot.py
python3 tools/dzplot/test/test_workspace.py --browser --artifacts /tmp/dzplot-workspace
python3.10 tools/dzplot/test/test_workspace.py --binding
python3 tools/dzplot/test/test_visualizer_components.py --integration --browser
python3 -m pytest tools/dzplot/test/test_transport.py tools/dzplot/test/test_rate_controller.py tools/dzplot/test/test_kinematics.py -q
```

浏览器检查需要 Playwright 与 Chrome / Chromium；绑定检查需要匹配 ABI 的 Python，本机使用 CPython 3.10。

实际 SHM 端到端检查位于 `tools/dzplot/test/test_dzflat_live_e2e.py`。先在启用了 `LIBIPC_BUILD_TESTS` 的构建目录中生成 `dzflat_py_publisher`：

```bash
cmake --build build --target dzflat_py_publisher -j2
LD_LIBRARY_PATH="$PWD/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  python3.10 tools/dzplot/test/test_dzflat_live_e2e.py
```

## 原有 SHM 回归的结果

按顺序运行，测试期间停止了其它本任务的 SHM 发布端与服务。

| 检查 | 结果 |
|---|---|
| verify_segment_naming.py | 34 通过、0 失败 |
| verify_runtime_no_garbage.py | 12 通过、1 失败 |
| integration_pub_restart.py（独立运行） | 23 通过、0 失败 |

目录合并后又验证了段名检查（34 通过）和发布端重启检查（23 通过）。下述完整 CI 的旧断言差异是上一轮结果，本次未修改其预期。

全量 `LD_LIBRARY_PATH=$PWD/local/lib bash scripts/ci_check.sh` 返回 `1`。失败的是原有检查的一条预期：它要求没有发布端时，嗅探停止后仍残留数据段；本机绑定实际清理了这些段。使用修改前的 `HEAD:tools/dzplot/main.py` 单独复验，运行中有该话题数据段，停止后没有残留，行为相同。这条旧断言的预期本次保留，未将全量 CI 标为通过。段名、控制面不创建空壳和发布端重启验证均通过。
