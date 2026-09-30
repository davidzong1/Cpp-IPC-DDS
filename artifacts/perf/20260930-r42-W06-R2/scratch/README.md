# scratch 探针（诊断用，非主判据）

主判据在上一级：`../negative_control_scan_wiring.log`（缺口 1）与 `../minimal/`（缺口 2/3）。
本目录只放**定位过程中**用到的两个一次性探针源码，供复核者自行重跑（来源 `build/r42/`，未进 CMake）：

- `probe_unreg2.cpp` —— 复现「断发布者会**把 route 从池里摘掉**」这一事实：
  连续采样 `wait_wakeups/wait_timeouts/route_count`。用途：证明第一版用例 3 的"注销未唤醒"
  是**假红**（前提不成立），从而确立"必须先重连再注销"的修法（交付文档 §2.2 的加注）。
  构建与运行：
  ```
  g++ -std=c++17 -O2 -DNDEBUG -I include -I src -I . -I 3rdparty scratch/probe_unreg2.cpp \
      -o /tmp/probe_unreg2 -L build/lib -lipc -lpthread -lrt -Wl,-rpath,$PWD/build/lib
  /tmp/probe_unreg2 8996
  ```
- `probe_backend.cpp` —— 早期后端注入探针（已被 `../minimal/case4*` 取代，保留仅作对照）。
