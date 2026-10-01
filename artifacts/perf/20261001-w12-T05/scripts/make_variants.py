#!/usr/bin/env python3
# T05 / W12 §7.3.3 —— 五库矩阵的源码变体生成（唯一改动面 = src/libipc/ipc.cpp 的
# reclaim 相关临界区）。⛔ 本脚本只写 run 目录，不触碰产品树。
import hashlib
import os
import sys

RUN = os.path.dirname(os.path.abspath(__file__)) + "/.."
RUN = os.path.normpath(RUN)
SRC = "src/libipc/ipc.cpp"
OUT = os.path.join(RUN, "src")

HEAD = open(SRC).read()
HEAD_SHA = hashlib.sha256(HEAD.encode()).hexdigest()
print("HEAD ipc.cpp sha256 =", HEAD_SHA)

# ── 变体 1：BASELINE = e800ccc 的 ipc.cpp（尚不存在 reclaim_orphan_segment）──
base_path = os.path.join(OUT, "ipc.cpp.BASELINE")
BASE = open(base_path).read()
assert "reclaim_orphan_segment" not in BASE, "BASELINE 不应含 reclaim_orphan_segment"
print("BASELINE sha256      =", hashlib.sha256(BASE.encode()).hexdigest())

# ── 变体 2：FIX = 当前 HEAD 原文（基准臂，逐字复制以便归档）──
open(os.path.join(OUT, "ipc.cpp.FIX"), "w").write(HEAD)
print("FIX      sha256      =", HEAD_SHA)

# ── 变体 3：ABLATE = 删除 reclaim_orphan_segment 调用 ──
abl_call = """        if (newly_attached)
        {
          (void)reclaim_orphan_segment(info, pref, chunk_size, "attach",
                                       orphan_candidate, &orphan_snap);
        }
"""
assert HEAD.count(abl_call) == 1, "ABLATE 锚点不唯一：%d" % HEAD.count(abl_call)
abl_new = """        if (newly_attached)
        {
          /* T05-ABLATE 实验臂：reclaim_orphan_segment() 调用被删除（函数体保留但不再被调用）
           * ⇒ 与"无 reclaim"逐字等价；其余临界区保持 HEAD 原样。 */
          (void)orphan_candidate;
          (void)orphan_snap;
        }
"""
ABL = HEAD.replace(abl_call, abl_new)
assert "reclaim_orphan_segment(info, pref, chunk_size" not in ABL
open(os.path.join(OUT, "ipc.cpp.ABLATE"), "w").write(ABL)
print("ABLATE   sha256      =", hashlib.sha256(ABL.encode()).hexdigest())

# ── 变体 4：V2NEG = 快照移到 handles_ 的 lock_ 之外 ──
snap_in_lock = """          if (newly_attached)
          {
            auto *probe = static_cast<chunk_info_t *>(h->get());
            if (probe != nullptr)
            {
              /* 锁序与热路径一致：handles_.lock_ → info->lock_（见 acquire_storage）。 */
              probe->lock_.lock();
              bool const pristine = probe->pool_.invalid();
              if (!pristine)
              {
                std::memcpy(&orphan_snap, &probe->pool_, sizeof(orphan_snap));
                orphan_candidate = true;
              }
              probe->lock_.unlock();
            }
          }
        }
"""
assert HEAD.count(snap_in_lock) == 1, "V2NEG 锚点不唯一：%d" % HEAD.count(snap_in_lock)
snap_out_lock = """        }
        /* T05-V2NEG 实验臂：把「素净判定 + 空闲链快照」移到 handles_ 的 lock_ **之外**
         * （= T73 之前的写法）。判据：同进程另一线程可在锁释放瞬间借出 id#0，而随后的
         * 字节镜像复核看到的正是被改动过的池 ⇒ 复核通过 ⇒ 复位把在飞借样重新发出。 */
        if (newly_attached)
        {
          auto *probe = static_cast<chunk_info_t *>(h->get());
          if (probe != nullptr)
          {
            probe->lock_.lock();
            bool const pristine = probe->pool_.invalid();
            if (!pristine)
            {
              std::memcpy(&orphan_snap, &probe->pool_, sizeof(orphan_snap));
              orphan_candidate = true;
            }
            probe->lock_.unlock();
          }
        }
"""
V2NEG = HEAD.replace(snap_in_lock, snap_out_lock)
assert V2NEG.count("T05-V2NEG") == 1
open(os.path.join(OUT, "ipc.cpp.V2NEG"), "w").write(V2NEG)
print("V2NEG    sha256      =", hashlib.sha256(V2NEG.encode()).hexdigest())

# ── 变体 5：FIXNEG = 保留 reclaim 调用与 handles_ 临界区内取快照，但破坏 info->lock_
#    下的复核临界区（删除 memcmp 复核 ⇒ 复位变成无条件）──
recheck = """    info->lock_.lock();
    if (std::memcmp(&snap, &info->pool_, sizeof(snap)) != 0)
    {
      info->lock_.unlock();
      return false;
    }
    info->pool_.reset_free_chain();
    info->lock_.unlock();
"""
assert HEAD.count(recheck) == 1, "FIXNEG 锚点不唯一：%d" % HEAD.count(recheck)
no_recheck = """    info->lock_.lock();
    /* T05-FIXNEG 实验臂：reclaim 调用与 handles_ 临界区内的快照都保留，
     * 但删除 info->lock_ 下的 memcmp 复核 ⇒「复核」这一临界区被破坏，
     * 复位对并发借出（在飞借样）不再让路。 */
    (void)snap;
    info->pool_.reset_free_chain();
    info->lock_.unlock();
"""
FIXNEG = HEAD.replace(recheck, no_recheck)
assert "memcmp(&snap" not in FIXNEG
open(os.path.join(OUT, "ipc.cpp.FIXNEG"), "w").write(FIXNEG)
print("FIXNEG   sha256      =", hashlib.sha256(FIXNEG.encode()).hexdigest())

# ── 机械核对：五个变体的差异只应落在 reclaim 相关行上 ──
import difflib
for name, text in (("ABLATE", ABL), ("V2NEG", V2NEG), ("FIXNEG", FIXNEG)):
    d = list(difflib.unified_diff(HEAD.splitlines(), text.splitlines(),
                                  "HEAD", name, lineterm="", n=1))
    diff = [l for l in d if l[:1] in "+-" and l[:3] not in ("+++", "---")]
    print("%s vs HEAD 差异行数 = %d" % (name, len(diff)))

# ── 反向验证：把三个负控臂的改法**逐字还原** ⇒ 必须得到 HEAD 原文 ──
def restore(text, marker, replacement_of_marker, original):
    assert text.count(marker) == 1, "还原锚点不唯一：%s" % marker[:40]
    out = text.replace(marker, original)
    assert marker not in out
    return out

# ABLATE 的调用块还原
out_abl = ABL.replace(abl_new, abl_call)
# V2NEG 的锁外块还原
out_v2 = V2NEG.replace(snap_out_lock, snap_in_lock)
# FIXNEG 的复核块还原
out_fn = FIXNEG.replace(no_recheck, recheck)
for name, out in (("ABLATE_REV", out_abl), ("V2NEG_REV", out_v2), ("FIXNEG_REV", out_fn)):
    same = (out == HEAD)
    print("%s == HEAD 逐字还原: %s" % (name, same))
    assert same, name + " 还原后与 HEAD 不一致"
    open(os.path.join(OUT, "ipc.cpp." + name), "w").write(out)
    print("%s sha256 = %s" % (name, hashlib.sha256(out.encode()).hexdigest()))

print("OK: 变体全部生成于", OUT)
