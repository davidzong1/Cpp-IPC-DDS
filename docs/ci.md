# CI 与验证入口

> 审计日期: 2026-09-15 · 对象: `HEAD=8663dab7` 工作树

> ⚠️ **本轮交付的验证状态(务必先读)**: 本文档与 `scripts/ci_check.sh` 是在**无法执行任何 shell**
> 的环境下写的(工具链故障, 全文所有 `bash`/`python3.10` 调用均未能运行)。因此:
> **§1 的事实均来自只读核对(逐行读 `.gitignore`、`git status`、目录枚举), 可靠; §2/§3/§4 的脚本与命令
> 未经实跑, 属"待验证"。** 首次使用前请先跑一次 `bash scripts/ci_check.sh`(或至少 `bash -n scripts/ci_check.sh`)
> 并把结果回填到本文件与交付报告 —— 尤其要确认: ①脚本语法与失败路径(故意把 `PY310` 指向不存在的解释器,
> 应得 `rc=2` 而非崩溃)②`python3.10 -c "import dzipc, dzipc._dzipc_core"` 真的可导入
> ③三条运行时核对在 `3.10` 下全绿。**不要把"未跑过"读成"已验证"。**


## 1. 审计结论

| 项 | 现状 |
|----|------|
| `.github/workflows/` | **不存在**(仓库无任何 CI 配置) |
| git remote | `git@github.com:davidzong1/Cpp-IPC-DDS.git`(有远端托管, 但无 workflow) |
| `.gitignore:64` | `.github/workflows/**` —— **workflow 目录被显式忽略** |
| C++ 测试入口 | `test/CMakeLists.txt` 无 `enable_testing()` / `add_test()` ⇒ gtest 二进制**没有可复现入口** |
| Python 验证入口 | 分散在 4 个脚本, 解释器要求各不相同, README 只记了其中 1 个 |
| 根 `README.md` | `## Test (TODO)` —— 该区为空 |

### 1.1 ⛔ 首要发现: workflow 目录被 gitignore, 写 workflow 是**静默失效**

`.gitignore:64` 是 `.github/workflows/**`(在 `# agnent` 注释块下, 与 `.vscode/**`、`perf_results/**` 同批加入)。后果:

- 把 `ci.yml` 放进 `.github/workflows/` **不会**被 `git add` 跟踪, 不会推到远端, **CI 永远不会跑**;
- 而本地 `ls .github/workflows` 看得到文件 —— 看起来"有 CI", 实际是空转。这与本仓其它已记录的静默失效(段名错位、`create|open` 造空壳)同类: **不报错, 只是不做**。

所以本仓的 CI 入口被设计成**可被跟踪的脚本** `scripts/ci_check.sh`, workflow 只是调用它的一层薄壳(见 §4)。

### 1.2 绑定是构建产物, 干净检出里没有

`python/dzipc/_dzipc_core.cpython-310-x86_64-linux-gnu.so` **不入库**(`.gitignore:8` 的 `*.so` + `python/.gitignore:4`)。含义:

- 任何需要绑定的检查, 在干净检出上**必须先构建**: `python3.10 -m pip install ./python`(或 `scripts/install.sh`);
- 文件名里的 `cpython-310` 是 ABI 标记, **不是随便写的**: 在 3.12 下 `import dzipc` 必失败;
- 因此"绑定缺失"必须是一条**显式失败**的前置检查, 不能靠"跑挂了再说"。

## 2. 最小 CI 矩阵

| 层 | 检查 | 解释器 | 需要绑定 |
|----|------|--------|----------|
| L1 | `tools/dzplot/test/test_dzplot.py` | 任意 CPython3(默认 `python3`) | 否 |
| L2 | `CPython 3.10 + dzipc._dzipc_core 可导入` | **CPython 3.10** | 是 |
| L2 | `tools/dzplot/test/verify_segment_naming.py` | **3.10** | 是 |
| L2 | `tools/dzplot/test/verify_runtime_no_garbage.py` | **3.10** | 是 |
| L2 | `tools/dzplot/test/integration_pub_restart.py` | **3.10** | 是 |

**版本固定**: L2 全部钉在 `3.10`(可用 `PY310=` 覆盖路径, 但不换版本)。理由是 ABI 而非偏好 —— 换到 3.12 不是"少测一点", 而是测不到: `import dzipc` 直接失败, 于是所有运行时核对退化成 SKIP。

**为什么 L1 要单独留一层**: `test_dzplot.py` 是自带 runner(既不是 pytest 也不是 unittest), 判据是静态规则核对(sanitize 逐字节、段名规则 vs C++)加纯 Python 逻辑, **不需要绑定**。它因此是旁路哨兵: 绑定挂了、3.10 不在, 段名规则这类判据仍在守。

**失败即停**: `scripts/ci_check.sh` 任一层失败立即 `exit`, 并打印已跑项的矩阵; workflow 侧配 `strategy.fail-fast: true`。

## 3. 可执行命令(本地 runbook)

