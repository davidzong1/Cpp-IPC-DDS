#!/usr/bin/env python3
"""步骤② 通道 B(临时插桩)的打补丁 / 还原驱动。

参考体例: docs/mut_poolobs.py(变异臂驱动)。差别在于本脚本**不记红集** ——
它不改变行为, 只加两个只读计数器, 所以判据不是"转红"而是:

  1. 补丁必须**恰好匹配一次**(匹配 0 次或多次 = 脚本错误, 不是结论);
  2. 插桩构建下 RESULT 行里必须 `instr=1` —— ⛔ 这是**非退化正对照**:
     若符号没导出(visibility/内联/改名任一), weak extern 解析为 nullptr,
     基准会自动降级成"只有通道 A"并打 `instr=0`, 而 `instr=0` 与
     "跑的是干净构建"完全同形 —— 会被读成"通道 B 同意通道 A"。
     所以 `instr=0` 时报通道 B 的数据一律作废(脚本层硬失败, 见 pool_attribution_run.sh);
  3. 还原后 md5 必须与打补丁前的**逐字节一致**(自证, 不是声称)。

用法:
    python3 docs/pool_attribution_instr.py status   # 只报: 当前是否插桩 + 三方 md5
    python3 docs/pool_attribution_instr.py on       # 备份 → 打补丁 → 重建 → 报 md5
    python3 docs/pool_attribution_instr.py off      # 还原 → 重建 → md5 自证
"""
import hashlib
import os
import subprocess
import sys

ROOT = "/home/zwc/cpp_ipc_dds"
SRC = f"{ROOT}/src/dzIPC/shm_pub_sub_ipc.cc"
BACKUP = "/tmp/pa_instr_backup.cc"          # 原文件逐字节副本(树外, 不污染 git status)
MD5REC = "/tmp/pa_instr_pristine.md5"       # 原文件 md5(独立于副本的第二个证据)
LIB = f"{ROOT}/build/lib/libipc.so.1.3.0"
BIN = f"{ROOT}/build/bin/pool_attribution_benchmark"

MARK = "PA-INSTR"

# ---------------------------------------------------------------- 补丁本体
# 两处 hunk, 各自必须恰好匹配一次。全部插桩点都带 MARK 注释, 便于 grep 定位/审计。
HUNKS = [
    # --- hunk 1: 两个只读计数器 ---
    # 放 dzIPC::shm::pa_instr —— 一个**只为插桩新起**的子命名空间。
    # ⛔ 两次踩坑的结论(都在下面注释的代码里留了痕):
    #   ① 叫 dzIPC::shm::detail ⇒ 遮蔽。本文件在 `namespace dzIPC { namespace shm {`
    #      内, 一旦 shm 里出现 detail, 所有既有的 `detail::NoteDzFlatRx(...)` 限定名
    #      查找就**在 shm 里命中即停**, 不再上溯到 dzIPC::detail ⇒ 6 处调用点集体报
    #      "not a member of dzIPC::shm::detail"。命名空间内的声明无论写在用点前后都
    #      可见, 所以"挪到后面"也躲不掉。
    #   ② 想绕开遮蔽、写 `namespace dzIPC { namespace detail {` 放**既有**的
    #      dzIPC::detail ⇒ 在当前作用域里从 dzIPC 开始嵌, 得到的是
    #      `dzIPC::shm::dzIPC::detail`(多套了一层), 反而更错; 而 C++ 的
    #      nested-namespace-definition 语法**不允许** `namespace ::dzIPC::detail`
    #      这种带前导 :: 的上溯写法。
    # ⇒ 唯一同时满足"外部链接"与"不碰既有名字"的形态: 自命名子空间。
    # 非 static / 非匿名 namespace ⇒ 外部链接 ⇒ 基准能 extern 到。
    (
        "using dzIPC::control_plane_shm::TopicState;\n",
        "using dzIPC::control_plane_shm::TopicState;\n"
        "\n"
        "/* " + MARK + " (步骤② 通道 B, 临时): 两个只读计数器, 由\n"
        " * docs/pool_attribution_instr.py 打上/还原, **不属于产品码**。\n"
        " * 全文件 grep '" + MARK + "' 可定位全部插桩点。\n"
        " * 子命名空间 pa_instr 是专为插桩新起的(不复用 detail —— 会遮蔽本 TU\n"
        " * 既有的 detail::NoteDzFlatRx 调用; 见本脚本 HUNKS 的注释)。 */\n"
        "namespace pa_instr\n"
        "{\n"
        "std::atomic<std::size_t> g_pa_view_q{0};\n"
        "std::atomic<std::size_t> g_pa_msg_q{0};\n"
        "}   // namespace pa_instr\n",
    ),
    # --- hunk 2: 采样点 ---
    (
        "                if (handshake_completed.load(std::memory_order_acquire))\n"
        "                {\n"
        "                    buff_t raw_data;\n",
        "                if (handshake_completed.load(std::memory_order_acquire))\n"
        "                {\n"
        "                    /* " + MARK + " 采样点(通道 B): 本轮 recv **之前** ——\n"
        "                     * 语义 = \"上一轮 recv+push 完成之后、本轮尚未取新消息\"的队列深度。\n"
        "                     *\n"
        "                     * ⚠️ 刻意放在本 if 块最前面, **不**放迭代末尾: 迭代体内有 6 处\n"
        "                     * continue(分流 / 拒收 / 类型不匹配 / 话题未就绪), 放末尾会漏采,\n"
        "                     * 而漏掉的恰好是\"刚 push 完就 continue\"——最该采的那种时刻。\n"
        "                     * 放最前面则每条迭代恰好采一次, 与走哪条分支无关。\n"
        "                     *\n"
        "                     * 相位: 通道 A(外部读池)与通道 B 读的是同一时刻的池/队列,\n"
        "                     * 但 A 的采样是独立线程,**相位差在 1 个 recv 周期内** —— 所以判据\n"
        "                     * 用 Q ≤ L 与分布(不是逐点相等), 见 pool_attribution_run.sh。\n"
        "                     * relaxed 足够: 只做占用量粗观测, 不参与任何同步。 */\n"
        "                    pa_instr::g_pa_view_q.store(view_queue_->size(),\n"
        "                                                std::memory_order_relaxed);\n"
        "                    pa_instr::g_pa_msg_q.store(msg_queue_->size(),\n"
        "                                               std::memory_order_relaxed);\n"
        "                    buff_t raw_data;\n",
    ),
]


