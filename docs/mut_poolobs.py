#!/usr/bin/env python3
"""步骤① 修复的变异验证驱动。

对每条变异臂: 备份 → 改写 → 重建 → 跑 → 记录**红集**(具体哪条断言红) → 还原。
⛔ 两条硬要求(见 memory: expected-red-must-be-machine-checkable):
  - 变异没落上(字符串没找到) = 脚本错误, 不是"臂不承重", 必须显式报出来;
  - 臂若跑全绿 = 该臂不承重, exit 1。
记红集而不只记 FAIL: 裸 FAIL 与"真验收 FAIL"同形, 必被误引。
"""
import hashlib, os, re, shutil, subprocess, sys

ROOT = "/home/zwc/cpp_ipc_dds"
IPC = f"{ROOT}/src/libipc/ipc.cpp"
TEST = f"{ROOT}/test/test_pool_exhaust_observability.cpp"
BIN = f"{ROOT}/build/bin/test_pool_exhaust_observability"

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

def build_run():
    b = subprocess.run(["cmake", "--build", "build", "-j8",
                        "--target", "test_pool_exhaust_observability"],
                       cwd=ROOT, capture_output=True, text=True)
    if b.returncode != 0:
        return None, "BUILD FAILED:\n" + b.stdout[-2000:] + b.stderr[-2000:]
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{ROOT}/build/lib:{ROOT}/build/bin"
    r = subprocess.run([BIN], cwd=ROOT, capture_output=True, text=True, env=env)
    return r, r.stdout + r.stderr

def red_set(out):
    """gtest 的失败行形如  <file>:<line>: Failure"""
    return sorted({m for m in re.findall(r"(\S+:\d+): Failure", out)})

def failed_tests(out):
    return sorted(set(re.findall(r"\[  FAILED  \] \S+\.(\w+)", out)))

# (臂名, 目标文件, 原文, 替换文)
ARMS = [
    ("A_删掉上报调用", IPC,
     "      note_pool_exhausted(kind, chunk_size, size, inf->prefix_);",
     "      /*MUT-A*/;"),

    ("C_节流退回跨档共用一个计数", IPC,
     "(s.key.chunk_size == chunk_size) &&",
     "(true) &&"),

    ("D_prefix_硬编码空串", IPC,
     "static_cast<unsigned long long>(n), prefix.c_str(),",
     'static_cast<unsigned long long>(n), "",'),

    ("E_空前缀点明句改为无条件", IPC,
     "                 prefix.empty()",
     "                 true"),

    ("B_阴性窗口内注入一次真耗尽", TEST,
     '    std::fprintf(stderr, "%s\\n", kProbe);\n',
     '    std::fprintf(stderr, "%s\\n", kProbe);\n'
     '    /*MUT-B*/ {\n'
     '        const std::string mn = "pool_obs_cycle_mut";\n'
     '        ipc::route::clear_storage(mn.c_str());\n'
     '        reset_chunk_pool("", 6144);   // payload 5120 -> align1K(5136) = 6144\n'
     '        ipc::route mtx{mn.c_str(), ipc::sender};\n'
     '        ipc::route mrx{mn.c_str(), ipc::receiver};\n'
     '        ASSERT_TRUE(mtx.wait_for_recv(1, 2000));\n'
     '        const auto mp = make_payload(5120, 0x11);\n'
     '        for (int i = 0; i < kChunkPoolSize + 1; ++i)\n'
     '            ASSERT_TRUE(mtx.send(mp.data(), mp.size()));\n'
     '    }\n'),
]

orig = {IPC: open(IPC).read(), TEST: open(TEST).read()}
fp0 = {p: md5(p) for p in (IPC, TEST)}
print("== 基线(未变异)指纹 ==")
for p, m in fp0.items():
    print(f"   {m}  {os.path.relpath(p, ROOT)}")

results = []
try:
    for name, path, old, new in ARMS:
        src = open(path).read()
        if src.count(old) != 1:
            print(f"\n!! {name}: 变异未落上(匹配 {src.count(old)} 次) —— 脚本错误, 非臂结论")
            results.append((name, "变体未落上", None, None))
            continue
        open(path, "w").write(src.replace(old, new))
        r, out = build_run()
        if r is None:
            print(f"\n!! {name}: {out}")
            results.append((name, "构建失败", None, None))
        else:
            ft, rs = failed_tests(out), red_set(out)
            print(f"\n== 臂 {name} ==  exit={r.returncode}")
            print(f"   红用例: {ft if ft else '（无 —— 该臂不承重!）'}")
            print(f"   红集({len(rs)}): {rs if rs else '（空）'}")
            results.append((name, ft, rs, r.returncode))
        open(path, "w").write(orig[path])          # 还原
finally:
    for p in (IPC, TEST):
        open(p, "w").write(orig[p])

fp1 = {p: md5(p) for p in (IPC, TEST)}
print("\n== 还原后指纹 ==")
ok = True
for p in (IPC, TEST):
    same = fp0[p] == fp1[p]
    ok &= same
    print(f"   {'✅' if same else '⛔'} {fp1[p]}  {os.path.relpath(p, ROOT)}"
          f"{'' if same else '  ← 与基线不一致!'}")

print("\n== 汇总 ==")
for name, ft, rs, rc in results:
    print(f"   {name:34s} 红用例={ft}")

# 承重判据: 每条臂都必须真的转红; 跑全绿 = 不承重
bad = [n for n, ft, rs, rc in results if not ft or "变体未落上" in str(ft) or "构建失败" in str(ft)]
print("\n" + ("✅ 所有臂都转红(均承重)" if (ok and not bad) else f"⛔ 问题: {bad} / 还原={ok}"))
sys.exit(0 if (ok and not bad) else 1)