```bash
# ① 一次性: 构建绑定(干净检出必做; 本机已构建则可跳过)
python3.10 -m pip install ./python
#   或: bash scripts/install.sh   (会连带更新 msg/srv)

# ② 全量最小门(失败即停)
bash scripts/ci_check.sh

# ③ 只跑无绑定层(任意 python3 可跑, 不碰 /dev/shm)
bash scripts/ci_check.sh --no-binding

# ④ 开发机: 绑定/解释器缺失时降级为跳过(CI 不得使用)
bash scripts/ci_check.sh --allow-skip

# ⑤ 单独跑某一条运行时核对(需 3.10)
python3.10 tools/dzplot/test/verify_segment_naming.py
python3.10 tools/dzplot/test/verify_runtime_no_garbage.py
python3.10 tools/dzplot/test/integration_pub_restart.py

# ⑥ 指定 3.10 路径
PY310=/usr/bin/python3.10 bash scripts/ci_check.sh
```

一律用 `bash <script>` 调用而不是裸 `./script`: 前者不依赖文件的执行位(`scripts/ci_check.sh` 是新增文件, 若 clone 后发现 `Permission denied`, 那只是**执行位**没置上, 与脚本内容无关)。

退出码(脚本与三个运行时核对脚本统一): **0=全过 / 1=有失败 / 2=前置不满足(缺解释器、缺绑定、并发占用)**。

⛔ **`2` 在 CI 里必须按失败处理**。这三个脚本自己在缺绑定时就返回 `2` 并打印 `SKIP`, 而 `SKIP` 与 `PASS` 在 CI 面板上一样是绿的 —— 门会变成摆设。`scripts/ci_check.sh` 已经把 L2 前置做成独立检查项, 并且在 `--allow-skip` 未给时把 `2` 直接判失败。

## 4. 上 CI 的两步(当前**未做**, 需拍板)

`scripts/ci_check.sh` 本身已可跑; 要让它真正在推送上跑, 需要两步:

**第 1 步** — 让 workflow 能被跟踪(删掉 `.gitignore` 里这一行, 或改为 `!.github/workflows/`):

```diff
--- a/.gitignore
+++ b/.gitignore
@@ -61,7 +61,6 @@ local/**
 tmux**
 
 .vscode/**
-.github/workflows/**
 perf_results/**
```

**第 2 步** — 落 `.github/workflows/ci.yml`:

```yaml
name: ci
on: [push, pull_request]

jobs:
  python-checks:
    runs-on: ubuntu-22.04
    strategy:
      fail-fast: true                 # 失败即停: 任一 job 红, 其余取消
      matrix:
        include:
          - python-version: '3.10'    # 绑定 ABI = cpython-310, 必须能导入
            ci-args: ''
          - python-version: '3.12'    # 只跑无绑定层
            ci-args: '--no-binding'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: ${{ matrix.python-version }}
      - name: 构建 dzipc Python 绑定
        if: matrix.ci-args == ''
        run: |
          sudo apt-get update
          sudo apt-get install -y build-essential cmake
          python -m pip install --upgrade pip
          python -m pip install ./python
      - name: 最小 CI 门
        env:
          PY310: python
          PYBASE: python3
        run: bash scripts/ci_check.sh ${{ matrix.ci-args }}
```

⚠️ 这段 yaml **尚未在 runner 上实跑过**(本机无法执行 GitHub Actions)。首次上线时应按 job 日志校验: 尤其 `pip install ./python` 的 cmake 配置是否在干净容器里成立, 以及三条运行时核对在 runner 的 `/dev/shm` 上是否全绿。

## 5. 已知缺口(未在本轮修)

1. **C++ 层完全没有 CI**: `test/CMakeLists.txt` 无 `enable_testing()` / `add_test()`, gtest 二进制只能手工构建运行 ⇒ 源码变更后不重建就会有"陈旧二进制"风险。`test_udp_port_boundary.cpp`(新增, 8 用例)尤其如此 —— 它**没有注册进 CTest**, 无复现入口。本轮的 `scripts/ci_check.sh` **刻意不覆盖这一层**(任务要求"不依赖产品代码"), C++ 层需单独排期。
2. **三条运行时核对不可并行**: 它们起真实 SHM 段。`scripts/ci_check.sh` 用 `flock` 挡住了 ci_check 之间的并发, 但与 C++ gtest 并发的互斥仍靠调用方自觉(已有实测假红先例: 并发 gtest 的 `*_ser_control2` 段被误判成本 topic 的垃圾段)。
3. **`sniffer.open()` 的产品侧遗留**: 无发布端时仍会建 `__IPC_SHM__QU_CONN__*` 段且不回收。`verify_runtime_no_garbage.py` 以"钉住现状"的方式记录它 —— **上游修好时该组会立刻变红**, 须有人来更新预期。这是设计意图, 不是缺陷。
4. **`integration_pub_restart.py` 在 pytest 下不可收集**(模块级导入绑定)。本轮加了 SKIP guard 后, 3.12 下它给的是干净的 `rc=2` 而非 traceback; 但在 pytest 下仍是收集期 SKIP, 不是 test 用例。要进 pytest 需要把它改造成 fixture 式, 属另一件事。
