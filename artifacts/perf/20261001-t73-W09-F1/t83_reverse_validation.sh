#!/bin/bash
# t83 反向验证（判据的判据）：把 T73 修复"回退" ⇒ C9 必须变红。
# ⛔ 产品代码改动**只在本次调用内瞬态存在**：trap EXIT 保证无论如何都还原并重编。
# 用「同二进制 + 只换 libipc.so.3」口径 ⇒ 唯一变量 = 库。
set -u
cd /home/zwc/cpp_ipc_dds || exit 1
FIX_SRC=artifacts/perf/20261001-t73-W09-F1/ipc.cpp.T73FIX
NEGDIR=artifacts/perf/20261001-t73-W09-F1/lib_negative_t83
FIXDIR=artifacts/perf/20261001-t73-W09-F1/lib_fixed_t83
mkdir -p "$NEGDIR" "$FIXDIR"
ORIG=$(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1)
restore() {
  cp "$FIX_SRC" src/libipc/ipc.cpp
  make -C build -j16 ipc >/dev/null 2>&1
  printf 'RESTORED ipc.cpp sha256=%s (期望 %s) %s\n' \
    "$(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1 | cut -c1-16)" "$(echo $ORIG | cut -c1-16)" \
    "$([ "$(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1)" = "$ORIG" ] && echo OK || echo '⚠️MISMATCH')"
}
trap restore EXIT

FIXED=$(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1)
[ "$FIXED" = "$(sha256sum $FIX_SRC | cut -d' ' -f1)" ] || { echo "⚠️ 当前 ipc.cpp 与留档 FIX 不一致，中止"; exit 2; }

# ① 修复库（基线读数）
make -C build -j16 ipc >/dev/null 2>&1
cp build/lib/libipc.so.3 "$FIXDIR/libipc.so.3"; cp build/lib/libipc.so.3 "$FIXDIR/libipc.so"
echo "FIX   lib=$(sha256sum $FIXDIR/libipc.so.3 | cut -c1-16)"

# ② 负控库：把「素净判定 + 取快照」移出 handles_ 的 lock_（= T73 之前行为）
python3 - <<'PY'
p='src/libipc/ipc.cpp'
s=open(p).read()
fixed="""          if (newly_attached)
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
        }"""
neg="""        }
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
        }"""
assert s.count(fixed)==1, 'FIX 片段未唯一匹配：%d' % s.count(fixed)
open(p,'w').write(s.replace(fixed,neg))
print('negative patch applied')
PY
make -C build -j16 ipc >/dev/null 2>&1
cp build/lib/libipc.so.3 "$NEGDIR/libipc.so.3"; cp build/lib/libipc.so.3 "$NEGDIR/libipc.so"
echo "NEG   lib=$(sha256sum $NEGDIR/libipc.so.3 | cut -c1-16)"

BIN=build/bin/test_chunk_capacity_backpressure
echo
echo "--- 反向验证读数：同二进制、只换库、C9 ×1200 轮 ---"
for L in FIX:$FIXDIR NEG:$NEGDIR; do
  tag=${L%%:*}; d=${L#*:}
  for i in 1 2 3; do
    out=$(LD_LIBRARY_PATH=$d timeout 900 $BIN --gtest_filter='*OrphanReset*' 2>&1)
    line=$(echo "$out" | grep -oE 'W09-C9\].*' | head -1)
    res=$(echo "$out" | grep -oE '\[  (PASSED|FAILED)  \]|\[       OK \]|\[  FAILED  \] ChunkCapacity[^ ]*' | head -1)
    printf '  %-4s run%d  %s  %s\n' "$tag" "$i" "$res" "${line#*] }"
  done
done