def md5(p):
    try:
        return hashlib.md5(open(p, "rb").read()).hexdigest()
    except FileNotFoundError:
        return "(缺)"


def is_patched():
    return MARK in open(SRC).read()


def build():
    b = subprocess.run(["cmake", "--build", "build", "-j8",
                        "--target", "pool_attribution_benchmark"],
                       cwd=ROOT, capture_output=True, text=True)
    if b.returncode != 0:
        return "BUILD FAILED:\n" + b.stdout[-3000:] + b.stderr[-3000:]
    return None


def report(tag):
    print(f"[{tag}] src      md5={md5(SRC)}  patched={1 if is_patched() else 0}")
    print(f"[{tag}] libipc   md5={md5(LIB)}")
    print(f"[{tag}] bench    md5={md5(BIN)}")
    if os.path.exists(MD5REC):
        print(f"[{tag}] pristine md5={open(MD5REC).read().strip()}  (打补丁前的原文件)")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"

    if cmd == "status":
        report("status")
        return 0

    if cmd == "on":
        src = open(SRC).read()
        if MARK in src:
            print("!! 已经是插桩态 —— 先跑 `off` 还原再 `on`(避免在补丁上再打补丁)")
            report("on/已插桩")
            return 1
        for i, (old, new) in enumerate(HUNKS, 1):
            n = src.count(old)
            if n != 1:
                print(f"!! hunk {i} 匹配 {n} 次(要求恰好 1 次) —— 脚本错误, 不是结论。")
                print(f"   锚点原文:\n{old}")
                return 1
        # 备份 + 记 md5(两个独立证据: 副本可还原内容, md5 可核还原结果)
        open(BACKUP, "w").write(src)
        open(MD5REC, "w").write(md5(SRC) + "\n")
        pristine = md5(SRC)
        if md5(BACKUP) != pristine:
            print("!! 备份与原文不一致(写盘失败?) —— 中止, 未改树")
            return 1
        for old, new in HUNKS:
            src = src.replace(old, new)
        open(SRC, "w").write(src)
        print(f"打补丁前 md5 = {pristine}  (副本 {BACKUP})")
        err = build()
        if err:
            print(err)
            print("!! 构建失败 —— 已自动还原, 请查上面的报错")
            open(SRC, "w").write(open(BACKUP).read())
            build()
            return 1
        report("on")
        # 符号必须真的进了动态表, 否则运行时 instr=0(见文件头 ⛔)
        nm = subprocess.run(["nm", "-D", "--defined-only", LIB],
                            capture_output=True, text=True)
        hits = [l for l in nm.stdout.splitlines() if "g_pa_view_q" in l or "g_pa_msg_q" in l]
        print(f"[on] 动态符号表: {'✅ ' + '; '.join(hits) if hits else '⛔ 没找到 g_pa_* —— 通道 B 会静默降级'}")
        return 0 if len(hits) == 2 else 1

    if cmd == "off":
        if not os.path.exists(BACKUP):
            print(f"!! 找不到备份 {BACKUP} —— 无法还原(拒绝凭空猜原文)")
            return 1
        if not os.path.exists(MD5REC):
            print(f"!! 找不到 {MD5REC} —— 没有可核对的基线 md5, 拒绝还原")
            return 1
        pristine = open(MD5REC).read().strip()
        open(SRC, "w").write(open(BACKUP).read())
        now = md5(SRC)
        print(f"还原: md5 {now}  vs 打补丁前 {pristine}  ->  {'✅ 逐字节一致' if now == pristine else '⛔ 不一致!'}")
        err = build()
        if err:
            print(err)
            return 1
        report("off")
        return 0 if now == pristine else 1

    print(f"未知子命令 {cmd!r}; 用 status | on | off")
    return 2


if __name__ == "__main__":
    sys.exit(main())
