#!/usr/bin/env python3
"""阶段 2 定向变异测试 —— 落点 docs/消息接收架构改造/阶段2_全量测试方案.md §7。

纪律（沿用仓内既有做法，见 docs/mut_poolobs.py 与进度缓存 v3/v4 的记录）:
  · 变异只在**独立 /tmp 副本**上进行，仓库工作树零改动；
  · 每次变异后校验「变异确实落上」（marker 字符串在文件里）—— 没落上算脚本错误，不是"杀不死"；
  · 每次确认目标二进制实际加载的是变异库（LD_LIBRARY_PATH 覆盖 RUNPATH，ldd 复核）；
  · 每条变异必须被**指定用例**杀死；若跑出全绿 ⇒ 该臂不承重 ⇒ exit 1（不是"通过"）；
  · 记「红集」而不只记 FAIL —— 多杀一条或杀错一条都要能看见。

用法:
  scripts/stage2_mutation.py            # 跑全部臂
  scripts/stage2_mutation.py --only X1  # 跑单臂
  scripts/stage2_mutation.py --keep     # 保留 /tmp 副本(便于复查)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


@dataclass
class Mutant:
    """一条变异。

    file      相对仓库根的路径
    old/new   要替换的原文/替换文（old 必须**恰好出现一次**，否则脚本报错）
    marker    注入后应出现在文件里的标记（校验"确实落上"）
    expect    期望被杀的用例名（gtest 全名或子串）
    suite     跑哪个测试目标
    why       这条变异证明什么（写进报告）
    """

    arm: str
    file: str
    old: str
    new: str
    marker: str
    suite: str
    expect: list[str]
    why: str


MUTANTS: list[Mutant] = [
    Mutant(
        arm="X1",
        file="src/dzIPC/shm_route_session.cc",
        old="        old->disconnect();",
        new="        (void)old; /* MUTANT X1: begin_rebuild step-3 disconnect removed */",
        marker="MUTANT X1",
        suite="test_shm_route_session",
        expect=["RebuildDisconnectUnblocksBlockedRecv"],
        why="删掉 begin_rebuild 对旧 route 的 disconnect ⇒ 重建必须靠 recv 超时才返回；"
        "证明「重建自己的第 3 步必须叫醒在途 recv」有守门。",
    ),
    Mutant(
        arm="X2",
        file="src/dzIPC/shm_route_session.cc",
        old="        cv_.wait(lock, [this] { return receive_inflight_ == 0; });\n\n        /* 5.",
        new="        /* MUTANT X2: step-4 wait for inflight removed */\n\n        /* 5.",
        marker="MUTANT X2",
        suite="test_shm_route_session",
        expect=["RebuildWaitsForInflightBeforeReleasingOldRoute"],
        why="不等 inflight 归零就 release 旧 route ⇒ release 与 recv 并发（说明 §8.2 的核心"
        "判据）；证明「release 必须发生在 release_receive 之后」有因果序守门。",
    ),
    Mutant(
        arm="X3",
        file="src/dzIPC/shm_route_session.cc",
        old="    if (stopping_ || rebuilding_ || !route_)\n    {\n        return std::nullopt;\n    }",
        new="    if (!route_) /* MUTANT X3: rebuilding_/stopping_ check removed */\n    {\n        return std::nullopt;\n    }",
        marker="MUTANT X3",
        suite="test_shm_route_session",
        expect=["RebuildRejectsNewAcquire"],
        why="重建期间允许新 acquire ⇒ 新 recv 进入正在被 release 的对象（I3 破坏）。",
    ),
    Mutant(
        arm="X4",
        file="src/dzIPC/shm_route_session.cc",
        old="            stopping_ = false;",
        new="            /* MUTANT X4: stopping_ not reset on successful rebuild */",
        marker="MUTANT X4",
        suite="test_shm_route_session",
        expect=["StopThenSuccessfulRebuildReopensLeases", "StopIsIdempotentAndNonTerminal"],
        why="成功 rebuild 不复位 stopping_ ⇒ add_peer 失败重试路径让该话题收包**静默停摆**。",
    ),
    Mutant(
        arm="X5",
        file="src/dzIPC/shm_pub_sub_ipc.cc",
        old="                    if (raw_data.empty())\n"
        "                    {\n"
        "                        continue;\n"
        "                    }\n"
        "                    /* ⛔ 叫醒伪影门",
        new="                    if (raw_data.empty())\n"
        "                    {\n"
        "                        continue;\n"
        "                    }\n"
        "                    /* MUTANT X5: drop buffers popped on an older generation */\n"
        "                    if (lease->generation != route_session_.generation())\n"
        "                    {\n"
        "                        continue;\n"
        "                    }\n"
        "                    /* ⛔ 叫醒伪影门",
        marker="MUTANT X5",
        suite="test_shm_i5_pop_buffer",
        expect=["PoppedTlvSurvivesGenerationRebuild", "PoppedDzFlatSurvivesGenerationRebuild"],
        why="recv 返回后按 generation 不匹配丢弃 buffer ⇒ 违反 I5（已弹出的字节被丢）。"
        "这正是 v4 §5 判为「靠无人写该判断成立」的那条 —— 本臂证明现在有守门了。",
    ),
    Mutant(
        arm="X6",
        file="src/dzIPC/shm_pub_sub_ipc.cc",
        old="    running.store(false, std::memory_order_release);\n    route_session_.stop_and_wake();\n    /* §4.2 的承重点",
        new="    running.store(false, std::memory_order_release);\n    /* MUTANT X6: dtor stop_and_wake removed */\n    /* §4.2 的承重点",
        marker="MUTANT X6",
        suite="test_shm_sub_dtor_gate",
        expect=["DtorWakesInflightRecvInOrder"],
        why="删掉析构的 stop_and_wake ⇒ 收包线程只能等 recv(50) 超时退出。v4 实测此时"
        "既有 4 套件 79 条全绿（零守门）；本臂证明新套件能杀。",
    ),
    Mutant(
        arm="X7",
        file="src/dzIPC/common/wire_accept.cc",
        old="    for (std::size_t i = 0; i < raw.size(); ++i)\n    {\n        if (p[i] != 0)\n        {\n            return false;\n        }\n    }\n    return true;",
        new="    /* MUTANT X7: scan tail 12 bytes only */\n    const std::size_t from = (raw.size() > 12) ? (raw.size() - 12) : 0;\n    for (std::size_t i = from; i < raw.size(); ++i)\n    {\n        if (p[i] != 0)\n        {\n            return false;\n        }\n    }\n    return true;",
        marker="MUTANT X7",
        suite="test_wakeup_artifact",
        expect=["RealDzFlatOfArtifactLengthIsNotFlagged"],
        why="伪影门只查尾部 12 字节 ⇒ 尾 12 字节恰好全零的**真** DZFlat 段被静默吃掉"
        "（比原缺陷更难查）。证明判据的宽窄有守门。",
    ),
    Mutant(
        arm="X8",
        file="src/dzIPC/shm_pub_sub_ipc.cc",
        old="                    if (IsWakeupArtifact(raw_data))\n                    {\n",
        new="                    if (false && IsWakeupArtifact(raw_data)) /* MUTANT X8 */\n                    {\n",
        marker="MUTANT X8",
        suite="test_wakeup_artifact",
        expect=["NoPhantomMessageOnGenerationRebuild"],
        why="删掉伪影门 ⇒ disconnect 叫醒的全零伪影成为用户可见假消息（msg_id==0 话题上）。",
    ),
]


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def copy_tree(dst: Path) -> None:
    """把工作树（排除 build/ 与 .git/）复制到 dst。"""
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    ignore = shutil.ignore_patterns("build", "build_*", ".git", "__pycache__", "*.pyc")
    # dirs_exist_ok=True: 复制内容进已存在的 dst（dst 本身刚建好）
    shutil.copytree(ROOT, dst, ignore=ignore, dirs_exist_ok=True)


def build_in(tree: Path, log: Path) -> bool:
    cfg = run(["cmake", "-S", str(tree), "-B", str(tree / "build"), "-DLIBIPC_BUILD_TESTS=ON"],
              cwd=tree)
    log.write_text(cfg.stdout + cfg.stderr, encoding="utf-8")
    if cfg.returncode != 0:
        return False
    bld = run(["cmake", "--build", str(tree / "build"), "--target", "ipc", "-j4"], cwd=tree)
    log.write_text(log.read_text(encoding="utf-8") + bld.stdout + bld.stderr, encoding="utf-8")
    return bld.returncode == 0


def apply_mutation(tree: Path, m: Mutant) -> None:
    p = tree / m.file
    text = p.read_text(encoding="utf-8")
    n = text.count(m.old)
    if n != 1:
        raise SystemExit(f"[{m.arm}] 锚点不唯一（出现 {n} 次）: {m.file}\n---\n{m.old}\n---")
    p.write_text(text.replace(m.old, m.new), encoding="utf-8")
    if m.marker not in p.read_text(encoding="utf-8"):
        raise SystemExit(f"[{m.arm}] 变异未落上: {m.file}")


def run_suite(tree: Path, suite: str, out: Path, gtest_filter: str | None = None) -> tuple[int, str]:
    """用变异库跑测试二进制（LD_LIBRARY_PATH 覆盖 RUNPATH）。返回 (rc, 输出)。

    ⛔ 必须用 `timeout` 包住：变异体会让用例**挂死或 abort**（例如 X3 允许重建期
    acquire ⇒ 计数不配对 ⇒ `wait_quiescent` 永不返回；断言构建下还会
    `terminate called without an active exception`）。这不是脚本错误，正是"被杀"
    的形态之一 —— 所以单跑超时（rc=124）与被信号带走（rc=134）都算杀死，
    前提是日志里能看到期望用例**已开始执行**。
    只看"有没有 FAILED 行"会把这两种形态漏成"未杀死"。"""
    binp = BUILD / "bin" / suite
    if not binp.exists():
        raise SystemExit(f"缺少测试二进制 {binp}（先在本树构建）")
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{tree}/build/lib:{tree}/build/bin"
    cmd = [str(binp)]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120)
        rc, text = r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        rc = 124
        text = (e.stdout.decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or ""))
        text += (e.stderr.decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or ""))
        text += "\n[MUTATION] 用例超时未结束（rc=124）\n"
    out.write_text(text, encoding="utf-8")
    return rc, text


def ldd_check(tree: Path, suite: str) -> str:
    binp = BUILD / "bin" / suite
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{tree}/build/lib:{tree}/build/bin"
    r = subprocess.run(["ldd", str(binp)], capture_output=True, text=True, env=env)
    for line in r.stdout.splitlines():
        if "libipc.so" in line:
            return line.strip()
    return "<未解析到 libipc>"


def killed_by(text: str, rc: int, expect: str) -> bool:
    """判定「期望用例被杀死」。三种合法形态（任一成立即算）：

      ① 明确 FAILED：`[  FAILED  ] Suite.Test`
      ② 单跑超时（rc=124）且日志里该用例**已开始执行**（`[ RUN ] Suite.Test`）——
         变异导致挂死，是"被杀"的形态之一。
      ③ 进程被信号/abort 带走（rc<0，含 SIGABRT=-6、SIGSEGV=-11）且该用例已开始
         执行 —— 断言构建或计数错乱导致 terminate。
    只认 FAILED 行会把 ②③ 漏成"未杀死"，把有效的守门判成不承重。"""
    for m in re.finditer(r"^\[  FAILED  \] (\S+)", text, re.M):
        if "." in m.group(1) and expect in m.group(1):
            return True
    started = re.search(rf"^\[ RUN      \] \S*{re.escape(expect)}\b", text, re.M) is not None
    if started and (rc == 124 or rc < 0):
        return True
    return False


def red_set(text: str) -> list[str]:
    """从 gtest 输出里取红集（FAILED 的用例全名）。

    只收形如 `Suite.Test` 的行：gtest 的收尾摘要里还有
    `[  FAILED  ] 1 test, listed below:` 这类行，它的第二列是**计数**不是用例名，
    收进来会让"红集"里混进数字（并可能把「期望被杀」的子串匹配判错）。"""
    out = []
    for m in re.finditer(r"^\[  FAILED  \] (\S+)", text, re.M):
        name = m.group(1)
        if "." not in name:
            continue
        if name not in out:
            out.append(name)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=None, help="只跑某个臂（如 X1）")
    ap.add_argument("--keep", action="store_true", help="保留 /tmp 副本")
    ap.add_argument("--out", default="/tmp/stage2_mutation", help="日志目录")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    arms = [m for m in MUTANTS if args.only is None or m.arm == args.only]
    if not arms:
        print(f"没有匹配的臂: {args.only}", file=sys.stderr)
        return 2

    # 开工前记录工作树指纹（结束后复核零改动）。
    before = run(["git", "status", "--porcelain"], cwd=ROOT).stdout

    results = []
    overall_fail = False
    for m in arms:
        t0 = time.time()
        tmp = Path(tempfile.mkdtemp(prefix=f"stage2mut_{m.arm}_", dir="/tmp"))
        tree = tmp / "tree"
        print(f"\n===== {m.arm}: {m.file} =====")
        print(f"  副本: {tree}")
        copy_tree(tree)
        apply_mutation(tree, m)
        print(f"  变异已落上（marker={m.marker}）")

        blog = out_dir / f"{m.arm}.build.log"
        if not build_in(tree, blog):
            print(f"  ⛔ 变异体构建失败（见 {blog}）—— 该臂不成立")
            results.append({"arm": m.arm, "ok": False, "reason": "build_failed"})
            overall_fail = True
            continue

        libline = ldd_check(tree, m.suite)
        print(f"  实际加载: {libline}")
        if str(tree) not in libline:
            print("  ⛔ 未加载变异库 —— 结论无效")
            results.append({"arm": m.arm, "ok": False, "reason": "wrong_lib"})
            overall_fail = True
            continue

        # 跑两遍：全量（记录该变异**还**打破了什么 —— "多一条红"是重要信息），
        # 以及只跑期望用例（承重判定；避免别的用例先 abort/挂死把目标用例挡在后面）。
        tlog = out_dir / f"{m.arm}.test.log"
        rc, text = run_suite(tree, m.suite, tlog)
        reds = red_set(text)
        flog = out_dir / f"{m.arm}.filtered.log"
        frc, ftext = run_suite(tree, m.suite, flog,
                               gtest_filter=":".join("*" + e for e in m.expect))
        killed = [e for e in m.expect if killed_by(ftext, frc, e)]
        ok = len(killed) == len(m.expect)
        print(f"  全量: rc={rc}  红集={reds or '（无 FAILED 行）'}")
        print(f"  过滤({':'.join(m.expect)}): rc={frc}  "
              f"红集={red_set(ftext) or '（无 FAILED 行）'}")
        print(f"  期望被杀: {m.expect}")
        print(f"  判定: {'✅ 杀死' if ok else '⛔ 未杀死（该臂不承重）'}")
        if not ok:
            overall_fail = True
        results.append(
            {
                "arm": m.arm,
                "ok": ok,
                "suite": m.suite,
                "expect": m.expect,
                "killed": killed,
                "red_set": reds,
                "rc": rc,
                "filtered_rc": frc,
                "filtered_red_set": red_set(ftext),
                "lib": libline,
                "seconds": round(time.time() - t0, 1),
                "why": m.why,
            }
        )
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)

    after = run(["git", "status", "--porcelain"], cwd=ROOT).stdout
    if before != after:
        print("\n⛔ 工作树指纹在变异实验期间发生变化 —— 结论不可信", file=sys.stderr)
        overall_fail = True

    (out_dir / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n===== 汇总（明细 {out_dir / 'results.json'}）=====")
    for r in results:
        mark = "✅" if r.get("ok") else "⛔"
        print(f"  {mark} {r['arm']}: {r.get('reason', '杀死 ' + ','.join(r.get('killed', [])))}")
    print(f"工作树指纹前后一致: {'是' if before == after else '否'}")
    return 1 if overall_fail else 0


if __name__ == "__main__":
    sys.exit(main())
