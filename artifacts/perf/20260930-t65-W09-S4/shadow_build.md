# 影子库构建说明（⛔ 均在 tmp/ 内，未改仓库源）

共享 objs（a0 平台层，用 gcc 编译）：
```
for T in ablate shadow atomic norecheck undersame sleep; do (cd tmp/t65/$T && mkdir -p objs && for f in src/libipc/platform/linux/a0/*.c; do gcc -O3 -std=gnu11 -fPIC -Iinclude -Isrc -Isrc/libipc/platform/linux -c $f -o objs/$(basename $f .c).o; done); done
```

每个变体的链接命令（以 ablate 为例）：
```
cd tmp/t65/ablate && g++ -O3 -std=c++17 -fPIC -shared -DNDEBUG \
  -DLIBIPC_LIBRARY_SHARED_BUILDING__ -DLIBIPC_LIBRARY_SHARED_USING__ \
  -Iinclude -Isrc -Isrc/libipc/platform/linux \
  $(ls src/libipc/*.cpp src/libipc/circ/*.cpp src/libipc/memory/*.cpp src/libipc/platform/*.cpp src/libipc/socket/*.cpp src/libipc/sync/*.cpp) \
  src/dzIPC/*.cc src/dzIPC/common/*.cc src/dzIPC/logger/*.cc src/dzIPC/threepools/*.cc objs/*.o \
  -o libipc_ablate.so -lpthread -lrt
```

各变体与主线的**唯一**差异：
  ablate   ：删掉 `reclaim_orphan_segment` 的调用点（3 行 → `(void)newly_attached;`）
  norecheck：只删「重锁后 memcmp 复核」（4 行 → 注释 + `(void)snap;`）
  atomic   ：把「素净判定」与「取快照」合并进**同一临界区**
  undersame：把 reclaim **整体移入** `handles_` 的那把 `lock_` 内
  sleep    ：在锁外窗口插入 `usleep(200000)`（仅用于把窗口放大成确定性复现）
  baseline ：源 = `ipc.cpp.HEAD`/`id_pool.h.HEAD` 逐字快照（独立重建）

用法：`LD_LIBRARY_PATH=<变体目录> <二进制>`（RUNPATH 会被 LD_LIBRARY_PATH 覆盖，已实测）。
