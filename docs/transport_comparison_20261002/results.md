# cpp_ipc_dds 与 CycloneDDS + iceoryx 对比测试总报告

## 执行方案与续跑节点

交付目录为 docs/transport_comparison_20261002（report/ 被仓库忽略，故已迁移）。本文件先作为执行方案，随测试更新，最终保留为测试结果报告。任务开始时仓库 git status --short 为空。仅新增本目录三份 Markdown 报告与 test/transport_comparison 下测试源码、脚本；构建、日志、逐样本与计算中间件放入仓库外的专属临时目录，完成汇总后清理。

| 节点 | 内容 | 状态 | 证据或说明 |
|---|---|---|---|
| P01 | 接口、环境、依赖预检 | 已完成 | 原安装：512 发布者、256 通知器、最大 1 MiB 池块；同版本扩容构建与私有命名空间已准备 |
| P02 | 测试源码和可续跑编排 | 已完成 | test/transport_comparison；独立 CMake、逐组合 JSON 断点与三轮编排 |
| P03 | Release 构建、正确性与路径验证 | 已完成 | 两套构建通过；四条路径验证；DDS 借样地址验证；1000 话题 4 发布线程短测均达到 100 万 msg/s |
| P04 | pub/sub 全尺寸三轮测量 | 已完成 | pub/sub 378/378 |
| P05 | ser-cli 全尺寸三轮测量 | 已完成 | ser-cli 378/378（含不支持项） |
| P06 | 1000 话题压力预检 | 已完成 | 同版本扩容 DDS 与 cpp_ipc_dds：各 1000 话题，4 发布线程，3 秒输入 300 万条 |
| P07 | 压力正式重复测量 | 已完成 | 原安装 27/27，扩容 27/27 |
| P08 | 数据校核与三份报告 | 已完成 | 三份报告及逐轮账本 |
| P09 | 中间产物清理与交付审计 | 已完成 | 3 份 Markdown 报告、11 份测试源码/脚本；中间数据已清理 |

本次交付验证：810 条正式记录的矩阵覆盖、二进制身份、角色输出来源、计数及路径证据审计通过；2 项编排回归检查通过。两套 Release 构建完成，脚本语法与本次修改的编辑器诊断通过。审计通过表示记录完整且可核对，产品异常及目标未达标仍按实测保留。

清理于 2026-10-02 完成：删除本次 5 个仓库外临时工作目录、7 份日志、运行锁及 1 个已确认无人使用的预检共享内存通道，包含构建、依赖下载、逐轮 JSON、作废记录和计算中间数据。关键数据已嵌入本报告；清理后如需原始样本须重新执行复现脚本。本次没有修改产品源码，宿主 RouDi（PID 1207）保持运行。

## 测量约定

- 所有数据来自本机两个独立进程，禁用 nodelet；串行执行各试验，避免基准互相争抢资源。记录实际 CPU、编译选项、版本、进程线程数、依赖路径。
- 逻辑载荷大小为 8 × 2^k 字节，k=0..17（上限 1,048,576 B）。协议元数据额外计算，不将逻辑大小冒充线上字节。
- 发布订阅覆盖 shm/TLV 基线、socket/TLV 基线、SHM DzFlatA、DzFlatB、PrebuiltSegment，以及 CycloneDDS UDP 和启用 iceoryx 两档。Tlv 与 shm 基线是同一实际路径，不重复制造一条独立结果。
- ser-cli 使用原生 shm/TLV、socket/TLV、SHM DZFlat 请求响应接口；DDS 使用请求/应答双话题实现同等 success 语义。原生 ser-cli 无 loan/publish_prebuilt_segment 入口，B 与 PrebuiltSegment 按尺寸逐项记为不支持，不伪造性能数据。
- DDS 使用固定尺寸 IDL 类型，检查实际共享内存可用性。启用 SHM 或发现 RouDi 进程本身不足以证明用户数据经 SHM 传输。
- 每个可运行组合重复三轮；预热不计入统计。发送耗时、订阅到达延迟与 RPC 往返耗时分列。吞吐以实际完成/窗口计算，失败和未收数独立列出。
- 压力默认 1000 个独立话题 × 每话题 1000 msg/s，64 B 逻辑载荷。发送计划独立于接收，记录计划/实际尝试/成功发送/成功接收/遗漏计划数与各话题覆盖情况；发送端未达标时不能据此断言订阅侧达不到目标。
- “0 线程”指 DDS 无额外应用接收工作线程，使用 DDS 原生通知/取样；DDS 内部线程与 RouDi 成本另计，不描述为整个系统零线程。cpp_ipc_dds 验证事件接收池实际启用并记录线程与兼容回退。
- 进度与完整结果随节点更新；脚本输出稳定 case id，已落盘完整结果支持跳过续跑。最终在 Markdown 保留逐组合汇总和关键证据后删除本次中间产物。

## 已知边界

当前会话未暴露 AGENTS.md 指定的 member_read_shared / member_send_message / member_report_result 工具，无法执行指定 MCP 团队回报；本任务直接执行并在本报告记录。当前未收到专门 leader 派单或成员身份。

## 执行修订记录

- 采用 rootless unshare 私有 /dev/shm、/tmp 与 IPC 命名空间；正式速度保持原安装的 DDS/iceoryx 二进制，只给隔离 RouDi 增加 2 MiB 池档，使 1 MiB 应用载荷及协议开销可被容纳。宿主机原配置的失败另存为预检证据。
- 1000 话题原容量边界另测；补充同版本源码测试构建，将发布者/订阅者与内部通知器容量改为 2048，整体重编 iceoryx/C binding/CycloneDDS，以免混用 ABI。该档与原安装成绩严格分列。
- 速度测试使用最多 8 条在途消息，吞吐是该窗口下的完成速率；并非无限队列的绝对峰值。RPC 为单在途 success 往返。
- 发端在计时结束后等待最多 1 秒排空，再析构端点，防止测试脚本提前销毁发端造成尾部丢失。
- DDS 发布侧借样指针已证实位于 /dev/shm 映射；接收 dds_take 返回堆内样本，因此只能证明共享内存发送路径，不能称端到端零拷贝。

- 压力采用两侧一致的 4 发布线程、1 接收进程：短测均发送 300 万条/3秒，DDS 收到 300 万条，cpp_ipc_dds 收到 2,999,998 条；该数仅为预检，不替代后续 10 秒 × 3 轮。单发布线程的 cpp_ipc_dds 约 62 万 msg/s 是发送器受限，故调整发布并发度。


- 编排审计发现一次速度/压力意外重叠（等待脚本未匹配 Python 绝对路径）。按两者运行时间区间将 244 个记录隔离作废；已加入跨命名空间全局 flock，正式报告仅使用后续串行重跑的记录。作废依据是时间区间，不是数值优劣。





- 断点审计发现 1 个预热失败格遗留了前次作废运行的角色 JSON；已按文件时间戳剔除旧统计，保留本次失败状态及日志。编排新增运行前删除旧角色输出、运行后核对输出时间戳，避免失败时误取旧结果。







## 正式执行结果

生成时间：2026-10-02T07:23:20+08:00。

| 节点 | 落盘记录 | 预计 |
|---|---|---|
| 速度 | 756 | 756 |
| 原安装压力 | 27 | 27 |
| 扩容压力 | 27 | 27 |


详见 [速度分析](speed_analysis.md) 和 [压力分析](stress_analysis.md)。容量失败、接口不支持、异常与无错误完成分别记录。

| 速度状态 | 轮次 |
|---|---|
| 完成 | 568 |
| 有异常 | 78 |
| 失败 | 2 |
| 接口不支持 | 108 |


## 环境与可重复性

环境 JSON 中的 sources 是测试运行当时的脚本快照，文末为最终交付源码指纹；后续只修订编排与报告。扩容脚本交付前由 build_scaled_deps.sh 重命名为 prepare_scaled_deps.sh，以避开仓库忽略规则，构建步骤未变。

### 原安装

~~~json
{
  "started": "2026-10-02T06:56:13+08:00",
  "git": "1d52fba1b25e5aad9de13a4abf742ceb7d786277",
  "git_status": "?? docs/transport_comparison_20261002/\n?? test/transport_comparison/",
  "uname": "Linux zwc-leju 6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC Fri Aug  7 13:43:15 UTC  x86_64 x86_64 x86_64 GNU/Linux",
  "cpu": "架构：                                   x86_64\nCPU 运行模式：                           32-bit, 64-bit\nAddress sizes:                           39 bits physical, 48 bits virtual\n字节序：                                 Little Endian\nCPU:                                     32\n在线 CPU 列表：                          0-31\n厂商 ID：                                GenuineIntel\n型号名称：                               Intel(R) Core(TM) i9-14900KF\nCPU 系列：                               6\n型号：                                   183\n每个核的线程数：                         2\n每个座的核数：                           24\n座：                                     1\n步进：                                   1\nCPU 最大 MHz：                           6000.0000\nCPU 最小 MHz：                           800.0000\nBogoMIPS：                               6374.40\n标记：                                   fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc art arch_perfmon pebs bts rep_good nopl xtopology nonstop_tsc cpuid aperfmperf tsc_known_freq pni pclmulqdq dtes64 monitor ds_cpl vmx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm abm 3dnowprefetch cpuid_fault epb ssbd ibrs ibpb stibp ibrs_enhanced tpr_shadow flexpriority ept vpid ept_ad fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid rdseed adx smap clflushopt clwb intel_pt sha_ni xsaveopt xsavec xgetbv1 xsaves split_lock_detect user_shstk avx_vnni dtherm ida arat pln pts hwp hwp_notify hwp_act_window hwp_epp hwp_pkg_req hfi vnmi umip pku ospke waitpkg gfni vaes vpclmulqdq rdpid movdiri movdir64b fsrm md_clear serialize arch_lbr ibt flush_l1d arch_capabilities ibpb_exit_to_user\n虚拟化：                                 VT-x\nL1d 缓存：                               896 KiB (24 instances)\nL1i 缓存：                               1.3 MiB (24 instances)\nL2 缓存：                                32 MiB (12 instances)\nL3 缓存：                                36 MiB (1 instance)\nNUMA 节点：                              1\nNUMA 节点0 CPU：                         0-31\nVulnerability Gather data sampling:      Not affected\nVulnerability Indirect target selection: Not affected\nVulnerability Itlb multihit:             Not affected\nVulnerability L1tf:                      Not affected\nVulnerability Mds:                       Not affected\nVulnerability Meltdown:                  Not affected\nVulnerability Mmio stale data:           Not affected\nVulnerability Reg file data sampling:    Mitigation; Clear Register File\nVulnerability Retbleed:                  Not affected\nVulnerability Spec rstack overflow:      Not affected\nVulnerability Spec store bypass:         Mitigation; Speculative Store Bypass disabled via prctl\nVulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers and __user pointer sanitization\nVulnerability Spectre v2:                Mitigation; Enhanced / Automatic IBRS; IBPB conditional; PBRSB-eIBRS SW sequence; BHI BHI_DIS_S\nVulnerability Srbds:                     Not affected\nVulnerability Tsa:                       Not affected\nVulnerability Tsx async abort:           Not affected\nVulnerability Vmscape:                   Mitigation; IBPB before exit to userspace",
  "memory": "total        used        free      shared  buff/cache   available\n内存：       62Gi        31Gi       3.4Gi       639Mi        27Gi        29Gi\n交换：       65Gi       6.5Gi        59Gi",
  "compiler": "c++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0\nCopyright (C) 2021 Free Software Foundation, Inc.\nThis is free software; see the source for copying conditions.  There is NO\nwarranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
  "loaded": "linux-vdso.so.1 (0x00007fffd4ffe000)\n\tlibipc.so.3 => /var/tmp/cppipc-comparison-current/build/lib/libipc.so.3 (0x0000738b0d7bb000)\n\tlibddsc.so.0 => /home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2/lib/libddsc.so.0 (0x0000738b0d400000)\n\tlibstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x0000738b0d000000)\n\tlibm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x0000738b0d6b8000)\n\tlibgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x0000738b0d698000)\n\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x0000738b0cc00000)\n\t/lib64/ld-linux-x86-64.so.2 (0x0000738b0d8e1000)\n\tlibpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0 (0x0000738b0d691000)\n\tlibrt.so.1 => /lib/x86_64-linux-gnu/librt.so.1 (0x0000738b0d68c000)\n\tlibdl.so.2 => /lib/x86_64-linux-gnu/libdl.so.2 (0x0000738b0d687000)",
  "binary_sha256": "213d8193332bc45f4d6f574c44f863e8007ff326dc89a2553a5332189053c0be",
  "library_sha256": "8c80d0ba30e99c60d63b9b903e6203f1c9d17f482d136d6d1c40d3f5eda81e15",
  "roudi": "1207 /opt/lejurobot/iceoryx/bin/iox-roudi -c /etc/iceoryx/roudi_config.toml\n2724910 /home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi -c /var/tmp/cppipc-comparison-current/roudi.toml",
  "roudi_config": "[general]\nversion = 1\n[[segment]]\n[[segment.mempool]]\nsize = 128\ncount = 10000\n[[segment.mempool]]\nsize = 1024\ncount = 5000\n[[segment.mempool]]\nsize = 16384\ncount = 1000\n[[segment.mempool]]\nsize = 131072\ncount = 200\n[[segment.mempool]]\nsize = 1048576\ncount = 50\n[[segment.mempool]]\nsize = 2097152\ncount = 50\n",
  "sources": {
    "comparison.cpp": "3220fcd055a84ac05e1b452f99413d6496a729c8e569c2ad1487730e7ec5d265",
    "report.py": "79dc647463e77ac3e6d22b6655d64abcc00e9645fc314c457d0f63e99be7369b",
    "run.py": "e16c711c7940c8e6954044bbdf7fd5398516b9826baaae6344d180855d5d0eaa",
    "prepare_roudi.py": "add6eaa0b87d73ed58de64fe6d25e6f0cff49c9634620e81a893ccbe6351c4ad",
    "execute_all.sh": "cb7d9338ad8eeafcfcde2c9ebf713784bc150039995f799fe205f8fb1bebd1ae",
    "CMakeLists.txt": "5a403e994b962d84f00862a97ca18976588c602aa72e89c925f9d0ad5bda6aba",
    "generate_types.py": "730ce23d40d6650adc39d8b0c0b2831868f778e9b9466722fe107873ff134efe",
    "isolated.sh": "37af643f72dd1278b39ba81f50d4dfa2094d62a7d8ef73dcd756593dd5349936",
    "build_scaled_deps.sh": "646eedea097e815580c0dc7440b9f991bee137560f91a46577ea0a624eaf0950"
  },
  "flags": "# CMAKE generated file: DO NOT EDIT!\n# Generated by \"Unix Makefiles\" Generator, CMake Version 3.22\n\n# compile C with /usr/bin/cc\n# compile CXX with /usr/bin/c++\nC_DEFINES = -DLIBIPC_LIBRARY_SHARED_BUILDING__ -Dipc_EXPORTS\n\nC_INCLUDES = -I/home/zwc/cpp_ipc_dds/include -I/home/zwc/cpp_ipc_dds/src -I/home/zwc/cpp_ipc_dds/src/libipc/platform/linux\n\nC_FLAGS = -O3 -DNDEBUG -fPIC -O3\n\nCXX_DEFINES = -DLIBIPC_LIBRARY_SHARED_BUILDING__ -Dipc_EXPORTS\n\nCXX_INCLUDES = -I/home/zwc/cpp_ipc_dds/include -I/home/zwc/cpp_ipc_dds/src -I/home/zwc/cpp_ipc_dds/src/libipc/platform/linux\n\nCXX_FLAGS = -O3 -DNDEBUG -fPIC -O3\n\n",
  "workers": 32,
  "domain": 173
}
~~~

### 同版本扩容

~~~json
{
  "started": "2026-10-02T07:05:27+08:00",
  "git": "1d52fba1b25e5aad9de13a4abf742ceb7d786277",
  "git_status": "?? docs/transport_comparison_20261002/\n?? test/transport_comparison/",
  "uname": "Linux zwc-leju 6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC Fri Aug  7 13:43:15 UTC  x86_64 x86_64 x86_64 GNU/Linux",
  "cpu": "架构：                                   x86_64\nCPU 运行模式：                           32-bit, 64-bit\nAddress sizes:                           39 bits physical, 48 bits virtual\n字节序：                                 Little Endian\nCPU:                                     32\n在线 CPU 列表：                          0-31\n厂商 ID：                                GenuineIntel\n型号名称：                               Intel(R) Core(TM) i9-14900KF\nCPU 系列：                               6\n型号：                                   183\n每个核的线程数：                         2\n每个座的核数：                           24\n座：                                     1\n步进：                                   1\nCPU 最大 MHz：                           6000.0000\nCPU 最小 MHz：                           800.0000\nBogoMIPS：                               6374.40\n标记：                                   fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc art arch_perfmon pebs bts rep_good nopl xtopology nonstop_tsc cpuid aperfmperf tsc_known_freq pni pclmulqdq dtes64 monitor ds_cpl vmx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm abm 3dnowprefetch cpuid_fault epb ssbd ibrs ibpb stibp ibrs_enhanced tpr_shadow flexpriority ept vpid ept_ad fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid rdseed adx smap clflushopt clwb intel_pt sha_ni xsaveopt xsavec xgetbv1 xsaves split_lock_detect user_shstk avx_vnni dtherm ida arat pln pts hwp hwp_notify hwp_act_window hwp_epp hwp_pkg_req hfi vnmi umip pku ospke waitpkg gfni vaes vpclmulqdq rdpid movdiri movdir64b fsrm md_clear serialize arch_lbr ibt flush_l1d arch_capabilities ibpb_exit_to_user\n虚拟化：                                 VT-x\nL1d 缓存：                               896 KiB (24 instances)\nL1i 缓存：                               1.3 MiB (24 instances)\nL2 缓存：                                32 MiB (12 instances)\nL3 缓存：                                36 MiB (1 instance)\nNUMA 节点：                              1\nNUMA 节点0 CPU：                         0-31\nVulnerability Gather data sampling:      Not affected\nVulnerability Indirect target selection: Not affected\nVulnerability Itlb multihit:             Not affected\nVulnerability L1tf:                      Not affected\nVulnerability Mds:                       Not affected\nVulnerability Meltdown:                  Not affected\nVulnerability Mmio stale data:           Not affected\nVulnerability Reg file data sampling:    Mitigation; Clear Register File\nVulnerability Retbleed:                  Not affected\nVulnerability Spec rstack overflow:      Not affected\nVulnerability Spec store bypass:         Mitigation; Speculative Store Bypass disabled via prctl\nVulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers and __user pointer sanitization\nVulnerability Spectre v2:                Mitigation; Enhanced / Automatic IBRS; IBPB conditional; PBRSB-eIBRS SW sequence; BHI BHI_DIS_S\nVulnerability Srbds:                     Not affected\nVulnerability Tsa:                       Not affected\nVulnerability Tsx async abort:           Not affected\nVulnerability Vmscape:                   Mitigation; IBPB before exit to userspace",
  "memory": "total        used        free      shared  buff/cache   available\n内存：       62Gi        31Gi       3.2Gi       724Mi        27Gi        29Gi\n交换：       65Gi       6.5Gi        59Gi",
  "compiler": "c++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0\nCopyright (C) 2021 Free Software Foundation, Inc.\nThis is free software; see the source for copying conditions.  There is NO\nwarranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.",
  "loaded": "linux-vdso.so.1 (0x00007ffc95795000)\n\tlibipc.so.3 => /var/tmp/cppipc-comparison-scaled/build/lib/libipc.so.3 (0x00007bdf992d7000)\n\tlibddsc.so.0 => /var/tmp/cppipc-comparison-scaled-deps/prefix/lib/libddsc.so.0 (0x00007bdf99000000)\n\tlibstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x00007bdf98c00000)\n\tlibm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x00007bdf98f19000)\n\tlibgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x00007bdf9929b000)\n\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007bdf98800000)\n\t/lib64/ld-linux-x86-64.so.2 (0x00007bdf993fd000)",
  "binary_sha256": "92231ba5c8c9cd0c4ee314b4a2c14826373ae42099d9dc9931849eec758032db",
  "library_sha256": "8c80d0ba30e99c60d63b9b903e6203f1c9d17f482d136d6d1c40d3f5eda81e15",
  "roudi": "1207 /opt/lejurobot/iceoryx/bin/iox-roudi -c /etc/iceoryx/roudi_config.toml\n2742013 /var/tmp/cppipc-comparison-scaled-deps/prefix/bin/iox-roudi -c /var/tmp/cppipc-comparison-scaled/roudi.toml",
  "roudi_config": "[general]\nversion = 1\n[[segment]]\n[[segment.mempool]]\nsize = 128\ncount = 10000\n[[segment.mempool]]\nsize = 1024\ncount = 5000\n[[segment.mempool]]\nsize = 16384\ncount = 1000\n[[segment.mempool]]\nsize = 131072\ncount = 200\n[[segment.mempool]]\nsize = 1048576\ncount = 50\n[[segment.mempool]]\nsize = 2097152\ncount = 50\n",
  "sources": {
    "comparison.cpp": "3220fcd055a84ac05e1b452f99413d6496a729c8e569c2ad1487730e7ec5d265",
    "report.py": "79dc647463e77ac3e6d22b6655d64abcc00e9645fc314c457d0f63e99be7369b",
    "run.py": "7352f0fbd946cbe9cd2699fbaacd0983adad4a6eb8a09eb7be80b03d4eb46355",
    "prepare_roudi.py": "add6eaa0b87d73ed58de64fe6d25e6f0cff49c9634620e81a893ccbe6351c4ad",
    "execute_all.sh": "cb7d9338ad8eeafcfcde2c9ebf713784bc150039995f799fe205f8fb1bebd1ae",
    "CMakeLists.txt": "5a403e994b962d84f00862a97ca18976588c602aa72e89c925f9d0ad5bda6aba",
    "generate_types.py": "730ce23d40d6650adc39d8b0c0b2831868f778e9b9466722fe107873ff134efe",
    "isolated.sh": "37af643f72dd1278b39ba81f50d4dfa2094d62a7d8ef73dcd756593dd5349936",
    "build_scaled_deps.sh": "646eedea097e815580c0dc7440b9f991bee137560f91a46577ea0a624eaf0950"
  },
  "flags": "# CMAKE generated file: DO NOT EDIT!\n# Generated by \"Unix Makefiles\" Generator, CMake Version 3.22\n\n# compile C with /usr/bin/cc\n# compile CXX with /usr/bin/c++\nC_DEFINES = -DLIBIPC_LIBRARY_SHARED_BUILDING__ -Dipc_EXPORTS\n\nC_INCLUDES = -I/home/zwc/cpp_ipc_dds/include -I/home/zwc/cpp_ipc_dds/src -I/home/zwc/cpp_ipc_dds/src/libipc/platform/linux\n\nC_FLAGS = -O3 -DNDEBUG -fPIC -O3\n\nCXX_DEFINES = -DLIBIPC_LIBRARY_SHARED_BUILDING__ -Dipc_EXPORTS\n\nCXX_INCLUDES = -I/home/zwc/cpp_ipc_dds/include -I/home/zwc/cpp_ipc_dds/src -I/home/zwc/cpp_ipc_dds/src/libipc/platform/linux\n\nCXX_FLAGS = -O3 -DNDEBUG -fPIC -O3\n\n",
  "workers": 32,
  "domain": 173
}
~~~


源码归档 SHA-256：

~~~text
bf6de70e3edee71223f993a29bff5e61af95ce4871104929d8bd1729f544bafb  /var/tmp/cppipc-comparison-scaled-deps/sources/iceoryx.tar.gz
bc84e137e0c8a055b8cd97fbeafec94e36de1b0c2e88800896a82384fd867ae5  /var/tmp/cppipc-comparison-scaled-deps/sources/cyclonedds.tar.gz
~~~


作废重叠区间 Unix 秒：1790893973.2694936—1790894320.120663，共 244 条记录；audit.py 检查最终保留速度记录均不在该区间内。


## 原始内存池边界预检

使用正式工装和原安装依赖，在隔离命名空间保留最大 1 MiB 池块。预检得到原始上限失败；正式速度只增加 2 MiB 池档，未修改原安装二进制。

| 方式 | 载荷 | 状态 | 证据 |
|---|---|---|---|
| pubsub | 1048576 | 失败 | RouDi 池块不足以容纳载荷及协议开销 |
| rpc | 1048576 | 失败 | RouDi 池块不足以容纳载荷及协议开销 |


~~~text
2026-10-02 07:13:05.243 [ Fatal ]: The following mempools are available:  MemPool [ ChunkSize = 168, ChunkPayloadSize = 128, ChunkCount = 10000 ]  MemPool [ ChunkSize = 1064, ChunkPayloadSize = 1024, ChunkCount = 5000 ]  MemPool [ ChunkSize = 16424, ChunkPayloadSize = 16384, ChunkCount = 1000 ]  MemPool [ ChunkSize = 131112, ChunkPayloadSize = 131072, ChunkCount = 200 ]  MemPool [ ChunkSize = 1048616, ChunkPayloadSize = 1048576, ChunkCount = 50 ]Could not find a fitting mempool for a chunk of size 1048696
~~~


## 复现

~~~bash
bash test/transport_comparison/execute_all.sh /var/tmp/my-ipc-comparison
~~~

run.py 支持按 case 续跑，会核对二进制、时长与并发参数；覆盖需显式 --rerun。复现脚本默认保留中间数据供检查，本次交付在报告汇总审计后清理。


## 逐轮结果账本

将中间 JSON 的关键数据嵌入 Markdown，清理后仍可逐轮复核。RPC 枚举路径计数未接线，0 不表示未走 DZFlat，应看成功计数与请求/响应 view。view 和 DDS 指针抽样计数含预热，仅用于路径证明；DZFlat 关闭时 fallback 也计正常 TLV，不能一律当作失败。

| 配置 | case id | 状态 | 计划 | 尝试 | 发/请求成功 | 收 | 窗内收 | 发失败 | 坏样本 | 重复 | p50µs | p99µs | 计划窗 s | 发实耗 s | 收观测 s | 发CPU s | 收CPU s | 发/收线程 | TLV/A/B/Prebuilt | DZFlat/回退 | 请求/响应view | DDS发SHM/收SHM/收堆 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 原安装速度 | speed-pubsub-a-1024-n1-r1 | 完成 | 1192182 | 1192182 | 1192182 | 1192182 | 1192175 | 0 | 0 | 0 | 6.33 | 7.65 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1192182/0/0 | 1192182/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-1024-n1-r2 | 完成 | 1113954 | 1113954 | 1113954 | 1113954 | 1113947 | 0 | 0 | 0 | 6.77 | 7.89 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1113954/0/0 | 1113954/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-1024-n1-r3 | 完成 | 1191200 | 1191200 | 1191200 | 1191200 | 1191192 | 0 | 0 | 0 | 6.24 | 7.38 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1191200/0/0 | 1191200/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-1048576-n1-r1 | 完成 | 31660 | 31660 | 31660 | 31660 | 31659 | 0 | 0 | 0 | 32.91 | 55.74 | 1.0 | 1.000013 | 1.500000 | 1.00 | 1.60 | 2/3 | 0/31660/0/0 | 31660/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-1048576-n1-r2 | 完成 | 31174 | 31174 | 31174 | 31174 | 31173 | 0 | 0 | 0 | 35.58 | 47.93 | 1.0 | 1.000023 | 1.500000 | 1.00 | 1.66 | 2/3 | 0/31174/0/0 | 31174/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-1048576-n1-r3 | 完成 | 31961 | 31961 | 31961 | 31961 | 31959 | 0 | 0 | 0 | 34.95 | 40.10 | 1.0 | 1.000029 | 1.500000 | 1.00 | 1.66 | 2/3 | 0/31961/0/0 | 31961/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-128-n1-r1 | 完成 | 1121755 | 1121755 | 1121755 | 1121755 | 1121747 | 0 | 0 | 0 | 6.68 | 11.64 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1121755/0/0 | 1121755/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-128-n1-r2 | 完成 | 1113061 | 1113061 | 1113061 | 1113061 | 1113053 | 0 | 0 | 0 | 6.82 | 7.95 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/1113061/0/0 | 1113061/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-128-n1-r3 | 完成 | 1159116 | 1159116 | 1159116 | 1159116 | 1159108 | 0 | 0 | 0 | 6.51 | 8.28 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1159116/0/0 | 1159116/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-131072-n1-r1 | 完成 | 232951 | 232951 | 232951 | 232951 | 232950 | 0 | 0 | 0 | 5.93 | 10.56 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.10 | 2/3 | 0/232951/0/0 | 232951/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-131072-n1-r2 | 完成 | 235888 | 235888 | 235888 | 235888 | 235887 | 0 | 0 | 0 | 5.84 | 7.72 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.07 | 2/3 | 0/235888/0/0 | 235888/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-131072-n1-r3 | 完成 | 231779 | 231779 | 231779 | 231779 | 231777 | 0 | 0 | 0 | 6.08 | 9.48 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.18 | 2/3 | 0/231779/0/0 | 231779/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16-n1-r1 | 完成 | 1160060 | 1160060 | 1160060 | 1160060 | 1160052 | 0 | 0 | 0 | 6.56 | 7.85 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1160060/0/0 | 1160060/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16-n1-r2 | 完成 | 1172396 | 1172396 | 1172396 | 1172396 | 1172388 | 0 | 0 | 0 | 6.48 | 7.55 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1172396/0/0 | 1172396/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16-n1-r3 | 完成 | 1178294 | 1178294 | 1178294 | 1178294 | 1178286 | 0 | 0 | 0 | 6.30 | 11.45 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1178294/0/0 | 1178294/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16384-n1-r1 | 完成 | 962555 | 962555 | 962555 | 962555 | 962553 | 0 | 0 | 0 | 1.81 | 3.66 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/962555/0/0 | 962555/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16384-n1-r2 | 完成 | 965549 | 965549 | 965549 | 965549 | 965547 | 0 | 0 | 0 | 1.80 | 3.56 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/965549/0/0 | 965549/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-16384-n1-r3 | 完成 | 952375 | 952375 | 952375 | 952375 | 952373 | 0 | 0 | 0 | 1.77 | 2.88 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/952375/0/0 | 952375/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-2048-n1-r1 | 完成 | 1163449 | 1163449 | 1163449 | 1163449 | 1163442 | 0 | 0 | 0 | 6.28 | 12.01 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1163449/0/0 | 1163449/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-2048-n1-r2 | 完成 | 1102313 | 1102313 | 1102313 | 1102313 | 1102306 | 0 | 0 | 0 | 6.57 | 7.85 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1102313/0/0 | 1102313/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-2048-n1-r3 | 完成 | 1157875 | 1157875 | 1157875 | 1157875 | 1157867 | 0 | 0 | 0 | 6.38 | 7.58 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1157875/0/0 | 1157875/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-256-n1-r1 | 完成 | 1152772 | 1152772 | 1152772 | 1152772 | 1152764 | 0 | 0 | 0 | 6.42 | 11.93 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1152772/0/0 | 1152772/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-256-n1-r2 | 完成 | 1123875 | 1123875 | 1123875 | 1123875 | 1123868 | 0 | 0 | 0 | 6.60 | 12.07 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1123875/0/0 | 1123875/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-256-n1-r3 | 完成 | 1209488 | 1209488 | 1209488 | 1209488 | 1209484 | 0 | 0 | 0 | 6.00 | 6.95 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1209488/0/0 | 1209488/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-262144-n1-r1 | 完成 | 133327 | 133327 | 133327 | 133327 | 133325 | 0 | 0 | 0 | 9.50 | 13.28 | 1.0 | 1.000007 | 1.500000 | 1.00 | 1.97 | 2/3 | 0/133327/0/0 | 133327/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-262144-n1-r2 | 完成 | 131835 | 131835 | 131835 | 131835 | 131832 | 0 | 0 | 0 | 9.50 | 13.87 | 1.0 | 1.000003 | 1.500000 | 1.00 | 1.94 | 2/3 | 0/131835/0/0 | 131835/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-262144-n1-r3 | 完成 | 134170 | 134170 | 134170 | 134170 | 134168 | 0 | 0 | 0 | 9.38 | 12.48 | 1.0 | 1.000006 | 1.500000 | 1.00 | 1.95 | 2/3 | 0/134170/0/0 | 134170/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32-n1-r1 | 完成 | 1226606 | 1226606 | 1226606 | 1226606 | 1226598 | 0 | 0 | 0 | 5.93 | 7.71 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1226606/0/0 | 1226606/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32-n1-r2 | 完成 | 1205177 | 1205177 | 1205177 | 1205177 | 1205169 | 0 | 0 | 0 | 6.23 | 7.34 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1205177/0/0 | 1205177/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32-n1-r3 | 完成 | 1133395 | 1133395 | 1133395 | 1133395 | 1133388 | 0 | 0 | 0 | 6.63 | 7.83 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1133395/0/0 | 1133395/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32768-n1-r1 | 完成 | 590594 | 590594 | 590594 | 590594 | 590592 | 0 | 0 | 0 | 2.62 | 12.44 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.45 | 2/3 | 0/590594/0/0 | 590594/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32768-n1-r2 | 完成 | 596634 | 596634 | 596634 | 596634 | 596632 | 0 | 0 | 0 | 2.59 | 4.21 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.44 | 2/3 | 0/596634/0/0 | 596634/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-32768-n1-r3 | 完成 | 616048 | 616048 | 616048 | 616048 | 616046 | 0 | 0 | 0 | 2.60 | 4.24 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.46 | 2/3 | 0/616048/0/0 | 616048/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-4096-n1-r1 | 完成 | 1152380 | 1152380 | 1152380 | 1152380 | 1152372 | 0 | 0 | 0 | 6.41 | 12.05 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1152380/0/0 | 1152380/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-4096-n1-r2 | 完成 | 1144294 | 1144294 | 1144294 | 1144294 | 1144286 | 0 | 0 | 0 | 6.35 | 8.51 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1144294/0/0 | 1144294/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-4096-n1-r3 | 完成 | 1168973 | 1168973 | 1168973 | 1168973 | 1168967 | 0 | 0 | 0 | 6.30 | 7.45 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/1168973/0/0 | 1168973/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-512-n1-r1 | 完成 | 1095641 | 1095641 | 1095641 | 1095641 | 1095634 | 0 | 0 | 0 | 6.84 | 10.63 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1095641/0/0 | 1095641/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-512-n1-r2 | 完成 | 1149302 | 1149302 | 1149302 | 1149302 | 1149294 | 0 | 0 | 0 | 6.56 | 7.82 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1149302/0/0 | 1149302/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-512-n1-r3 | 完成 | 1113121 | 1113121 | 1113121 | 1113121 | 1113113 | 0 | 0 | 0 | 6.68 | 9.90 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1113121/0/0 | 1113121/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-524288-n1-r1 | 完成 | 72179 | 72179 | 72179 | 72179 | 72178 | 0 | 0 | 0 | 17.23 | 21.71 | 1.0 | 1.000007 | 1.500000 | 1.00 | 1.79 | 2/3 | 0/72179/0/0 | 72179/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-524288-n1-r2 | 完成 | 71853 | 71853 | 71853 | 71853 | 71852 | 0 | 0 | 0 | 15.78 | 18.93 | 1.0 | 1.000011 | 1.500000 | 1.00 | 1.72 | 2/3 | 0/71853/0/0 | 71853/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-524288-n1-r3 | 完成 | 71646 | 71646 | 71646 | 71646 | 71645 | 0 | 0 | 0 | 17.51 | 20.76 | 1.0 | 1.000005 | 1.500000 | 1.00 | 1.81 | 2/3 | 0/71646/0/0 | 71646/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-64-n1-r1 | 完成 | 1186632 | 1186632 | 1186632 | 1186632 | 1186625 | 0 | 0 | 0 | 6.27 | 10.81 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1186632/0/0 | 1186632/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-64-n1-r2 | 完成 | 1135587 | 1135587 | 1135587 | 1135587 | 1135579 | 0 | 0 | 0 | 6.61 | 7.79 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1135587/0/0 | 1135587/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-64-n1-r3 | 完成 | 1182853 | 1182853 | 1182853 | 1182853 | 1182845 | 0 | 0 | 0 | 6.33 | 7.46 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1182853/0/0 | 1182853/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-65536-n1-r1 | 完成 | 398800 | 398800 | 398800 | 398800 | 398799 | 0 | 0 | 0 | 4.03 | 6.05 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.29 | 2/3 | 0/398800/0/0 | 398800/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-65536-n1-r2 | 完成 | 401468 | 401468 | 401468 | 401468 | 401467 | 0 | 0 | 0 | 3.90 | 5.12 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.30 | 2/3 | 0/401468/0/0 | 401468/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-65536-n1-r3 | 完成 | 385986 | 385986 | 385986 | 385986 | 385984 | 0 | 0 | 0 | 4.03 | 5.60 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.28 | 2/3 | 0/385986/0/0 | 385986/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8-n1-r1 | 完成 | 1034746 | 1034746 | 1034746 | 1034746 | 1034739 | 0 | 0 | 0 | 7.40 | 8.84 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/1034746/0/0 | 1034746/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8-n1-r2 | 完成 | 1074701 | 1074701 | 1074701 | 1074701 | 1074693 | 0 | 0 | 0 | 7.08 | 8.91 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1074701/0/0 | 1074701/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8-n1-r3 | 完成 | 1095186 | 1095186 | 1095186 | 1095186 | 1095178 | 0 | 0 | 0 | 6.82 | 12.49 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1095186/0/0 | 1095186/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8192-n1-r1 | 完成 | 1172363 | 1172363 | 1172363 | 1172363 | 1172361 | 0 | 0 | 0 | 1.70 | 3.17 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1172363/0/0 | 1172363/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8192-n1-r2 | 完成 | 1119720 | 1119720 | 1119720 | 1119720 | 1119717 | 0 | 0 | 0 | 2.65 | 9.25 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1119720/0/0 | 1119720/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-a-8192-n1-r3 | 完成 | 1119365 | 1119365 | 1119365 | 1119365 | 1119362 | 0 | 0 | 0 | 1.71 | 5.60 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/1119365/0/0 | 1119365/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1024-n1-r1 | 完成 | 1149630 | 1149630 | 1149630 | 1149630 | 1149622 | 0 | 0 | 0 | 6.49 | 10.94 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1149630/0 | 1149630/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1024-n1-r2 | 完成 | 1118515 | 1118515 | 1118515 | 1118515 | 1118507 | 0 | 0 | 0 | 6.75 | 12.02 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1118515/0 | 1118515/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1024-n1-r3 | 完成 | 1148889 | 1148889 | 1148889 | 1148889 | 1148881 | 0 | 0 | 0 | 6.60 | 7.75 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1148889/0 | 1148889/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1048576-n1-r1 | 完成 | 69677 | 69677 | 69677 | 69677 | 69676 | 0 | 0 | 0 | 16.59 | 21.03 | 1.0 | 1.000007 | 1.500000 | 1.00 | 1.76 | 2/3 | 0/0/69677/0 | 69677/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1048576-n1-r2 | 完成 | 68352 | 68352 | 68352 | 68352 | 68351 | 0 | 0 | 0 | 17.55 | 30.85 | 1.0 | 1.000012 | 1.500000 | 1.00 | 1.78 | 2/3 | 0/0/68352/0 | 68352/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-1048576-n1-r3 | 完成 | 69608 | 69608 | 69608 | 69608 | 69607 | 0 | 0 | 0 | 18.38 | 22.88 | 1.0 | 1.000005 | 1.500000 | 1.00 | 1.85 | 2/3 | 0/0/69608/0 | 69608/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-128-n1-r1 | 完成 | 1167908 | 1167908 | 1167908 | 1167908 | 1167900 | 0 | 0 | 0 | 6.46 | 7.72 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1167908/0 | 1167908/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-128-n1-r2 | 完成 | 1119955 | 1119955 | 1119955 | 1119955 | 1119947 | 0 | 0 | 0 | 6.79 | 8.03 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1119955/0 | 1119955/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-128-n1-r3 | 完成 | 1130202 | 1130202 | 1130202 | 1130202 | 1130194 | 0 | 0 | 0 | 6.59 | 7.75 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1130202/0 | 1130202/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-131072-n1-r1 | 完成 | 412952 | 412952 | 412952 | 412952 | 412950 | 0 | 0 | 0 | 3.82 | 7.02 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.27 | 2/3 | 0/0/412952/0 | 412952/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-131072-n1-r2 | 完成 | 419923 | 419923 | 419923 | 419923 | 419922 | 0 | 0 | 0 | 3.74 | 5.63 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.30 | 2/3 | 0/0/419923/0 | 419923/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-131072-n1-r3 | 完成 | 417625 | 417625 | 417625 | 417625 | 417623 | 0 | 0 | 0 | 3.83 | 5.05 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.27 | 2/3 | 0/0/417625/0 | 417625/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16-n1-r1 | 完成 | 1178941 | 1178941 | 1178941 | 1178941 | 1178933 | 0 | 0 | 0 | 6.40 | 8.29 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1178941/0 | 1178941/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16-n1-r2 | 完成 | 1138563 | 1138563 | 1138563 | 1138563 | 1138556 | 0 | 0 | 0 | 6.69 | 7.91 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/0/1138563/0 | 1138563/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16-n1-r3 | 完成 | 1135300 | 1135300 | 1135300 | 1135300 | 1135292 | 0 | 0 | 0 | 6.70 | 7.82 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1135300/0 | 1135300/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16384-n1-r1 | 完成 | 1155899 | 1155899 | 1155899 | 1155899 | 1155897 | 0 | 0 | 0 | 1.61 | 12.26 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1155899/0 | 1155899/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16384-n1-r2 | 完成 | 1174202 | 1174202 | 1174202 | 1174202 | 1174200 | 0 | 0 | 0 | 1.53 | 3.30 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1174202/0 | 1174202/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-16384-n1-r3 | 完成 | 1169076 | 1169076 | 1169076 | 1169076 | 1169074 | 0 | 0 | 0 | 1.67 | 5.73 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1169076/0 | 1169076/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-2048-n1-r1 | 完成 | 1099400 | 1099400 | 1099400 | 1099400 | 1099392 | 0 | 0 | 0 | 6.75 | 12.03 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1099400/0 | 1099400/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-2048-n1-r2 | 完成 | 1022019 | 1022019 | 1022019 | 1022019 | 1022011 | 0 | 0 | 0 | 7.43 | 10.26 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/0/1022019/0 | 1022019/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-2048-n1-r3 | 完成 | 1124315 | 1124315 | 1124315 | 1124315 | 1124307 | 0 | 0 | 0 | 6.75 | 8.83 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1124315/0 | 1124315/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-256-n1-r1 | 完成 | 1114339 | 1114339 | 1114339 | 1114339 | 1114332 | 0 | 0 | 0 | 6.80 | 10.05 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1114339/0 | 1114339/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-256-n1-r2 | 完成 | 1094031 | 1094031 | 1094031 | 1094031 | 1094024 | 0 | 0 | 0 | 6.98 | 8.25 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1094031/0 | 1094031/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-256-n1-r3 | 完成 | 1075456 | 1075456 | 1075456 | 1075456 | 1075448 | 0 | 0 | 0 | 7.11 | 8.87 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1075456/0 | 1075456/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-262144-n1-r1 | 完成 | 252631 | 252631 | 252631 | 252631 | 252629 | 0 | 0 | 0 | 5.79 | 8.02 | 1.0 | 1.000004 | 1.500000 | 1.00 | 2.16 | 2/3 | 0/0/252631/0 | 252631/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-262144-n1-r2 | 有异常 | 106785 | 106785 | 106785 | 106777 | 106777 | 0 | 0 | 0 | 5.75 | 7.52 | 1.0 | 1.000000 | 2.000619 | 1.00 | 2.28 | 2/3 | 0/0/106785/0 | 106785/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-262144-n1-r3 | 完成 | 253633 | 253633 | 253633 | 253633 | 253632 | 0 | 0 | 0 | 5.75 | 8.05 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.16 | 2/3 | 0/0/253633/0 | 253633/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32-n1-r1 | 完成 | 1186996 | 1186996 | 1186996 | 1186996 | 1186988 | 0 | 0 | 0 | 6.34 | 8.32 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1186996/0 | 1186996/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32-n1-r2 | 完成 | 1179381 | 1179381 | 1179381 | 1179381 | 1179373 | 0 | 0 | 0 | 6.33 | 10.42 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1179381/0 | 1179381/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32-n1-r3 | 完成 | 1248529 | 1248529 | 1248529 | 1248529 | 1248521 | 0 | 0 | 0 | 5.99 | 7.50 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/0/1248529/0 | 1248529/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32768-n1-r1 | 完成 | 870461 | 870461 | 870461 | 870461 | 870459 | 0 | 0 | 0 | 1.93 | 3.51 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/870461/0 | 870461/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32768-n1-r2 | 有异常 | 2204 | 2204 | 2204 | 2196 | 2196 | 0 | 0 | 0 | 3.53 | 62.24 | 1.0 | 1.000000 | 2.000753 | 1.00 | 2.01 | 2/3 | 0/0/2204/0 | 2204/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-32768-n1-r3 | 完成 | 873142 | 873142 | 873142 | 873142 | 873140 | 0 | 0 | 0 | 1.94 | 3.53 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/873142/0 | 873142/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-4096-n1-r1 | 完成 | 1168589 | 1168589 | 1168589 | 1168589 | 1168581 | 0 | 0 | 0 | 6.35 | 7.56 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1168589/0 | 1168589/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-4096-n1-r2 | 完成 | 1048577 | 1048577 | 1048577 | 1048577 | 1048577 | 0 | 0 | 0 | 7.19 | 11.11 | 1.0 | 1.003024 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/0/1048577/0 | 1048577/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-4096-n1-r3 | 完成 | 1167141 | 1167141 | 1167141 | 1167141 | 1167133 | 0 | 0 | 0 | 6.45 | 7.85 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1167141/0 | 1167141/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-512-n1-r1 | 完成 | 1158804 | 1158804 | 1158804 | 1158804 | 1158796 | 0 | 0 | 0 | 6.46 | 7.81 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1158804/0 | 1158804/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-512-n1-r2 | 完成 | 1192264 | 1192264 | 1192264 | 1192264 | 1192256 | 0 | 0 | 0 | 6.32 | 7.65 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1192264/0 | 1192264/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-512-n1-r3 | 完成 | 1100900 | 1100900 | 1100900 | 1100900 | 1100892 | 0 | 0 | 0 | 6.91 | 8.50 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1100900/0 | 1100900/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-524288-n1-r1 | 完成 | 143549 | 143549 | 143549 | 143549 | 143548 | 0 | 0 | 0 | 8.83 | 18.71 | 1.0 | 1.000001 | 1.500000 | 1.00 | 1.98 | 2/3 | 0/0/143549/0 | 143549/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-524288-n1-r2 | 完成 | 144222 | 144222 | 144222 | 144222 | 144221 | 0 | 0 | 0 | 9.03 | 12.76 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.03 | 2/3 | 0/0/144222/0 | 144222/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-524288-n1-r3 | 完成 | 145020 | 145020 | 145020 | 145020 | 145019 | 0 | 0 | 0 | 8.85 | 12.42 | 1.0 | 1.000001 | 1.500000 | 1.00 | 1.99 | 2/3 | 0/0/145020/0 | 145020/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-64-n1-r1 | 完成 | 1177336 | 1177336 | 1177336 | 1177336 | 1177329 | 0 | 0 | 0 | 6.35 | 9.87 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1177336/0 | 1177336/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-64-n1-r2 | 完成 | 1181620 | 1181620 | 1181620 | 1181620 | 1181612 | 0 | 0 | 0 | 6.39 | 7.54 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1181620/0 | 1181620/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-64-n1-r3 | 完成 | 1149625 | 1149625 | 1149625 | 1149625 | 1149617 | 0 | 0 | 0 | 6.59 | 8.55 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1149625/0 | 1149625/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-65536-n1-r1 | 完成 | 647205 | 647205 | 647205 | 647205 | 647204 | 0 | 0 | 0 | 2.46 | 3.93 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.47 | 2/3 | 0/0/647205/0 | 647205/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-65536-n1-r2 | 完成 | 634981 | 634981 | 634981 | 634981 | 634980 | 0 | 0 | 0 | 2.49 | 4.61 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.47 | 2/3 | 0/0/634981/0 | 634981/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-65536-n1-r3 | 完成 | 649337 | 649337 | 649337 | 649337 | 649335 | 0 | 0 | 0 | 2.46 | 4.10 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.45 | 2/3 | 0/0/649337/0 | 649337/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8-n1-r1 | 完成 | 1164944 | 1164944 | 1164944 | 1164944 | 1164936 | 0 | 0 | 0 | 6.50 | 7.90 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1164944/0 | 1164944/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8-n1-r2 | 完成 | 1080038 | 1080038 | 1080038 | 1080038 | 1080031 | 0 | 0 | 0 | 7.02 | 12.46 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1080038/0 | 1080038/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8-n1-r3 | 完成 | 1159933 | 1159933 | 1159933 | 1159933 | 1159926 | 0 | 0 | 0 | 6.42 | 7.57 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1159933/0 | 1159933/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8192-n1-r1 | 完成 | 1192733 | 1192733 | 1192733 | 1192733 | 1192728 | 0 | 0 | 0 | 3.69 | 7.02 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1192733/0 | 1192733/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8192-n1-r2 | 完成 | 1095432 | 1095432 | 1095432 | 1095432 | 1095424 | 0 | 0 | 0 | 6.40 | 12.97 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1095432/0 | 1095432/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-b-8192-n1-r3 | 完成 | 1138999 | 1138999 | 1138999 | 1138999 | 1138992 | 0 | 0 | 0 | 6.45 | 8.48 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/1138999/0 | 1138999/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-dds-iox-1024-n1-r1 | 完成 | 918988 | 918988 | 918988 | 918988 | 918980 | 0 | 0 | 0 | 4.43 | 8.94 | 1.0 | 1.000000 | 1.500973 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9191/0/9191 |
| 原安装速度 | speed-pubsub-dds-iox-1024-n1-r2 | 完成 | 916142 | 916142 | 916142 | 916142 | 916134 | 0 | 0 | 0 | 8.25 | 12.65 | 1.0 | 1.000000 | 1.500862 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9162/0/9162 |
| 原安装速度 | speed-pubsub-dds-iox-1024-n1-r3 | 完成 | 952282 | 952282 | 952282 | 952282 | 952274 | 0 | 0 | 0 | 6.66 | 8.96 | 1.0 | 1.000001 | 1.500982 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9524/0/9524 |
| 原安装速度 | speed-pubsub-dds-iox-1048576-n1-r1 | 完成 | 1149 | 1149 | 1149 | 1149 | 1141 | 0 | 0 | 0 | 6,926.90 | 8,091.29 | 1.0 | 1.000000 | 1.500692 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-pubsub-dds-iox-1048576-n1-r2 | 完成 | 1126 | 1126 | 1126 | 1126 | 1118 | 0 | 0 | 0 | 7,059.07 | 8,593.68 | 1.0 | 1.000000 | 1.500236 | 1.00 | 1.02 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-pubsub-dds-iox-1048576-n1-r3 | 完成 | 1158 | 1158 | 1158 | 1158 | 1150 | 0 | 0 | 0 | 6,886.73 | 8,166.07 | 1.0 | 1.000000 | 1.500603 | 1.00 | 1.02 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-pubsub-dds-iox-128-n1-r1 | 完成 | 944383 | 944383 | 944383 | 944383 | 944381 | 0 | 0 | 0 | 1.28 | 12.61 | 1.0 | 1.000001 | 1.500346 | 1.00 | 0.99 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9445/0/9445 |
| 原安装速度 | speed-pubsub-dds-iox-128-n1-r2 | 完成 | 864862 | 864862 | 864862 | 864862 | 864860 | 0 | 0 | 0 | 1.38 | 2.63 | 1.0 | 1.000001 | 1.500888 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8649/0/8649 |
| 原安装速度 | speed-pubsub-dds-iox-128-n1-r3 | 完成 | 899327 | 899327 | 899327 | 899327 | 899326 | 0 | 0 | 0 | 1.31 | 3.27 | 1.0 | 1.000001 | 1.500702 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8994/0/8994 |
| 原安装速度 | speed-pubsub-dds-iox-131072-n1-r1 | 完成 | 12561 | 12561 | 12561 | 12561 | 12553 | 0 | 0 | 0 | 634.45 | 655.45 | 1.0 | 1.000000 | 1.500528 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 126/0/126 |
| 原安装速度 | speed-pubsub-dds-iox-131072-n1-r2 | 完成 | 12601 | 12601 | 12601 | 12601 | 12593 | 0 | 0 | 0 | 629.44 | 990.73 | 1.0 | 1.000000 | 1.500543 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 127/0/127 |
| 原安装速度 | speed-pubsub-dds-iox-131072-n1-r3 | 完成 | 12521 | 12521 | 12521 | 12521 | 12513 | 0 | 0 | 0 | 635.56 | 706.61 | 1.0 | 1.000000 | 1.500162 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 126/0/126 |
| 原安装速度 | speed-pubsub-dds-iox-16-n1-r1 | 完成 | 840661 | 840661 | 840661 | 840661 | 840660 | 0 | 0 | 0 | 1.41 | 11.51 | 1.0 | 1.000001 | 1.500711 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8407/0/8407 |
| 原安装速度 | speed-pubsub-dds-iox-16-n1-r2 | 完成 | 915553 | 915553 | 915553 | 915553 | 915552 | 0 | 0 | 0 | 1.28 | 2.58 | 1.0 | 1.000000 | 1.500743 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9156/0/9156 |
| 原安装速度 | speed-pubsub-dds-iox-16-n1-r3 | 完成 | 929651 | 929651 | 929651 | 929651 | 929650 | 0 | 0 | 0 | 1.23 | 12.15 | 1.0 | 1.000000 | 1.500397 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9297/0/9297 |
| 原安装速度 | speed-pubsub-dds-iox-16384-n1-r1 | 完成 | 139317 | 139317 | 139317 | 139317 | 139309 | 0 | 0 | 0 | 56.75 | 71.54 | 1.0 | 1.000000 | 1.500344 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1394/0/1394 |
| 原安装速度 | speed-pubsub-dds-iox-16384-n1-r2 | 完成 | 141180 | 141180 | 141180 | 141180 | 141172 | 0 | 0 | 0 | 56.24 | 58.81 | 1.0 | 1.000001 | 1.500463 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1413/0/1413 |
| 原安装速度 | speed-pubsub-dds-iox-16384-n1-r3 | 完成 | 142785 | 142785 | 142785 | 142785 | 142777 | 0 | 0 | 0 | 55.16 | 58.18 | 1.0 | 1.000000 | 1.500575 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1430/0/1429 |
| 原安装速度 | speed-pubsub-dds-iox-2048-n1-r1 | 完成 | 636851 | 636851 | 636851 | 636851 | 636843 | 0 | 0 | 0 | 12.12 | 24.04 | 1.0 | 1.000001 | 1.500587 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 6369/0/6369 |
| 原安装速度 | speed-pubsub-dds-iox-2048-n1-r2 | 完成 | 615300 | 615300 | 615300 | 615300 | 615292 | 0 | 0 | 0 | 12.59 | 14.25 | 1.0 | 1.000001 | 1.500653 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 6154/0/6154 |
| 原安装速度 | speed-pubsub-dds-iox-2048-n1-r3 | 完成 | 602581 | 602581 | 602581 | 602581 | 602573 | 0 | 0 | 0 | 12.93 | 14.34 | 1.0 | 1.000001 | 1.500344 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 6027/0/6027 |
| 原安装速度 | speed-pubsub-dds-iox-256-n1-r1 | 完成 | 936312 | 936312 | 936312 | 936312 | 936310 | 0 | 0 | 0 | 1.38 | 4.51 | 1.0 | 1.000001 | 1.500413 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9364/0/9364 |
| 原安装速度 | speed-pubsub-dds-iox-256-n1-r2 | 完成 | 836712 | 836712 | 836712 | 836712 | 836711 | 0 | 0 | 0 | 1.42 | 13.52 | 1.0 | 1.000000 | 1.500259 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8368/0/8368 |
| 原安装速度 | speed-pubsub-dds-iox-256-n1-r3 | 完成 | 938095 | 938095 | 938095 | 938095 | 938094 | 0 | 0 | 0 | 1.41 | 7.64 | 1.0 | 1.000000 | 1.500461 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9382/0/9382 |
| 原安装速度 | speed-pubsub-dds-iox-262144-n1-r1 | 完成 | 6208 | 6208 | 6208 | 6208 | 6200 | 0 | 0 | 0 | 1,273.90 | 1,386.92 | 1.0 | 1.000000 | 1.500709 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 63/0/63 |
| 原安装速度 | speed-pubsub-dds-iox-262144-n1-r2 | 完成 | 6333 | 6333 | 6333 | 6333 | 6325 | 0 | 0 | 0 | 1,261.74 | 1,297.83 | 1.0 | 1.000000 | 1.500829 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 64/0/64 |
| 原安装速度 | speed-pubsub-dds-iox-262144-n1-r3 | 完成 | 6298 | 6298 | 6298 | 6298 | 6290 | 0 | 0 | 0 | 1,267.52 | 1,309.04 | 1.0 | 1.000000 | 1.500048 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 64/0/64 |
| 原安装速度 | speed-pubsub-dds-iox-32-n1-r1 | 完成 | 814106 | 814106 | 814106 | 814106 | 814104 | 0 | 0 | 0 | 1.49 | 3.36 | 1.0 | 1.000001 | 1.500853 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8142/0/8142 |
| 原安装速度 | speed-pubsub-dds-iox-32-n1-r2 | 完成 | 938996 | 938996 | 938996 | 938996 | 938995 | 0 | 0 | 0 | 1.42 | 4.22 | 1.0 | 1.000001 | 1.500226 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9391/0/9391 |
| 原安装速度 | speed-pubsub-dds-iox-32-n1-r3 | 完成 | 917614 | 917614 | 917614 | 917614 | 917613 | 0 | 0 | 0 | 1.29 | 10.79 | 1.0 | 1.000000 | 1.500374 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9177/0/9177 |
| 原安装速度 | speed-pubsub-dds-iox-32768-n1-r1 | 完成 | 76341 | 76341 | 76341 | 76341 | 76333 | 0 | 0 | 0 | 104.42 | 108.48 | 1.0 | 1.000000 | 1.500019 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 764/0/764 |
| 原安装速度 | speed-pubsub-dds-iox-32768-n1-r2 | 完成 | 74779 | 74779 | 74779 | 74779 | 74771 | 0 | 0 | 0 | 105.66 | 112.24 | 1.0 | 1.000000 | 1.500737 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 749/0/749 |
| 原安装速度 | speed-pubsub-dds-iox-32768-n1-r3 | 完成 | 76324 | 76324 | 76324 | 76324 | 76316 | 0 | 0 | 0 | 102.97 | 111.12 | 1.0 | 1.000000 | 1.501054 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 764/0/764 |
| 原安装速度 | speed-pubsub-dds-iox-4096-n1-r1 | 完成 | 412394 | 412394 | 412394 | 412394 | 412386 | 0 | 0 | 0 | 18.72 | 39.57 | 1.0 | 1.000000 | 1.500090 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 4125/0/4125 |
| 原安装速度 | speed-pubsub-dds-iox-4096-n1-r2 | 完成 | 422566 | 422566 | 422566 | 422566 | 422558 | 0 | 0 | 0 | 18.37 | 23.60 | 1.0 | 1.000000 | 1.500515 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 4226/0/4226 |
| 原安装速度 | speed-pubsub-dds-iox-4096-n1-r3 | 完成 | 419936 | 419936 | 419936 | 419936 | 419928 | 0 | 0 | 0 | 18.73 | 20.21 | 1.0 | 1.000001 | 1.500295 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 4200/0/4200 |
| 原安装速度 | speed-pubsub-dds-iox-512-n1-r1 | 完成 | 833480 | 833480 | 833480 | 833480 | 833479 | 0 | 0 | 0 | 1.40 | 5.90 | 1.0 | 1.000001 | 1.500403 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8336/0/8336 |
| 原安装速度 | speed-pubsub-dds-iox-512-n1-r2 | 完成 | 836880 | 836880 | 836880 | 836880 | 836879 | 0 | 0 | 0 | 1.55 | 7.63 | 1.0 | 1.000001 | 1.500136 | 1.00 | 0.99 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8370/0/8370 |
| 原安装速度 | speed-pubsub-dds-iox-512-n1-r3 | 完成 | 927716 | 927716 | 927716 | 927716 | 927715 | 0 | 0 | 0 | 1.50 | 14.05 | 1.0 | 1.000001 | 1.500080 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9278/0/9278 |
| 原安装速度 | speed-pubsub-dds-iox-524288-n1-r1 | 完成 | 3139 | 3139 | 3139 | 3139 | 3131 | 0 | 0 | 0 | 2,547.28 | 2,627.93 | 1.0 | 1.000000 | 1.500625 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 32/0/32 |
| 原安装速度 | speed-pubsub-dds-iox-524288-n1-r2 | 完成 | 3158 | 3158 | 3158 | 3158 | 3150 | 0 | 0 | 0 | 2,531.06 | 2,616.10 | 1.0 | 1.000000 | 1.500872 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 32/0/32 |
| 原安装速度 | speed-pubsub-dds-iox-524288-n1-r3 | 完成 | 3176 | 3176 | 3176 | 3176 | 3168 | 0 | 0 | 0 | 2,516.74 | 2,590.08 | 1.0 | 1.000000 | 1.500996 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 33/0/33 |
| 原安装速度 | speed-pubsub-dds-iox-64-n1-r1 | 完成 | 929012 | 929012 | 929012 | 929012 | 929011 | 0 | 0 | 0 | 1.30 | 12.07 | 1.0 | 1.000000 | 1.500234 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9291/0/9291 |
| 原安装速度 | speed-pubsub-dds-iox-64-n1-r2 | 完成 | 847807 | 847807 | 847807 | 847807 | 847805 | 0 | 0 | 0 | 1.41 | 11.95 | 1.0 | 1.000001 | 1.500795 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8479/0/8479 |
| 原安装速度 | speed-pubsub-dds-iox-64-n1-r3 | 完成 | 861475 | 861475 | 861475 | 861475 | 861474 | 0 | 0 | 0 | 1.40 | 11.69 | 1.0 | 1.000000 | 1.500102 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8616/0/8616 |
| 原安装速度 | speed-pubsub-dds-iox-65536-n1-r1 | 完成 | 33194 | 33194 | 33194 | 33194 | 33186 | 0 | 0 | 0 | 237.88 | 249.85 | 1.0 | 1.000000 | 1.500360 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 333/0/333 |
| 原安装速度 | speed-pubsub-dds-iox-65536-n1-r2 | 完成 | 34334 | 34334 | 34334 | 34334 | 34326 | 0 | 0 | 0 | 230.09 | 244.69 | 1.0 | 1.000001 | 1.500633 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 344/0/344 |
| 原安装速度 | speed-pubsub-dds-iox-65536-n1-r3 | 完成 | 32830 | 32830 | 32830 | 32830 | 32822 | 0 | 0 | 0 | 240.91 | 252.28 | 1.0 | 1.000000 | 1.501032 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 329/0/329 |
| 原安装速度 | speed-pubsub-dds-iox-8-n1-r1 | 完成 | 895627 | 895627 | 895627 | 895627 | 895626 | 0 | 0 | 0 | 1.27 | 4.85 | 1.0 | 1.000000 | 1.500088 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8957/0/8957 |
| 原安装速度 | speed-pubsub-dds-iox-8-n1-r2 | 完成 | 892169 | 892169 | 892169 | 892169 | 892168 | 0 | 0 | 0 | 1.32 | 2.58 | 1.0 | 1.000001 | 1.500018 | 1.00 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 8923/0/8923 |
| 原安装速度 | speed-pubsub-dds-iox-8-n1-r3 | 完成 | 910124 | 910124 | 910124 | 910124 | 910122 | 0 | 0 | 0 | 1.26 | 4.93 | 1.0 | 1.000001 | 1.501009 | 1.00 | 0.99 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 9102/0/9102 |
| 原安装速度 | speed-pubsub-dds-iox-8192-n1-r1 | 完成 | 254494 | 254494 | 254494 | 254494 | 254486 | 0 | 0 | 0 | 30.97 | 32.95 | 1.0 | 1.000001 | 1.500114 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 2546/0/2546 |
| 原安装速度 | speed-pubsub-dds-iox-8192-n1-r2 | 完成 | 252980 | 252980 | 252980 | 252980 | 252972 | 0 | 0 | 0 | 31.34 | 33.05 | 1.0 | 1.000000 | 1.500213 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 2531/0/2531 |
| 原安装速度 | speed-pubsub-dds-iox-8192-n1-r3 | 完成 | 252143 | 252143 | 252143 | 252143 | 252135 | 0 | 0 | 0 | 31.20 | 33.10 | 1.0 | 1.000000 | 1.500389 | 1.00 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 2522/0/2522 |
| 原安装速度 | speed-pubsub-dds-udp-1024-n1-r1 | 完成 | 139218 | 139218 | 139218 | 139218 | 139217 | 0 | 0 | 0 | 10.93 | 73.90 | 1.0 | 1.000002 | 1.500438 | 1.00 | 0.77 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1393 |
| 原安装速度 | speed-pubsub-dds-udp-1024-n1-r2 | 完成 | 133765 | 133765 | 133765 | 133765 | 133763 | 0 | 0 | 0 | 11.39 | 74.05 | 1.0 | 1.000006 | 1.500613 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1338 |
| 原安装速度 | speed-pubsub-dds-udp-1024-n1-r3 | 完成 | 138843 | 138843 | 138843 | 138843 | 138842 | 0 | 0 | 0 | 11.11 | 18.18 | 1.0 | 1.000003 | 1.500082 | 1.00 | 0.75 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1389 |
| 原安装速度 | speed-pubsub-dds-udp-1048576-n1-r1 | 完成 | 648 | 648 | 648 | 648 | 640 | 0 | 0 | 0 | 12,369.84 | 14,070.38 | 1.0 | 1.000375 | 1.500387 | 1.00 | 1.02 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/7 |
| 原安装速度 | speed-pubsub-dds-udp-1048576-n1-r2 | 完成 | 650 | 650 | 650 | 650 | 642 | 0 | 0 | 0 | 12,356.53 | 13,880.09 | 1.0 | 1.000000 | 1.500204 | 1.00 | 1.02 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/7 |
| 原安装速度 | speed-pubsub-dds-udp-1048576-n1-r3 | 完成 | 637 | 637 | 637 | 637 | 629 | 0 | 0 | 0 | 12,547.71 | 15,100.89 | 1.0 | 1.000248 | 1.500066 | 1.00 | 1.02 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/7 |
| 原安装速度 | speed-pubsub-dds-udp-128-n1-r1 | 完成 | 141079 | 141079 | 141079 | 141079 | 141078 | 0 | 0 | 0 | 10.80 | 66.84 | 1.0 | 1.000000 | 1.500727 | 1.00 | 0.75 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1412 |
| 原安装速度 | speed-pubsub-dds-udp-128-n1-r2 | 完成 | 139882 | 139882 | 139882 | 139882 | 139881 | 0 | 0 | 0 | 10.99 | 64.46 | 1.0 | 1.000000 | 1.500736 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1400 |
| 原安装速度 | speed-pubsub-dds-udp-128-n1-r3 | 完成 | 134891 | 134891 | 134891 | 134891 | 134889 | 0 | 0 | 0 | 11.16 | 68.05 | 1.0 | 1.000003 | 1.500939 | 1.00 | 0.75 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1350 |
| 原安装速度 | speed-pubsub-dds-udp-131072-n1-r1 | 完成 | 8554 | 8554 | 8554 | 8554 | 8546 | 0 | 0 | 0 | 932.18 | 983.90 | 1.0 | 1.000000 | 1.500287 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/86 |
| 原安装速度 | speed-pubsub-dds-udp-131072-n1-r2 | 完成 | 8709 | 8709 | 8709 | 8709 | 8701 | 0 | 0 | 0 | 913.64 | 977.64 | 1.0 | 1.000034 | 1.500314 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/88 |
| 原安装速度 | speed-pubsub-dds-udp-131072-n1-r3 | 完成 | 8609 | 8609 | 8609 | 8609 | 8601 | 0 | 0 | 0 | 921.10 | 1,049.57 | 1.0 | 1.000058 | 1.500225 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/87 |
| 原安装速度 | speed-pubsub-dds-udp-16-n1-r1 | 完成 | 144723 | 144723 | 144723 | 144723 | 144722 | 0 | 0 | 0 | 10.58 | 16.41 | 1.0 | 1.000002 | 1.501003 | 1.00 | 0.72 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1448 |
| 原安装速度 | speed-pubsub-dds-udp-16-n1-r2 | 完成 | 144929 | 144929 | 144929 | 144929 | 144928 | 0 | 0 | 0 | 10.69 | 17.43 | 1.0 | 1.000002 | 1.500600 | 1.00 | 0.72 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1450 |
| 原安装速度 | speed-pubsub-dds-udp-16-n1-r3 | 完成 | 143804 | 143804 | 143804 | 143804 | 143802 | 0 | 0 | 0 | 10.54 | 70.15 | 1.0 | 1.000005 | 1.500574 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1439 |
| 原安装速度 | speed-pubsub-dds-udp-16384-n1-r1 | 完成 | 69150 | 69150 | 69150 | 69150 | 69148 | 0 | 0 | 0 | 31.71 | 249.93 | 1.0 | 1.000000 | 1.500732 | 1.00 | 0.99 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/692 |
| 原安装速度 | speed-pubsub-dds-udp-16384-n1-r2 | 完成 | 75506 | 75506 | 75506 | 75506 | 75504 | 0 | 0 | 0 | 34.78 | 110.66 | 1.0 | 1.000010 | 1.500204 | 1.00 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/756 |
| 原安装速度 | speed-pubsub-dds-udp-16384-n1-r3 | 完成 | 69755 | 69755 | 69755 | 69755 | 69751 | 0 | 0 | 0 | 18.39 | 252.75 | 1.0 | 1.000013 | 1.500945 | 1.00 | 0.99 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/698 |
| 原安装速度 | speed-pubsub-dds-udp-2048-n1-r1 | 完成 | 138718 | 138718 | 138718 | 138718 | 138717 | 0 | 0 | 0 | 11.13 | 16.16 | 1.0 | 1.000001 | 1.500031 | 1.00 | 0.78 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1388 |
| 原安装速度 | speed-pubsub-dds-udp-2048-n1-r2 | 完成 | 132485 | 132485 | 132485 | 132485 | 132484 | 0 | 0 | 0 | 11.37 | 78.31 | 1.0 | 1.000001 | 1.500219 | 1.00 | 0.82 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1326 |
| 原安装速度 | speed-pubsub-dds-udp-2048-n1-r3 | 完成 | 138749 | 138749 | 138749 | 138749 | 138748 | 0 | 0 | 0 | 11.22 | 71.00 | 1.0 | 1.000001 | 1.500836 | 1.00 | 0.81 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1388 |
| 原安装速度 | speed-pubsub-dds-udp-256-n1-r1 | 完成 | 143345 | 143345 | 143345 | 143345 | 143343 | 0 | 0 | 0 | 10.76 | 65.55 | 1.0 | 1.000004 | 1.500824 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1434 |
| 原安装速度 | speed-pubsub-dds-udp-256-n1-r2 | 完成 | 143853 | 143853 | 143853 | 143853 | 143851 | 0 | 0 | 0 | 10.70 | 64.43 | 1.0 | 1.000005 | 1.500332 | 1.00 | 0.74 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1439 |
| 原安装速度 | speed-pubsub-dds-udp-256-n1-r3 | 完成 | 139804 | 139804 | 139804 | 139804 | 139802 | 0 | 0 | 0 | 10.92 | 67.38 | 1.0 | 1.000006 | 1.501029 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1399 |
| 原安装速度 | speed-pubsub-dds-udp-262144-n1-r1 | 完成 | 4279 | 4279 | 4279 | 4279 | 4271 | 0 | 0 | 0 | 1,837.31 | 3,372.43 | 1.0 | 1.000000 | 1.500079 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/44 |
| 原安装速度 | speed-pubsub-dds-udp-262144-n1-r2 | 完成 | 4345 | 4345 | 4345 | 4345 | 4337 | 0 | 0 | 0 | 1,837.61 | 1,964.04 | 1.0 | 1.000000 | 1.500784 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/44 |
| 原安装速度 | speed-pubsub-dds-udp-262144-n1-r3 | 完成 | 4333 | 4333 | 4333 | 4333 | 4325 | 0 | 0 | 0 | 1,834.40 | 1,952.09 | 1.0 | 1.000062 | 1.500370 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/44 |
| 原安装速度 | speed-pubsub-dds-udp-32-n1-r1 | 完成 | 138771 | 138771 | 138771 | 138771 | 138769 | 0 | 0 | 0 | 10.85 | 64.49 | 1.0 | 1.000004 | 1.500958 | 1.00 | 0.70 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1389 |
| 原安装速度 | speed-pubsub-dds-udp-32-n1-r2 | 完成 | 139805 | 139805 | 139805 | 139805 | 139804 | 0 | 0 | 0 | 10.93 | 66.59 | 1.0 | 1.000002 | 1.500677 | 1.00 | 0.74 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1399 |
| 原安装速度 | speed-pubsub-dds-udp-32-n1-r3 | 完成 | 139424 | 139424 | 139424 | 139424 | 139423 | 0 | 0 | 0 | 10.92 | 64.67 | 1.0 | 1.000002 | 1.500202 | 1.00 | 0.75 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1395 |
| 原安装速度 | speed-pubsub-dds-udp-32768-n1-r1 | 完成 | 42083 | 42083 | 42083 | 42083 | 42075 | 0 | 0 | 0 | 189.27 | 224.48 | 1.0 | 1.000011 | 1.500911 | 1.00 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/422 |
| 原安装速度 | speed-pubsub-dds-udp-32768-n1-r2 | 完成 | 41766 | 41766 | 41766 | 41766 | 41758 | 0 | 0 | 0 | 186.12 | 432.44 | 1.0 | 1.000015 | 1.500711 | 1.00 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/418 |
| 原安装速度 | speed-pubsub-dds-udp-32768-n1-r3 | 完成 | 43440 | 43440 | 43440 | 43440 | 43432 | 0 | 0 | 0 | 181.80 | 206.70 | 1.0 | 1.000004 | 1.500692 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/435 |
| 原安装速度 | speed-pubsub-dds-udp-4096-n1-r1 | 完成 | 129963 | 129963 | 129963 | 129963 | 129961 | 0 | 0 | 0 | 11.62 | 86.46 | 1.0 | 1.000005 | 1.500175 | 1.00 | 0.89 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1300 |
| 原安装速度 | speed-pubsub-dds-udp-4096-n1-r2 | 完成 | 135532 | 135532 | 135532 | 135532 | 135531 | 0 | 0 | 0 | 11.48 | 86.64 | 1.0 | 1.000001 | 1.500454 | 1.00 | 0.87 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1356 |
| 原安装速度 | speed-pubsub-dds-udp-4096-n1-r3 | 完成 | 127527 | 127527 | 127527 | 127527 | 127526 | 0 | 0 | 0 | 11.84 | 88.77 | 1.0 | 1.000003 | 1.500666 | 1.00 | 0.88 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1276 |
| 原安装速度 | speed-pubsub-dds-udp-512-n1-r1 | 完成 | 139722 | 139722 | 139722 | 139722 | 139720 | 0 | 0 | 0 | 10.91 | 67.34 | 1.0 | 1.000007 | 1.500929 | 1.00 | 0.73 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1398 |
| 原安装速度 | speed-pubsub-dds-udp-512-n1-r2 | 完成 | 143008 | 143008 | 143008 | 143008 | 143006 | 0 | 0 | 0 | 10.78 | 69.47 | 1.0 | 1.000004 | 1.500549 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1431 |
| 原安装速度 | speed-pubsub-dds-udp-512-n1-r3 | 完成 | 138666 | 138666 | 138666 | 138666 | 138665 | 0 | 0 | 0 | 11.03 | 72.84 | 1.0 | 1.000001 | 1.500874 | 1.00 | 0.76 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1387 |
| 原安装速度 | speed-pubsub-dds-udp-524288-n1-r1 | 完成 | 2144 | 2144 | 2144 | 2144 | 2136 | 0 | 0 | 0 | 3,710.27 | 4,178.63 | 1.0 | 1.000219 | 1.500010 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-pubsub-dds-udp-524288-n1-r2 | 完成 | 2111 | 2111 | 2111 | 2111 | 2103 | 0 | 0 | 0 | 3,751.61 | 4,293.25 | 1.0 | 1.000000 | 1.500320 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-pubsub-dds-udp-524288-n1-r3 | 完成 | 2124 | 2124 | 2124 | 2124 | 2116 | 0 | 0 | 0 | 3,742.35 | 4,234.74 | 1.0 | 1.000000 | 1.500640 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-pubsub-dds-udp-64-n1-r1 | 完成 | 145007 | 145007 | 145007 | 145007 | 145005 | 0 | 0 | 0 | 10.62 | 64.00 | 1.0 | 1.000006 | 1.500625 | 1.00 | 0.74 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1451 |
| 原安装速度 | speed-pubsub-dds-udp-64-n1-r2 | 完成 | 138790 | 138790 | 138790 | 138790 | 138789 | 0 | 0 | 0 | 10.95 | 66.61 | 1.0 | 1.000001 | 1.500529 | 1.00 | 0.73 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1389 |
| 原安装速度 | speed-pubsub-dds-udp-64-n1-r3 | 完成 | 140067 | 140067 | 140067 | 140067 | 140065 | 0 | 0 | 0 | 10.84 | 64.82 | 1.0 | 1.000004 | 1.500323 | 1.00 | 0.71 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1401 |
| 原安装速度 | speed-pubsub-dds-udp-65536-n1-r1 | 完成 | 20156 | 20156 | 20156 | 20156 | 20148 | 0 | 0 | 0 | 392.55 | 437.56 | 1.0 | 1.000000 | 1.500105 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/202 |
| 原安装速度 | speed-pubsub-dds-udp-65536-n1-r2 | 完成 | 20349 | 20349 | 20349 | 20349 | 20341 | 0 | 0 | 0 | 389.76 | 433.64 | 1.0 | 1.000008 | 1.500408 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/204 |
| 原安装速度 | speed-pubsub-dds-udp-65536-n1-r3 | 完成 | 20061 | 20061 | 20061 | 20061 | 20053 | 0 | 0 | 0 | 395.83 | 433.38 | 1.0 | 1.000030 | 1.500309 | 1.00 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/201 |
| 原安装速度 | speed-pubsub-dds-udp-8-n1-r1 | 完成 | 136188 | 136188 | 136188 | 136188 | 136187 | 0 | 0 | 0 | 11.08 | 63.91 | 1.0 | 1.000003 | 1.500556 | 1.00 | 0.71 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1363 |
| 原安装速度 | speed-pubsub-dds-udp-8-n1-r2 | 完成 | 144019 | 144019 | 144019 | 144019 | 144017 | 0 | 0 | 0 | 10.64 | 15.69 | 1.0 | 1.000005 | 1.500777 | 1.00 | 0.71 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1441 |
| 原安装速度 | speed-pubsub-dds-udp-8-n1-r3 | 完成 | 145044 | 145044 | 145044 | 145044 | 145043 | 0 | 0 | 0 | 10.77 | 66.27 | 1.0 | 1.000001 | 1.500361 | 1.00 | 0.79 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1451 |
| 原安装速度 | speed-pubsub-dds-udp-8192-n1-r1 | 完成 | 137729 | 137729 | 137729 | 137729 | 137727 | 0 | 0 | 0 | 15.10 | 49.08 | 1.0 | 1.000003 | 1.500294 | 1.00 | 0.99 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1378 |
| 原安装速度 | speed-pubsub-dds-udp-8192-n1-r2 | 完成 | 125677 | 125677 | 125677 | 125677 | 125675 | 0 | 0 | 0 | 12.11 | 117.69 | 1.0 | 1.000008 | 1.500010 | 1.00 | 0.98 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1258 |
| 原安装速度 | speed-pubsub-dds-udp-8192-n1-r3 | 完成 | 131805 | 131805 | 131805 | 131805 | 131799 | 0 | 0 | 0 | 15.15 | 126.88 | 1.0 | 1.000002 | 1.500724 | 1.00 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/1319 |
| 原安装速度 | speed-pubsub-prebuilt-1024-n1-r1 | 完成 | 1118300 | 1118300 | 1118300 | 1118300 | 1118292 | 0 | 0 | 0 | 6.71 | 7.95 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1118300 | 1118300/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-1024-n1-r2 | 完成 | 1214166 | 1214166 | 1214166 | 1214166 | 1214158 | 0 | 0 | 0 | 5.80 | 7.49 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1214166 | 1214166/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-1024-n1-r3 | 完成 | 1170023 | 1170023 | 1170023 | 1170023 | 1170020 | 0 | 0 | 0 | 6.12 | 7.76 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1170023 | 1170023/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-1048576-n1-r1 | 完成 | 12401 | 12401 | 12401 | 12401 | 12400 | 0 | 0 | 0 | 82.47 | 93.25 | 1.0 | 1.000008 | 1.500000 | 1.00 | 1.55 | 2/3 | 0/0/0/12401 | 12401/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-1048576-n1-r2 | 完成 | 12319 | 12319 | 12319 | 12319 | 12318 | 0 | 0 | 0 | 84.66 | 113.36 | 1.0 | 1.000075 | 1.500000 | 1.00 | 1.57 | 2/3 | 0/0/0/12319 | 12319/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-1048576-n1-r3 | 完成 | 12375 | 12375 | 12375 | 12375 | 12374 | 0 | 0 | 0 | 83.29 | 93.01 | 1.0 | 1.000070 | 1.500000 | 1.00 | 1.56 | 2/3 | 0/0/0/12375 | 12375/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-128-n1-r1 | 完成 | 1145573 | 1145573 | 1145573 | 1145573 | 1145566 | 0 | 0 | 0 | 6.60 | 8.50 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1145573 | 1145573/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-128-n1-r2 | 完成 | 1212153 | 1212153 | 1212153 | 1212153 | 1212145 | 0 | 0 | 0 | 6.18 | 7.51 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 0/0/0/1212153 | 1212153/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-128-n1-r3 | 完成 | 1113810 | 1113810 | 1113810 | 1113810 | 1113802 | 0 | 0 | 0 | 6.74 | 8.55 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1113810 | 1113810/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-131072-n1-r1 | 完成 | 131434 | 131434 | 131434 | 131434 | 131432 | 0 | 0 | 0 | 11.13 | 13.19 | 1.0 | 1.000005 | 1.500000 | 1.00 | 2.07 | 2/3 | 0/0/0/131434 | 131434/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-131072-n1-r2 | 完成 | 131292 | 131292 | 131292 | 131292 | 131290 | 0 | 0 | 0 | 11.07 | 13.63 | 1.0 | 1.000006 | 1.500000 | 1.00 | 2.01 | 2/3 | 0/0/0/131292 | 131292/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-131072-n1-r3 | 完成 | 132378 | 132378 | 132378 | 132378 | 132377 | 0 | 0 | 0 | 9.44 | 12.13 | 1.0 | 1.000001 | 1.500000 | 1.00 | 1.90 | 2/3 | 0/0/0/132378 | 132378/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16-n1-r1 | 完成 | 1120276 | 1120276 | 1120276 | 1120276 | 1120268 | 0 | 0 | 0 | 6.72 | 8.76 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1120276 | 1120276/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16-n1-r2 | 完成 | 1169167 | 1169167 | 1169167 | 1169167 | 1169159 | 0 | 0 | 0 | 6.43 | 9.27 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1169167 | 1169167/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16-n1-r3 | 完成 | 1110810 | 1110810 | 1110810 | 1110810 | 1110802 | 0 | 0 | 0 | 6.84 | 8.13 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1110810 | 1110810/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16384-n1-r1 | 完成 | 735067 | 735067 | 735067 | 735067 | 735066 | 0 | 0 | 0 | 2.19 | 3.94 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/735067 | 735067/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16384-n1-r2 | 完成 | 737553 | 737553 | 737553 | 737553 | 737551 | 0 | 0 | 0 | 2.20 | 4.28 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/737553 | 737553/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-16384-n1-r3 | 完成 | 677750 | 677750 | 677750 | 677750 | 677749 | 0 | 0 | 0 | 2.30 | 13.05 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.48 | 2/3 | 0/0/0/677750 | 677750/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-2048-n1-r1 | 完成 | 1172442 | 1172442 | 1172442 | 1172442 | 1172434 | 0 | 0 | 0 | 5.58 | 7.25 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1172442 | 1172442/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-2048-n1-r2 | 完成 | 1175027 | 1175027 | 1175027 | 1175027 | 1175020 | 0 | 0 | 0 | 4.63 | 7.57 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1175027 | 1175027/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-2048-n1-r3 | 完成 | 1168952 | 1168952 | 1168952 | 1168952 | 1168945 | 0 | 0 | 0 | 2.09 | 9.54 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1168952 | 1168952/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-256-n1-r1 | 完成 | 1147917 | 1147917 | 1147917 | 1147917 | 1147911 | 0 | 0 | 0 | 6.55 | 7.67 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1147917 | 1147917/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-256-n1-r2 | 完成 | 1143553 | 1143553 | 1143553 | 1143553 | 1143546 | 0 | 0 | 0 | 6.48 | 11.63 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1143553 | 1143553/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-256-n1-r3 | 完成 | 1128680 | 1128680 | 1128680 | 1128680 | 1128672 | 0 | 0 | 0 | 6.55 | 8.85 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1128680 | 1128680/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-262144-n1-r1 | 完成 | 71001 | 71001 | 71001 | 71001 | 71000 | 0 | 0 | 0 | 17.78 | 20.34 | 1.0 | 1.000010 | 1.500000 | 1.00 | 1.80 | 2/3 | 0/0/0/71001 | 71001/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-262144-n1-r2 | 完成 | 70985 | 70985 | 70985 | 70985 | 70983 | 0 | 0 | 0 | 15.97 | 18.84 | 1.0 | 1.000012 | 1.500000 | 1.00 | 1.70 | 2/3 | 0/0/0/70985 | 70985/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-262144-n1-r3 | 完成 | 70427 | 70427 | 70427 | 70427 | 70426 | 0 | 0 | 0 | 18.00 | 24.60 | 1.0 | 1.000008 | 1.500000 | 1.00 | 1.81 | 2/3 | 0/0/0/70427 | 70427/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32-n1-r1 | 完成 | 1142089 | 1142089 | 1142089 | 1142089 | 1142081 | 0 | 0 | 0 | 6.69 | 7.79 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1142089 | 1142089/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32-n1-r2 | 完成 | 1155309 | 1155309 | 1155309 | 1155309 | 1155301 | 0 | 0 | 0 | 6.52 | 8.12 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1155309 | 1155309/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32-n1-r3 | 完成 | 1158113 | 1158113 | 1158113 | 1158113 | 1158105 | 0 | 0 | 0 | 6.54 | 8.12 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1158113 | 1158113/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32768-n1-r1 | 有异常 | 328976 | 328976 | 328976 | 328968 | 328968 | 0 | 0 | 0 | 3.84 | 5.06 | 1.0 | 1.000000 | 2.000088 | 1.00 | 2.62 | 2/3 | 0/0/0/328976 | 328976/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32768-n1-r2 | 完成 | 420589 | 420589 | 420589 | 420589 | 420588 | 0 | 0 | 0 | 3.90 | 5.67 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.30 | 2/3 | 0/0/0/420589 | 420589/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-32768-n1-r3 | 完成 | 410459 | 410459 | 410459 | 410459 | 410457 | 0 | 0 | 0 | 4.12 | 5.59 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.26 | 2/3 | 0/0/0/410459 | 410459/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-4096-n1-r1 | 完成 | 1135791 | 1135791 | 1135791 | 1135791 | 1135789 | 0 | 0 | 0 | 4.76 | 10.67 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1135791 | 1135791/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-4096-n1-r2 | 完成 | 1066687 | 1066687 | 1066687 | 1066687 | 1066685 | 0 | 0 | 0 | 6.58 | 12.27 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1066687 | 1066687/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-4096-n1-r3 | 完成 | 1104035 | 1104035 | 1104035 | 1104035 | 1104034 | 0 | 0 | 0 | 2.30 | 12.44 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1104035 | 1104035/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-512-n1-r1 | 完成 | 1094194 | 1094194 | 1094194 | 1094194 | 1094186 | 0 | 0 | 0 | 6.86 | 12.22 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1094194 | 1094194/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-512-n1-r2 | 完成 | 1171486 | 1171486 | 1171486 | 1171486 | 1171479 | 0 | 0 | 0 | 6.34 | 7.83 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1171486 | 1171486/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-512-n1-r3 | 完成 | 1165156 | 1165156 | 1165156 | 1165156 | 1165149 | 0 | 0 | 0 | 6.25 | 8.15 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1165156 | 1165156/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-524288-n1-r1 | 完成 | 35558 | 35558 | 35558 | 35558 | 35557 | 0 | 0 | 0 | 31.75 | 38.53 | 1.0 | 1.000017 | 1.500000 | 1.00 | 1.66 | 2/3 | 0/0/0/35558 | 35558/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-524288-n1-r2 | 完成 | 35922 | 35922 | 35922 | 35922 | 35921 | 0 | 0 | 0 | 31.01 | 49.12 | 1.0 | 1.000006 | 1.500000 | 1.00 | 1.66 | 2/3 | 0/0/0/35922 | 35922/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-524288-n1-r3 | 完成 | 35434 | 35434 | 35434 | 35434 | 35433 | 0 | 0 | 0 | 31.48 | 56.92 | 1.0 | 1.000003 | 1.500000 | 1.00 | 1.66 | 2/3 | 0/0/0/35434 | 35434/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-64-n1-r1 | 完成 | 1169438 | 1169438 | 1169438 | 1169438 | 1169430 | 0 | 0 | 0 | 6.40 | 8.34 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1169438 | 1169438/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-64-n1-r2 | 完成 | 1187765 | 1187765 | 1187765 | 1187765 | 1187758 | 0 | 0 | 0 | 6.31 | 7.43 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1187765 | 1187765/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-64-n1-r3 | 完成 | 1142826 | 1142826 | 1142826 | 1142826 | 1142818 | 0 | 0 | 0 | 6.58 | 7.72 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1142826 | 1142826/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-65536-n1-r1 | 完成 | 226588 | 226588 | 226588 | 226588 | 226587 | 0 | 0 | 0 | 6.12 | 9.53 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.10 | 2/3 | 0/0/0/226588 | 226588/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-65536-n1-r2 | 完成 | 227459 | 227459 | 227459 | 227459 | 227457 | 0 | 0 | 0 | 6.11 | 8.65 | 1.0 | 1.000004 | 1.500000 | 1.00 | 2.09 | 2/3 | 0/0/0/227459 | 227459/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-65536-n1-r3 | 完成 | 227965 | 227965 | 227965 | 227965 | 227963 | 0 | 0 | 0 | 6.17 | 9.33 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.14 | 2/3 | 0/0/0/227965 | 227965/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8-n1-r1 | 完成 | 1155884 | 1155884 | 1155884 | 1155884 | 1155876 | 0 | 0 | 0 | 6.58 | 7.86 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1155884 | 1155884/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8-n1-r2 | 完成 | 1093140 | 1093140 | 1093140 | 1093140 | 1093132 | 0 | 0 | 0 | 6.70 | 12.38 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1093140 | 1093140/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8-n1-r3 | 完成 | 1167826 | 1167826 | 1167826 | 1167826 | 1167818 | 0 | 0 | 0 | 6.46 | 7.79 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1167826 | 1167826/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8192-n1-r1 | 有异常 | 662782 | 662782 | 662782 | 662774 | 662774 | 0 | 0 | 0 | 1.73 | 12.69 | 1.0 | 1.000000 | 2.000201 | 1.00 | 2.65 | 2/3 | 0/0/0/662782 | 662782/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8192-n1-r2 | 完成 | 1032982 | 1032982 | 1032982 | 1032982 | 1032979 | 0 | 0 | 0 | 1.76 | 5.04 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/1032982 | 1032982/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-prebuilt-8192-n1-r3 | 完成 | 980940 | 980940 | 980940 | 980940 | 980938 | 0 | 0 | 0 | 1.75 | 5.09 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 0/0/0/980940 | 980940/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1024-n1-r1 | 完成 | 684754 | 684754 | 684754 | 684754 | 684747 | 0 | 0 | 0 | 11.09 | 21.14 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 684754/0/0/0 | 0/684754 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1024-n1-r2 | 完成 | 700822 | 700822 | 700822 | 700822 | 700814 | 0 | 0 | 0 | 11.15 | 13.78 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 700822/0/0/0 | 0/700822 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1024-n1-r3 | 完成 | 674232 | 674232 | 674232 | 674232 | 674224 | 0 | 0 | 0 | 11.47 | 13.97 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 674232/0/0/0 | 0/674232 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1048576-n1-r1 | 完成 | 11293 | 11293 | 11293 | 11293 | 11291 | 0 | 0 | 0 | 155.32 | 175.37 | 1.0 | 1.000084 | 1.500000 | 1.00 | 2.29 | 2/3 | 11293/0/0/0 | 0/11293 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1048576-n1-r2 | 完成 | 11046 | 11046 | 11046 | 11046 | 11045 | 0 | 0 | 0 | 156.03 | 229.58 | 1.0 | 1.000008 | 1.500000 | 1.00 | 2.29 | 2/3 | 11046/0/0/0 | 0/11046 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-1048576-n1-r3 | 完成 | 11411 | 11411 | 11411 | 11411 | 11409 | 0 | 0 | 0 | 152.17 | 196.98 | 1.0 | 1.000044 | 1.500000 | 1.00 | 2.28 | 2/3 | 11411/0/0/0 | 0/11411 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-128-n1-r1 | 完成 | 725676 | 725676 | 725676 | 725676 | 725668 | 0 | 0 | 0 | 10.61 | 12.56 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 725676/0/0/0 | 0/725676 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-128-n1-r2 | 完成 | 727561 | 727561 | 727561 | 727561 | 727553 | 0 | 0 | 0 | 10.75 | 12.40 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 727561/0/0/0 | 0/727561 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-128-n1-r3 | 完成 | 690000 | 690000 | 690000 | 690000 | 689992 | 0 | 0 | 0 | 11.21 | 13.49 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 690000/0/0/0 | 0/690000 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-131072-n1-r1 | 完成 | 97742 | 97742 | 97742 | 97742 | 97734 | 0 | 0 | 0 | 23.77 | 89.58 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.47 | 2/3 | 97742/0/0/0 | 0/97742 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-131072-n1-r2 | 完成 | 99938 | 99938 | 99938 | 99938 | 99935 | 0 | 0 | 0 | 27.36 | 37.40 | 1.0 | 1.000008 | 1.500000 | 1.00 | 2.49 | 2/3 | 99938/0/0/0 | 0/99938 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-131072-n1-r3 | 完成 | 92864 | 92864 | 92864 | 92864 | 92862 | 0 | 0 | 0 | 86.34 | 89.78 | 1.0 | 1.000008 | 1.500000 | 1.00 | 2.50 | 2/3 | 92864/0/0/0 | 0/92864 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16-n1-r1 | 完成 | 714398 | 714398 | 714398 | 714398 | 714390 | 0 | 0 | 0 | 10.88 | 13.29 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 714398/0/0/0 | 0/714398 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16-n1-r2 | 完成 | 768810 | 768810 | 768810 | 768810 | 768802 | 0 | 0 | 0 | 10.10 | 13.13 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 768810/0/0/0 | 0/768810 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16-n1-r3 | 完成 | 744577 | 744577 | 744577 | 744577 | 744574 | 0 | 0 | 0 | 10.41 | 12.29 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.49 | 2/3 | 744577/0/0/0 | 0/744577 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16384-n1-r1 | 完成 | 327902 | 327902 | 327902 | 327902 | 327894 | 0 | 0 | 0 | 22.79 | 27.74 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 327902/0/0/0 | 0/327902 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16384-n1-r2 | 完成 | 346484 | 346484 | 346484 | 346484 | 346476 | 0 | 0 | 0 | 22.46 | 32.37 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/3 | 346484/0/0/0 | 0/346484 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-16384-n1-r3 | 完成 | 367235 | 367235 | 367235 | 367235 | 367227 | 0 | 0 | 0 | 21.21 | 33.20 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 367235/0/0/0 | 0/367235 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-2048-n1-r1 | 完成 | 623785 | 623785 | 623785 | 623785 | 623777 | 0 | 0 | 0 | 12.40 | 15.31 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 623785/0/0/0 | 0/623785 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-2048-n1-r2 | 完成 | 628627 | 628627 | 628627 | 628627 | 628619 | 0 | 0 | 0 | 12.35 | 15.21 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 628627/0/0/0 | 0/628627 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-2048-n1-r3 | 完成 | 629456 | 629456 | 629456 | 629456 | 629448 | 0 | 0 | 0 | 12.28 | 13.96 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 629456/0/0/0 | 0/629456 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-256-n1-r1 | 完成 | 752883 | 752883 | 752883 | 752883 | 752875 | 0 | 0 | 0 | 10.24 | 11.67 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 752883/0/0/0 | 0/752883 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-256-n1-r2 | 完成 | 717930 | 717930 | 717930 | 717930 | 717922 | 0 | 0 | 0 | 10.78 | 14.91 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 717930/0/0/0 | 0/717930 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-256-n1-r3 | 完成 | 715562 | 715562 | 715562 | 715562 | 715554 | 0 | 0 | 0 | 10.81 | 13.09 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 715562/0/0/0 | 0/715562 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-262144-n1-r1 | 完成 | 51134 | 51134 | 51134 | 51134 | 51128 | 0 | 0 | 0 | 42.95 | 123.68 | 1.0 | 1.000013 | 1.500000 | 1.00 | 2.47 | 2/3 | 51134/0/0/0 | 0/51134 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-262144-n1-r2 | 完成 | 52206 | 52206 | 52206 | 52206 | 52200 | 0 | 0 | 0 | 40.82 | 120.43 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.48 | 2/3 | 52206/0/0/0 | 0/52206 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-262144-n1-r3 | 完成 | 51650 | 51650 | 51650 | 51650 | 51648 | 0 | 0 | 0 | 48.52 | 125.31 | 1.0 | 1.000005 | 1.500000 | 1.00 | 2.48 | 2/3 | 51650/0/0/0 | 0/51650 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32-n1-r1 | 完成 | 724335 | 724335 | 724335 | 724335 | 724327 | 0 | 0 | 0 | 10.73 | 12.40 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 724335/0/0/0 | 0/724335 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32-n1-r2 | 完成 | 720875 | 720875 | 720875 | 720875 | 720867 | 0 | 0 | 0 | 10.80 | 12.34 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 720875/0/0/0 | 0/720875 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32-n1-r3 | 完成 | 679575 | 679575 | 679575 | 679575 | 679567 | 0 | 0 | 0 | 11.45 | 13.10 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 679575/0/0/0 | 0/679575 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32768-n1-r1 | 完成 | 299288 | 299288 | 299288 | 299288 | 299280 | 0 | 0 | 0 | 25.97 | 30.46 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 299288/0/0/0 | 0/299288 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32768-n1-r2 | 完成 | 286959 | 286959 | 286959 | 286959 | 286951 | 0 | 0 | 0 | 26.95 | 34.92 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/3 | 286959/0/0/0 | 0/286959 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-32768-n1-r3 | 完成 | 293072 | 293072 | 293072 | 293072 | 293064 | 0 | 0 | 0 | 25.89 | 30.89 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/3 | 293072/0/0/0 | 0/293072 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-4096-n1-r1 | 完成 | 567471 | 567471 | 567471 | 567471 | 567463 | 0 | 0 | 0 | 13.41 | 25.34 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 567471/0/0/0 | 0/567471 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-4096-n1-r2 | 完成 | 554695 | 554695 | 554695 | 554695 | 554687 | 0 | 0 | 0 | 14.24 | 24.35 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 554695/0/0/0 | 0/554695 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-4096-n1-r3 | 完成 | 590025 | 590025 | 590025 | 590025 | 590017 | 0 | 0 | 0 | 13.11 | 14.93 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 590025/0/0/0 | 0/590025 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-512-n1-r1 | 完成 | 694608 | 694608 | 694608 | 694608 | 694600 | 0 | 0 | 0 | 11.15 | 13.04 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/3 | 694608/0/0/0 | 0/694608 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-512-n1-r2 | 完成 | 683249 | 683249 | 683249 | 683249 | 683241 | 0 | 0 | 0 | 11.44 | 13.22 | 1.0 | 1.000001 | 1.500001 | 1.00 | 2.50 | 2/3 | 683249/0/0/0 | 0/683249 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-512-n1-r3 | 完成 | 692835 | 692835 | 692835 | 692835 | 692828 | 0 | 0 | 0 | 10.91 | 21.57 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 692835/0/0/0 | 0/692835 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-524288-n1-r1 | 完成 | 26330 | 26330 | 26330 | 26330 | 26328 | 0 | 0 | 0 | 74.91 | 191.17 | 1.0 | 1.000010 | 1.500000 | 1.00 | 2.47 | 2/3 | 26330/0/0/0 | 0/26330 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-524288-n1-r2 | 完成 | 26233 | 26233 | 26233 | 26233 | 26230 | 0 | 0 | 0 | 73.56 | 109.88 | 1.0 | 1.000032 | 1.500000 | 1.00 | 2.45 | 2/3 | 26233/0/0/0 | 0/26233 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-524288-n1-r3 | 完成 | 26085 | 26085 | 26085 | 26085 | 26083 | 0 | 0 | 0 | 89.52 | 111.14 | 1.0 | 1.000024 | 1.500000 | 1.00 | 2.48 | 2/3 | 26085/0/0/0 | 0/26085 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-64-n1-r1 | 完成 | 722247 | 722247 | 722247 | 722247 | 722239 | 0 | 0 | 0 | 10.79 | 12.49 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 722247/0/0/0 | 0/722247 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-64-n1-r2 | 完成 | 761268 | 761268 | 761268 | 761268 | 761260 | 0 | 0 | 0 | 10.12 | 12.59 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 761268/0/0/0 | 0/761268 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-64-n1-r3 | 完成 | 747213 | 747213 | 747213 | 747213 | 747205 | 0 | 0 | 0 | 10.18 | 19.66 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.49 | 2/3 | 747213/0/0/0 | 0/747213 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-65536-n1-r1 | 完成 | 177456 | 177456 | 177456 | 177456 | 177451 | 0 | 0 | 0 | 24.55 | 49.05 | 1.0 | 1.000004 | 1.500000 | 1.00 | 2.50 | 2/3 | 177456/0/0/0 | 0/177456 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-65536-n1-r2 | 完成 | 161471 | 161471 | 161471 | 161471 | 161463 | 0 | 0 | 0 | 49.24 | 51.42 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/3 | 161471/0/0/0 | 0/161471 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-65536-n1-r3 | 完成 | 164427 | 164427 | 164427 | 164427 | 164423 | 0 | 0 | 0 | 47.56 | 49.74 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 164427/0/0/0 | 0/164427 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8-n1-r1 | 完成 | 717266 | 717266 | 717266 | 717266 | 717258 | 0 | 0 | 0 | 10.63 | 19.94 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 717266/0/0/0 | 0/717266 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8-n1-r2 | 完成 | 759399 | 759399 | 759399 | 759399 | 759391 | 0 | 0 | 0 | 10.07 | 20.85 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 759399/0/0/0 | 0/759399 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8-n1-r3 | 完成 | 750457 | 750457 | 750457 | 750457 | 750450 | 0 | 0 | 0 | 10.27 | 13.71 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/3 | 750457/0/0/0 | 0/750457 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8192-n1-r1 | 完成 | 491700 | 491700 | 491700 | 491700 | 491692 | 0 | 0 | 0 | 15.92 | 18.22 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 491700/0/0/0 | 0/491700 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8192-n1-r2 | 完成 | 484094 | 484094 | 484094 | 484094 | 484086 | 0 | 0 | 0 | 16.15 | 18.73 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 484094/0/0/0 | 0/484094 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-shm-8192-n1-r3 | 完成 | 446139 | 446139 | 446139 | 446139 | 446131 | 0 | 0 | 0 | 17.91 | 19.13 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/3 | 446139/0/0/0 | 0/446139 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1024-n1-r1 | 完成 | 72310 | 72310 | 72310 | 72310 | 72302 | 0 | 0 | 0 | 104.75 | 340.23 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1024-n1-r2 | 完成 | 69032 | 69032 | 69032 | 69032 | 69024 | 0 | 0 | 0 | 109.97 | 364.89 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1024-n1-r3 | 完成 | 64405 | 64405 | 64405 | 64405 | 64397 | 0 | 0 | 0 | 111.45 | 396.55 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.48 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1048576-n1-r1 | 完成 | 113 | 113 | 113 | 113 | 112 | 0 | 0 | 0 | 10,842.49 | 14,925.22 | 1.0 | 1.000439 | 1.500000 | 0.27 | 2.25 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1048576-n1-r2 | 有异常 | 115 | 115 | 115 | 106 | 105 | 0 | 105 | 0 | 2,345.74 | 4,701.52 | 1.0 | 1.006171 | 1.500000 | 0.35 | 2.26 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-1048576-n1-r3 | 有异常 | 91 | 91 | 91 | 68 | 68 | 0 | 67 | 0 | 3,158.28 | 9,887.08 | 1.0 | 1.000000 | 2.000814 | 0.42 | 2.66 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-128-n1-r1 | 完成 | 71178 | 71178 | 71178 | 71178 | 71170 | 0 | 0 | 0 | 105.65 | 327.56 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-128-n1-r2 | 完成 | 73850 | 73850 | 73850 | 73850 | 73842 | 0 | 0 | 0 | 101.76 | 322.56 | 1.0 | 1.000005 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-128-n1-r3 | 完成 | 73409 | 73409 | 73409 | 73409 | 73401 | 0 | 0 | 0 | 103.12 | 351.50 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-131072-n1-r1 | 有异常 | 911 | 911 | 911 | 910 | 908 | 0 | 1 | 0 | 2,988.51 | 9,004.86 | 1.0 | 1.002768 | 1.500000 | 0.30 | 2.27 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-131072-n1-r2 | 完成 | 913 | 913 | 913 | 913 | 908 | 0 | 0 | 0 | 3,022.70 | 7,108.01 | 1.0 | 1.000228 | 1.500000 | 0.29 | 2.27 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-131072-n1-r3 | 有异常 | 912 | 912 | 912 | 900 | 897 | 0 | 879 | 0 | 2,102.59 | 6,434.75 | 1.0 | 1.000250 | 1.500000 | 0.34 | 2.26 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16-n1-r1 | 完成 | 73096 | 73096 | 73096 | 73096 | 73088 | 0 | 0 | 0 | 101.95 | 326.65 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16-n1-r2 | 完成 | 73945 | 73945 | 73945 | 73945 | 73937 | 0 | 0 | 0 | 101.63 | 366.94 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16-n1-r3 | 完成 | 69484 | 69484 | 69484 | 69484 | 69476 | 0 | 0 | 0 | 105.51 | 342.07 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16384-n1-r1 | 完成 | 6971 | 6971 | 6971 | 6971 | 6963 | 0 | 0 | 0 | 1,101.03 | 1,554.40 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.46 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16384-n1-r2 | 完成 | 7019 | 7019 | 7019 | 7019 | 7011 | 0 | 0 | 0 | 1,094.88 | 1,569.65 | 1.0 | 1.000000 | 1.500000 | 0.99 | 2.46 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-16384-n1-r3 | 完成 | 7189 | 7189 | 7189 | 7189 | 7188 | 0 | 0 | 0 | 889.57 | 1,717.38 | 1.0 | 1.003997 | 1.500000 | 0.82 | 2.34 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-2048-n1-r1 | 完成 | 41915 | 41915 | 41915 | 41915 | 41907 | 0 | 0 | 0 | 174.13 | 593.13 | 1.0 | 1.000008 | 1.500000 | 0.99 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-2048-n1-r2 | 完成 | 42326 | 42326 | 42326 | 42326 | 42318 | 0 | 0 | 0 | 175.20 | 570.35 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-2048-n1-r3 | 完成 | 43138 | 43138 | 43138 | 43138 | 43130 | 0 | 0 | 0 | 172.40 | 562.83 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-256-n1-r1 | 完成 | 64845 | 64845 | 64845 | 64845 | 64837 | 0 | 0 | 0 | 106.68 | 399.84 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-256-n1-r2 | 完成 | 70845 | 70845 | 70845 | 70845 | 70837 | 0 | 0 | 0 | 105.02 | 324.88 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.48 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-256-n1-r3 | 完成 | 67814 | 67814 | 67814 | 67814 | 67806 | 0 | 0 | 0 | 104.30 | 342.67 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r1 | 有异常 | 457 | 457 | 457 | 449 | 448 | 0 | 443 | 0 | 2,177.40 | 4,756.58 | 1.0 | 1.004880 | 1.500000 | 0.29 | 2.25 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r2 | 有异常 | 457 | 457 | 457 | 422 | 418 | 0 | 415 | 0 | 3,256.41 | 10,708.73 | 1.0 | 1.004801 | 1.500000 | 0.29 | 2.35 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r3 | 有异常 | 456 | 456 | 456 | 455 | 454 | 0 | 1 | 0 | 3,928.76 | 8,048.39 | 1.0 | 1.003102 | 1.500000 | 0.33 | 2.29 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32-n1-r1 | 完成 | 55603 | 55603 | 55603 | 55603 | 55595 | 0 | 0 | 0 | 130.46 | 375.60 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.47 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32-n1-r2 | 完成 | 73477 | 73477 | 73477 | 73477 | 73469 | 0 | 0 | 0 | 102.95 | 341.39 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32-n1-r3 | 完成 | 71785 | 71785 | 71785 | 71785 | 71777 | 0 | 0 | 0 | 105.35 | 390.92 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32768-n1-r1 | 完成 | 3615 | 3615 | 3615 | 3615 | 3607 | 0 | 0 | 0 | 1,731.04 | 2,453.39 | 1.0 | 1.000018 | 1.500000 | 0.75 | 2.30 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32768-n1-r2 | 完成 | 3603 | 3603 | 3603 | 3603 | 3602 | 0 | 0 | 0 | 1,746.96 | 5,649.02 | 1.0 | 1.000335 | 1.500000 | 0.76 | 2.30 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-32768-n1-r3 | 完成 | 3616 | 3616 | 3616 | 3616 | 3615 | 0 | 0 | 0 | 1,757.97 | 2,981.61 | 1.0 | 1.003864 | 1.500000 | 0.78 | 2.33 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-4096-n1-r1 | 完成 | 24853 | 24853 | 24853 | 24853 | 24845 | 0 | 0 | 0 | 310.07 | 688.54 | 1.0 | 1.000010 | 1.500000 | 1.00 | 2.47 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-4096-n1-r2 | 完成 | 26447 | 26447 | 26447 | 26447 | 26439 | 0 | 0 | 0 | 254.88 | 665.61 | 1.0 | 1.000000 | 1.500000 | 0.94 | 2.42 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-4096-n1-r3 | 完成 | 27681 | 27681 | 27681 | 27681 | 27673 | 0 | 0 | 0 | 243.74 | 694.91 | 1.0 | 1.000000 | 1.500000 | 0.89 | 2.37 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-512-n1-r1 | 完成 | 66098 | 66098 | 66098 | 66098 | 66090 | 0 | 0 | 0 | 105.74 | 413.50 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-512-n1-r2 | 完成 | 72710 | 72710 | 72710 | 72710 | 72702 | 0 | 0 | 0 | 104.02 | 332.75 | 1.0 | 1.000002 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-512-n1-r3 | 完成 | 70849 | 70849 | 70849 | 70849 | 70841 | 0 | 0 | 0 | 104.85 | 396.78 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r1 | 有异常 | 213 | 213 | 213 | 200 | 198 | 0 | 197 | 0 | 2,410.42 | 10,923.77 | 1.0 | 1.000000 | 2.000264 | 0.31 | 2.70 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r2 | 有异常 | 56 | 56 | 56 | 39 | 39 | 0 | 14 | 0 | 8,222.19 | 115,461.68 | 1.0 | 1.000000 | 2.000163 | 0.84 | 2.21 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r3 | 有异常 | 228 | 228 | 228 | 190 | 190 | 0 | 183 | 0 | 3,045.36 | 13,834.44 | 1.0 | 1.000272 | 1.500000 | 0.29 | 2.34 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-64-n1-r1 | 完成 | 74578 | 74578 | 74578 | 74578 | 74570 | 0 | 0 | 0 | 101.09 | 406.11 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-64-n1-r2 | 完成 | 74413 | 74413 | 74413 | 74413 | 74405 | 0 | 0 | 0 | 100.59 | 345.61 | 1.0 | 1.000001 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-64-n1-r3 | 完成 | 75739 | 75739 | 75739 | 75739 | 75731 | 0 | 0 | 0 | 99.19 | 323.58 | 1.0 | 1.000003 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-65536-n1-r1 | 完成 | 1814 | 1814 | 1814 | 1814 | 1811 | 0 | 0 | 0 | 3,209.21 | 6,268.38 | 1.0 | 1.003584 | 1.500000 | 0.56 | 2.28 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-65536-n1-r2 | 完成 | 1813 | 1813 | 1813 | 1813 | 1805 | 0 | 0 | 0 | 4,353.01 | 5,082.89 | 1.0 | 1.000054 | 1.500000 | 0.95 | 2.45 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-65536-n1-r3 | 完成 | 1820 | 1820 | 1820 | 1820 | 1814 | 0 | 0 | 0 | 3,225.76 | 6,085.62 | 1.0 | 1.004635 | 1.500000 | 0.54 | 2.27 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8-n1-r1 | 完成 | 71363 | 71363 | 71363 | 71363 | 71355 | 0 | 0 | 0 | 104.50 | 348.19 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8-n1-r2 | 完成 | 71325 | 71325 | 71325 | 71325 | 71317 | 0 | 0 | 0 | 104.07 | 355.97 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.49 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8-n1-r3 | 完成 | 74570 | 74570 | 74570 | 74570 | 74562 | 0 | 0 | 0 | 99.88 | 353.31 | 1.0 | 1.000004 | 1.500000 | 1.00 | 2.50 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8192-n1-r1 | 完成 | 14190 | 14190 | 14190 | 14190 | 14182 | 0 | 0 | 0 | 463.69 | 974.98 | 1.0 | 1.000000 | 1.500000 | 0.86 | 2.35 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8192-n1-r2 | 完成 | 13407 | 13407 | 13407 | 13407 | 13399 | 0 | 0 | 0 | 565.52 | 1,006.69 | 1.0 | 1.000000 | 1.500000 | 1.00 | 2.46 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-pubsub-socket-8192-n1-r3 | 完成 | 14154 | 14154 | 14154 | 14154 | 14146 | 0 | 0 | 0 | 459.45 | 969.39 | 1.0 | 1.000007 | 1.500000 | 0.85 | 2.36 | 2/2 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1024-n1-r1 | 有异常 | 35285 | 35285 | 35284 | 0 | 0 | 1 | 0 | 0 | 9.85 | 12.55 | 1.0 | 1.306712 | 1.500875 | 0.24 | 0.29 | 2/4 | 0/0/0/0 | 35285/0 | 35316/35316 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1024-n1-r2 | 完成 | 127868 | 127868 | 127868 | 0 | 0 | 0 | 0 | 0 | 6.59 | 12.97 | 1.0 | 1.000001 | 1.500433 | 0.83 | 0.96 | 2/4 | 0/0/0/0 | 127868/0 | 127900/127900 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1024-n1-r3 | 有异常 | 7706 | 7706 | 7705 | 0 | 0 | 1 | 0 | 0 | 10.38 | 15.63 | 1.0 | 1.082409 | 1.500861 | 0.06 | 0.08 | 2/4 | 0/0/0/0 | 7706/0 | 7737/7737 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1048576-n1-r1 | 完成 | 22838 | 22838 | 22838 | 0 | 0 | 0 | 0 | 0 | 43.21 | 62.78 | 1.0 | 1.000041 | 1.500008 | 0.94 | 0.21 | 2/4 | 0/0/0/0 | 22838/0 | 22870/22870 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1048576-n1-r2 | 完成 | 21841 | 21841 | 21841 | 0 | 0 | 0 | 0 | 0 | 44.86 | 70.66 | 1.0 | 1.000024 | 1.500736 | 0.92 | 0.22 | 2/4 | 0/0/0/0 | 21841/0 | 21873/21873 | 0/0/0 |
| 原安装速度 | speed-rpc-a-1048576-n1-r3 | 完成 | 22186 | 22186 | 22186 | 0 | 0 | 0 | 0 | 0 | 43.73 | 66.62 | 1.0 | 1.000010 | 1.500910 | 0.90 | 0.25 | 2/4 | 0/0/0/0 | 22186/0 | 22218/22218 | 0/0/0 |
| 原安装速度 | speed-rpc-a-128-n1-r1 | 完成 | 122571 | 122571 | 122571 | 0 | 0 | 0 | 0 | 0 | 8.42 | 13.51 | 1.0 | 1.000008 | 1.500402 | 0.79 | 0.96 | 2/4 | 0/0/0/0 | 122571/0 | 122603/122603 | 0/0/0 |
| 原安装速度 | speed-rpc-a-128-n1-r2 | 有异常 | 2995 | 2995 | 2994 | 0 | 0 | 1 | 0 | 0 | 12.10 | 19.07 | 1.0 | 1.039981 | 1.500853 | 0.02 | 0.04 | 2/4 | 0/0/0/0 | 2995/0 | 3026/3026 | 0/0/0 |
| 原安装速度 | speed-rpc-a-128-n1-r3 | 有异常 | 22774 | 22774 | 22773 | 0 | 0 | 1 | 0 | 0 | 9.69 | 13.51 | 1.0 | 1.198526 | 1.500150 | 0.16 | 0.19 | 2/4 | 0/0/0/0 | 22774/0 | 22805/22805 | 0/0/0 |
| 原安装速度 | speed-rpc-a-131072-n1-r1 | 完成 | 81990 | 81990 | 81990 | 0 | 0 | 0 | 0 | 0 | 13.19 | 18.47 | 1.0 | 1.000001 | 1.500371 | 0.85 | 0.66 | 2/4 | 0/0/0/0 | 81990/0 | 82022/82022 | 0/0/0 |
| 原安装速度 | speed-rpc-a-131072-n1-r2 | 完成 | 97549 | 97549 | 97549 | 0 | 0 | 0 | 0 | 0 | 9.04 | 15.93 | 1.0 | 1.000005 | 1.500980 | 0.93 | 0.66 | 2/4 | 0/0/0/0 | 97549/0 | 97581/97581 | 0/0/0 |
| 原安装速度 | speed-rpc-a-131072-n1-r3 | 完成 | 86052 | 86052 | 86052 | 0 | 0 | 0 | 0 | 0 | 10.01 | 17.07 | 1.0 | 1.000007 | 1.501047 | 0.86 | 0.67 | 2/4 | 0/0/0/0 | 86052/0 | 86084/86084 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16-n1-r1 | 完成 | 132289 | 132289 | 132289 | 0 | 0 | 0 | 0 | 0 | 5.51 | 13.58 | 1.0 | 1.000001 | 1.500070 | 0.81 | 0.98 | 2/4 | 0/0/0/0 | 132289/0 | 132321/132321 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16-n1-r2 | 有异常 | 115454 | 115454 | 115453 | 0 | 0 | 1 | 0 | 0 | 5.41 | 9.99 | 1.0 | 1.646121 | 1.647021 | 0.64 | 0.68 | 2/4 | 0/0/0/0 | 115454/0 | 115485/115485 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16-n1-r3 | 有异常 | 3395 | 3395 | 3394 | 0 | 0 | 1 | 0 | 0 | 11.94 | 16.70 | 1.0 | 1.040695 | 1.500827 | 0.03 | 0.05 | 2/4 | 0/0/0/0 | 3395/0 | 3426/3426 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16384-n1-r1 | 有异常 | 58618 | 58618 | 58617 | 0 | 0 | 1 | 0 | 0 | 5.84 | 13.36 | 1.0 | 1.437150 | 1.500863 | 0.40 | 0.40 | 2/4 | 0/0/0/0 | 58618/0 | 58649/58649 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16384-n1-r2 | 完成 | 126413 | 126413 | 126413 | 0 | 0 | 0 | 0 | 0 | 5.98 | 14.74 | 1.0 | 1.000001 | 1.500387 | 0.85 | 0.92 | 2/4 | 0/0/0/0 | 126413/0 | 126445/126445 | 0/0/0 |
| 原安装速度 | speed-rpc-a-16384-n1-r3 | 有异常 | 25709 | 25709 | 25708 | 0 | 0 | 1 | 0 | 0 | 5.81 | 10.69 | 1.0 | 1.161968 | 1.500561 | 0.16 | 0.16 | 2/4 | 0/0/0/0 | 25709/0 | 25740/25740 | 0/0/0 |
| 原安装速度 | speed-rpc-a-2048-n1-r1 | 完成 | 157275 | 157275 | 157275 | 0 | 0 | 0 | 0 | 0 | 5.42 | 13.26 | 1.0 | 1.000007 | 1.500546 | 0.93 | 1.02 | 2/4 | 0/0/0/0 | 157275/0 | 157307/157307 | 0/0/0 |
| 原安装速度 | speed-rpc-a-2048-n1-r2 | 完成 | 157694 | 157694 | 157694 | 0 | 0 | 0 | 0 | 0 | 5.70 | 12.67 | 1.0 | 1.000003 | 1.500001 | 0.94 | 1.05 | 2/4 | 0/0/0/0 | 157694/0 | 157726/157726 | 0/0/0 |
| 原安装速度 | speed-rpc-a-2048-n1-r3 | 有异常 | 79288 | 79288 | 79287 | 0 | 0 | 1 | 0 | 0 | 9.74 | 13.36 | 1.0 | 1.668661 | 1.669099 | 0.52 | 0.63 | 2/4 | 0/0/0/0 | 79288/0 | 79319/79319 | 0/0/0 |
| 原安装速度 | speed-rpc-a-256-n1-r1 | 有异常 | 54488 | 54488 | 54487 | 0 | 0 | 1 | 0 | 0 | 5.38 | 12.58 | 1.0 | 1.367376 | 1.500170 | 0.33 | 0.37 | 2/4 | 0/0/0/0 | 54488/0 | 54519/54519 | 0/0/0 |
| 原安装速度 | speed-rpc-a-256-n1-r2 | 有异常 | 111995 | 111995 | 111994 | 0 | 0 | 1 | 0 | 0 | 5.71 | 10.91 | 1.0 | 1.720746 | 1.721736 | 0.67 | 0.77 | 2/4 | 0/0/0/0 | 111995/0 | 112026/112026 | 0/0/0 |
| 原安装速度 | speed-rpc-a-256-n1-r3 | 有异常 | 10699 | 10699 | 10698 | 0 | 0 | 1 | 0 | 0 | 10.76 | 17.54 | 1.0 | 1.124692 | 1.500246 | 0.09 | 0.12 | 2/4 | 0/0/0/0 | 10699/0 | 10730/10730 | 0/0/0 |
| 原安装速度 | speed-rpc-a-262144-n1-r1 | 完成 | 63997 | 63997 | 63997 | 0 | 0 | 0 | 0 | 0 | 16.42 | 22.02 | 1.0 | 1.000003 | 1.500279 | 0.88 | 0.53 | 2/4 | 0/0/0/0 | 63997/0 | 64029/64029 | 0/0/0 |
| 原安装速度 | speed-rpc-a-262144-n1-r2 | 完成 | 57963 | 57963 | 57963 | 0 | 0 | 0 | 0 | 0 | 16.87 | 25.72 | 1.0 | 1.000012 | 1.500729 | 0.82 | 0.54 | 2/4 | 0/0/0/0 | 57963/0 | 57995/57995 | 0/0/0 |
| 原安装速度 | speed-rpc-a-262144-n1-r3 | 完成 | 63540 | 63540 | 63540 | 0 | 0 | 0 | 0 | 0 | 16.16 | 25.44 | 1.0 | 1.000009 | 1.500861 | 0.86 | 0.54 | 2/4 | 0/0/0/0 | 63540/0 | 63572/63572 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32-n1-r1 | 完成 | 147911 | 147911 | 147911 | 0 | 0 | 0 | 0 | 0 | 5.41 | 13.16 | 1.0 | 1.000004 | 1.500385 | 0.88 | 1.00 | 2/4 | 0/0/0/0 | 147911/0 | 147943/147943 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32-n1-r2 | 有异常 | 75128 | 75128 | 75127 | 0 | 0 | 1 | 0 | 0 | 5.49 | 13.13 | 1.0 | 1.500340 | 1.501198 | 0.46 | 0.51 | 2/4 | 0/0/0/0 | 75128/0 | 75159/75159 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32-n1-r3 | 有异常 | 3336 | 3336 | 3335 | 0 | 0 | 1 | 0 | 0 | 9.86 | 15.11 | 1.0 | 1.035277 | 1.500534 | 0.02 | 0.04 | 2/4 | 0/0/0/0 | 3336/0 | 3367/3367 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32768-n1-r1 | 有异常 | 84126 | 84126 | 84125 | 0 | 0 | 1 | 0 | 0 | 6.44 | 13.30 | 1.0 | 1.655947 | 1.655997 | 0.60 | 0.58 | 2/4 | 0/0/0/0 | 84126/0 | 84157/84157 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32768-n1-r2 | 有异常 | 22607 | 22607 | 22606 | 0 | 0 | 1 | 0 | 0 | 6.54 | 13.76 | 1.0 | 1.189036 | 1.500448 | 0.16 | 0.17 | 2/4 | 0/0/0/0 | 22607/0 | 22638/22638 | 0/0/0 |
| 原安装速度 | speed-rpc-a-32768-n1-r3 | 完成 | 137471 | 137471 | 137471 | 0 | 0 | 0 | 0 | 0 | 6.38 | 13.36 | 1.0 | 1.000002 | 1.500720 | 0.92 | 0.88 | 2/4 | 0/0/0/0 | 137471/0 | 137503/137503 | 0/0/0 |
| 原安装速度 | speed-rpc-a-4096-n1-r1 | 有异常 | 29128 | 29128 | 29127 | 0 | 0 | 1 | 0 | 0 | 10.33 | 14.86 | 1.0 | 1.268195 | 1.500947 | 0.20 | 0.26 | 2/4 | 0/0/0/0 | 29128/0 | 29159/29159 | 0/0/0 |
| 原安装速度 | speed-rpc-a-4096-n1-r2 | 有异常 | 13204 | 13204 | 13203 | 0 | 0 | 1 | 0 | 0 | 10.64 | 16.94 | 1.0 | 1.130206 | 1.500323 | 0.11 | 0.13 | 2/4 | 0/0/0/0 | 13204/0 | 13235/13235 | 0/0/0 |
| 原安装速度 | speed-rpc-a-4096-n1-r3 | 有异常 | 80475 | 80475 | 80474 | 0 | 0 | 1 | 0 | 0 | 5.76 | 10.38 | 1.0 | 1.500029 | 1.500804 | 0.49 | 0.54 | 2/4 | 0/0/0/0 | 80475/0 | 80506/80506 | 0/0/0 |
| 原安装速度 | speed-rpc-a-512-n1-r1 | 完成 | 180799 | 180799 | 180799 | 0 | 0 | 0 | 0 | 0 | 5.27 | 8.74 | 1.0 | 1.000005 | 1.500896 | 0.99 | 1.04 | 2/4 | 0/0/0/0 | 180799/0 | 180831/180831 | 0/0/0 |
| 原安装速度 | speed-rpc-a-512-n1-r2 | 有异常 | 19828 | 19828 | 19827 | 0 | 0 | 1 | 0 | 0 | 10.03 | 16.02 | 1.0 | 1.188534 | 1.500439 | 0.15 | 0.18 | 2/4 | 0/0/0/0 | 19828/0 | 19859/19859 | 0/0/0 |
| 原安装速度 | speed-rpc-a-512-n1-r3 | 有异常 | 128695 | 128695 | 128694 | 0 | 0 | 1 | 0 | 0 | 5.43 | 13.11 | 1.0 | 1.816660 | 1.816892 | 0.75 | 0.83 | 2/4 | 0/0/0/0 | 128695/0 | 128726/128726 | 0/0/0 |
| 原安装速度 | speed-rpc-a-524288-n1-r1 | 完成 | 44050 | 44050 | 44050 | 0 | 0 | 0 | 0 | 0 | 22.91 | 36.08 | 1.0 | 1.000020 | 1.500430 | 0.91 | 0.35 | 2/4 | 0/0/0/0 | 44050/0 | 44082/44082 | 0/0/0 |
| 原安装速度 | speed-rpc-a-524288-n1-r2 | 完成 | 43049 | 43049 | 43049 | 0 | 0 | 0 | 0 | 0 | 23.30 | 36.11 | 1.0 | 1.000004 | 1.500926 | 0.89 | 0.38 | 2/4 | 0/0/0/0 | 43049/0 | 43081/43081 | 0/0/0 |
| 原安装速度 | speed-rpc-a-524288-n1-r3 | 完成 | 41576 | 41576 | 41576 | 0 | 0 | 0 | 0 | 0 | 23.48 | 38.67 | 1.0 | 1.000001 | 1.500389 | 0.88 | 0.38 | 2/4 | 0/0/0/0 | 41576/0 | 41608/41608 | 0/0/0 |
| 原安装速度 | speed-rpc-a-64-n1-r1 | 有异常 | 2624 | 2624 | 2623 | 0 | 0 | 1 | 0 | 0 | 12.35 | 19.03 | 1.0 | 1.033436 | 1.500081 | 0.02 | 0.04 | 2/4 | 0/0/0/0 | 2624/0 | 2655/2655 | 0/0/0 |
| 原安装速度 | speed-rpc-a-64-n1-r2 | 完成 | 147747 | 147747 | 147747 | 0 | 0 | 0 | 0 | 0 | 5.32 | 12.55 | 1.0 | 1.000005 | 1.500832 | 0.88 | 0.99 | 2/4 | 0/0/0/0 | 147747/0 | 147779/147779 | 0/0/0 |
| 原安装速度 | speed-rpc-a-64-n1-r3 | 有异常 | 4240 | 4240 | 4239 | 0 | 0 | 1 | 0 | 0 | 10.39 | 19.52 | 1.0 | 1.044712 | 1.500142 | 0.03 | 0.05 | 2/4 | 0/0/0/0 | 4240/0 | 4271/4271 | 0/0/0 |
| 原安装速度 | speed-rpc-a-65536-n1-r1 | 完成 | 102300 | 102300 | 102300 | 0 | 0 | 0 | 0 | 0 | 7.49 | 15.51 | 1.0 | 1.000008 | 1.500354 | 0.85 | 0.77 | 2/4 | 0/0/0/0 | 102300/0 | 102332/102332 | 0/0/0 |
| 原安装速度 | speed-rpc-a-65536-n1-r2 | 完成 | 106462 | 106462 | 106462 | 0 | 0 | 0 | 0 | 0 | 7.37 | 14.93 | 1.0 | 1.000004 | 1.500206 | 0.87 | 0.78 | 2/4 | 0/0/0/0 | 106462/0 | 106494/106494 | 0/0/0 |
| 原安装速度 | speed-rpc-a-65536-n1-r3 | 有异常 | 44912 | 44912 | 44911 | 0 | 0 | 1 | 0 | 0 | 10.84 | 14.90 | 1.0 | 1.443359 | 1.500649 | 0.37 | 0.35 | 2/4 | 0/0/0/0 | 44912/0 | 44943/44943 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8-n1-r1 | 有异常 | 2906 | 2906 | 2905 | 0 | 0 | 1 | 0 | 0 | 10.66 | 29.14 | 1.0 | 1.038163 | 1.500827 | 0.03 | 0.04 | 2/4 | 0/0/0/0 | 2906/0 | 2937/2937 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8-n1-r2 | 完成 | 145835 | 145835 | 145835 | 0 | 0 | 0 | 0 | 0 | 5.42 | 13.03 | 1.0 | 1.000005 | 1.500588 | 0.90 | 0.99 | 2/4 | 0/0/0/0 | 145835/0 | 145867/145867 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8-n1-r3 | 完成 | 155880 | 155880 | 155880 | 0 | 0 | 0 | 0 | 0 | 5.44 | 13.23 | 1.0 | 1.000008 | 1.500085 | 0.91 | 1.01 | 2/4 | 0/0/0/0 | 155880/0 | 155912/155912 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8192-n1-r1 | 有异常 | 38753 | 38753 | 38752 | 0 | 0 | 1 | 0 | 0 | 5.65 | 12.61 | 1.0 | 1.288331 | 1.500313 | 0.25 | 0.28 | 2/4 | 0/0/0/0 | 38753/0 | 38784/38784 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8192-n1-r2 | 有异常 | 26279 | 26279 | 26278 | 0 | 0 | 1 | 0 | 0 | 10.19 | 14.34 | 1.0 | 1.250422 | 1.500629 | 0.19 | 0.24 | 2/4 | 0/0/0/0 | 26279/0 | 26310/26310 | 0/0/0 |
| 原安装速度 | speed-rpc-a-8192-n1-r3 | 完成 | 128645 | 128645 | 128645 | 0 | 0 | 0 | 0 | 0 | 5.72 | 13.16 | 1.0 | 1.000001 | 1.500327 | 0.83 | 0.95 | 2/4 | 0/0/0/0 | 128645/0 | 128677/128677 | 0/0/0 |
| 原安装速度 | speed-rpc-b-1024-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-1024-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-1024-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-1048576-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-1048576-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-1048576-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-128-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-128-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-128-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-131072-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-131072-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-131072-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16384-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16384-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-16384-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-2048-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-2048-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-2048-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-256-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-256-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-256-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-262144-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-262144-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-262144-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32768-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32768-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-32768-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-4096-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-4096-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-4096-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-512-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-512-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-512-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-524288-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-524288-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-524288-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-64-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-64-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-64-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-65536-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-65536-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-65536-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8192-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8192-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-b-8192-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-dds-iox-1024-n1-r1 | 完成 | 166201 | 166201 | 166201 | 0 | 0 | 0 | 0 | 0 | 5.43 | 11.12 | 1.0 | 1.000003 | 1.500730 | 1.47 | 0.58 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1663/0/1663 |
| 原安装速度 | speed-rpc-dds-iox-1024-n1-r2 | 完成 | 133031 | 133031 | 133031 | 0 | 0 | 0 | 0 | 0 | 7.57 | 12.19 | 1.0 | 1.000005 | 1.500471 | 1.56 | 0.56 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1331/0/1331 |
| 原安装速度 | speed-rpc-dds-iox-1024-n1-r3 | 完成 | 152980 | 152980 | 152980 | 0 | 0 | 0 | 0 | 0 | 5.57 | 11.73 | 1.0 | 1.000002 | 1.500091 | 1.46 | 0.64 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1531/0/1531 |
| 原安装速度 | speed-rpc-dds-iox-1048576-n1-r1 | 完成 | 1137 | 1137 | 1137 | 0 | 0 | 0 | 0 | 0 | 863.66 | 1,081.35 | 1.0 | 1.000743 | 1.500853 | 1.02 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-rpc-dds-iox-1048576-n1-r2 | 完成 | 1145 | 1145 | 1145 | 0 | 0 | 0 | 0 | 0 | 854.26 | 1,099.26 | 1.0 | 1.000257 | 1.500539 | 1.01 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-rpc-dds-iox-1048576-n1-r3 | 完成 | 1137 | 1137 | 1137 | 0 | 0 | 0 | 0 | 0 | 860.59 | 1,137.84 | 1.0 | 1.000505 | 1.500035 | 1.02 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 12/0/12 |
| 原安装速度 | speed-rpc-dds-iox-128-n1-r1 | 完成 | 172326 | 172326 | 172326 | 0 | 0 | 0 | 0 | 0 | 5.24 | 9.64 | 1.0 | 1.000002 | 1.500586 | 1.48 | 0.55 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1724/0/1724 |
| 原安装速度 | speed-rpc-dds-iox-128-n1-r2 | 完成 | 157139 | 157139 | 157139 | 0 | 0 | 0 | 0 | 0 | 5.48 | 11.39 | 1.0 | 1.000002 | 1.500579 | 1.48 | 0.56 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1572/0/1572 |
| 原安装速度 | speed-rpc-dds-iox-128-n1-r3 | 完成 | 146135 | 146135 | 146135 | 0 | 0 | 0 | 0 | 0 | 7.24 | 11.57 | 1.0 | 1.000006 | 1.500255 | 1.54 | 0.53 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1462/0/1462 |
| 原安装速度 | speed-rpc-dds-iox-131072-n1-r1 | 完成 | 11857 | 11857 | 11857 | 0 | 0 | 0 | 0 | 0 | 81.91 | 132.66 | 1.0 | 1.000001 | 1.500556 | 1.05 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 119/0/119 |
| 原安装速度 | speed-rpc-dds-iox-131072-n1-r2 | 完成 | 12397 | 12397 | 12397 | 0 | 0 | 0 | 0 | 0 | 80.24 | 86.57 | 1.0 | 1.000028 | 1.500334 | 1.05 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 125/0/125 |
| 原安装速度 | speed-rpc-dds-iox-131072-n1-r3 | 完成 | 12198 | 12198 | 12198 | 0 | 0 | 0 | 0 | 0 | 81.05 | 89.48 | 1.0 | 1.000046 | 1.501004 | 1.04 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 123/0/123 |
| 原安装速度 | speed-rpc-dds-iox-16-n1-r1 | 完成 | 153001 | 153001 | 153001 | 0 | 0 | 0 | 0 | 0 | 5.59 | 11.48 | 1.0 | 1.000004 | 1.500879 | 1.53 | 0.53 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1531/0/1531 |
| 原安装速度 | speed-rpc-dds-iox-16-n1-r2 | 完成 | 172718 | 172718 | 172718 | 0 | 0 | 0 | 0 | 0 | 5.12 | 11.36 | 1.0 | 1.000002 | 1.500213 | 1.46 | 0.55 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1728/0/1728 |
| 原安装速度 | speed-rpc-dds-iox-16-n1-r3 | 完成 | 167596 | 167596 | 167596 | 0 | 0 | 0 | 0 | 0 | 5.71 | 8.55 | 1.0 | 1.000001 | 1.500426 | 1.45 | 0.61 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1677/0/1677 |
| 原安装速度 | speed-rpc-dds-iox-16384-n1-r1 | 完成 | 113979 | 113979 | 113979 | 0 | 0 | 0 | 0 | 0 | 8.57 | 10.54 | 1.0 | 1.000002 | 1.500608 | 1.43 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1141/0/1141 |
| 原安装速度 | speed-rpc-dds-iox-16384-n1-r2 | 完成 | 114176 | 114176 | 114176 | 0 | 0 | 0 | 0 | 0 | 8.59 | 10.46 | 1.0 | 1.000001 | 1.500771 | 1.31 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1143/0/1143 |
| 原安装速度 | speed-rpc-dds-iox-16384-n1-r3 | 完成 | 111792 | 111792 | 111792 | 0 | 0 | 0 | 0 | 0 | 8.61 | 18.58 | 1.0 | 1.000007 | 1.500757 | 1.35 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1119/0/1119 |
| 原安装速度 | speed-rpc-dds-iox-2048-n1-r1 | 完成 | 158140 | 158140 | 158140 | 0 | 0 | 0 | 0 | 0 | 5.62 | 12.26 | 1.0 | 1.000003 | 1.500869 | 1.43 | 0.67 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1582/0/1582 |
| 原安装速度 | speed-rpc-dds-iox-2048-n1-r2 | 完成 | 143962 | 143962 | 143962 | 0 | 0 | 0 | 0 | 0 | 6.07 | 12.09 | 1.0 | 1.000004 | 1.500998 | 1.52 | 0.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1440/0/1440 |
| 原安装速度 | speed-rpc-dds-iox-2048-n1-r3 | 完成 | 153765 | 153765 | 153765 | 0 | 0 | 0 | 0 | 0 | 5.74 | 12.41 | 1.0 | 1.000001 | 1.500372 | 1.42 | 0.69 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1538/0/1538 |
| 原安装速度 | speed-rpc-dds-iox-256-n1-r1 | 完成 | 147057 | 147057 | 147057 | 0 | 0 | 0 | 0 | 0 | 5.67 | 11.86 | 1.0 | 1.000006 | 1.500139 | 1.53 | 0.54 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1471/0/1471 |
| 原安装速度 | speed-rpc-dds-iox-256-n1-r2 | 完成 | 170243 | 170243 | 170243 | 0 | 0 | 0 | 0 | 0 | 5.31 | 10.95 | 1.0 | 1.000001 | 1.501011 | 1.38 | 0.66 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1703/0/1703 |
| 原安装速度 | speed-rpc-dds-iox-256-n1-r3 | 完成 | 174966 | 174966 | 174966 | 0 | 0 | 0 | 0 | 0 | 5.42 | 11.76 | 1.0 | 1.000004 | 1.500810 | 1.41 | 0.61 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1750/0/1750 |
| 原安装速度 | speed-rpc-dds-iox-262144-n1-r1 | 完成 | 6297 | 6297 | 6297 | 0 | 0 | 0 | 0 | 0 | 158.41 | 166.55 | 1.0 | 1.000145 | 1.500591 | 1.04 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 64/0/64 |
| 原安装速度 | speed-rpc-dds-iox-262144-n1-r2 | 完成 | 6238 | 6238 | 6238 | 0 | 0 | 0 | 0 | 0 | 159.99 | 169.51 | 1.0 | 1.000110 | 1.500173 | 1.03 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 63/0/63 |
| 原安装速度 | speed-rpc-dds-iox-262144-n1-r3 | 完成 | 6038 | 6038 | 6038 | 0 | 0 | 0 | 0 | 0 | 162.75 | 286.07 | 1.0 | 1.000099 | 1.500741 | 1.03 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 61/0/61 |
| 原安装速度 | speed-rpc-dds-iox-32-n1-r1 | 完成 | 165245 | 165245 | 165245 | 0 | 0 | 0 | 0 | 0 | 5.18 | 11.79 | 1.0 | 1.000006 | 1.501028 | 1.45 | 0.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1653/0/1653 |
| 原安装速度 | speed-rpc-dds-iox-32-n1-r2 | 完成 | 145956 | 145956 | 145956 | 0 | 0 | 0 | 0 | 0 | 5.45 | 12.30 | 1.0 | 1.000008 | 1.500625 | 1.45 | 0.60 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1460/0/1460 |
| 原安装速度 | speed-rpc-dds-iox-32-n1-r3 | 完成 | 163641 | 163641 | 163641 | 0 | 0 | 0 | 0 | 0 | 5.43 | 11.81 | 1.0 | 1.000002 | 1.500861 | 1.43 | 0.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1637/0/1637 |
| 原安装速度 | speed-rpc-dds-iox-32768-n1-r1 | 完成 | 64216 | 64216 | 64216 | 0 | 0 | 0 | 0 | 0 | 15.09 | 33.13 | 1.0 | 1.000004 | 1.500384 | 1.29 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 643/0/643 |
| 原安装速度 | speed-rpc-dds-iox-32768-n1-r2 | 完成 | 63016 | 63016 | 63016 | 0 | 0 | 0 | 0 | 0 | 15.43 | 19.64 | 1.0 | 1.000009 | 1.500235 | 1.27 | 0.99 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 631/0/631 |
| 原安装速度 | speed-rpc-dds-iox-32768-n1-r3 | 完成 | 63898 | 63898 | 63898 | 0 | 0 | 0 | 0 | 0 | 15.28 | 32.74 | 1.0 | 1.000006 | 1.500636 | 1.18 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 640/0/640 |
| 原安装速度 | speed-rpc-dds-iox-4096-n1-r1 | 完成 | 164633 | 164633 | 164633 | 0 | 0 | 0 | 0 | 0 | 5.87 | 9.66 | 1.0 | 1.000004 | 1.500799 | 1.37 | 0.81 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1647/0/1647 |
| 原安装速度 | speed-rpc-dds-iox-4096-n1-r2 | 完成 | 137578 | 137578 | 137578 | 0 | 0 | 0 | 0 | 0 | 7.57 | 12.36 | 1.0 | 1.000000 | 1.500093 | 1.50 | 0.74 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1377/0/1377 |
| 原安装速度 | speed-rpc-dds-iox-4096-n1-r3 | 完成 | 154492 | 154492 | 154492 | 0 | 0 | 0 | 0 | 0 | 6.11 | 9.53 | 1.0 | 1.000000 | 1.500018 | 1.39 | 0.78 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1546/0/1546 |
| 原安装速度 | speed-rpc-dds-iox-512-n1-r1 | 完成 | 168044 | 168044 | 168044 | 0 | 0 | 0 | 0 | 0 | 5.35 | 10.88 | 1.0 | 1.000004 | 1.501019 | 1.46 | 0.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1681/0/1681 |
| 原安装速度 | speed-rpc-dds-iox-512-n1-r2 | 完成 | 148345 | 148345 | 148345 | 0 | 0 | 0 | 0 | 0 | 5.55 | 12.17 | 1.0 | 1.000004 | 1.500849 | 1.48 | 0.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1484/0/1484 |
| 原安装速度 | speed-rpc-dds-iox-512-n1-r3 | 完成 | 138196 | 138196 | 138196 | 0 | 0 | 0 | 0 | 0 | 6.74 | 12.98 | 1.0 | 1.000003 | 1.500888 | 1.50 | 0.58 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1383/0/1383 |
| 原安装速度 | speed-rpc-dds-iox-524288-n1-r1 | 完成 | 3126 | 3126 | 3126 | 0 | 0 | 0 | 0 | 0 | 319.02 | 333.96 | 1.0 | 1.000095 | 1.500454 | 1.02 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 32/0/32 |
| 原安装速度 | speed-rpc-dds-iox-524288-n1-r2 | 完成 | 3113 | 3113 | 3113 | 0 | 0 | 0 | 0 | 0 | 319.86 | 350.23 | 1.0 | 1.000123 | 1.500372 | 1.01 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 32/0/32 |
| 原安装速度 | speed-rpc-dds-iox-524288-n1-r3 | 完成 | 3091 | 3091 | 3091 | 0 | 0 | 0 | 0 | 0 | 319.82 | 443.65 | 1.0 | 1.000297 | 1.500898 | 1.02 | 1.01 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 32/0/32 |
| 原安装速度 | speed-rpc-dds-iox-64-n1-r1 | 完成 | 162660 | 162660 | 162660 | 0 | 0 | 0 | 0 | 0 | 5.37 | 11.42 | 1.0 | 1.000005 | 1.500996 | 1.46 | 0.57 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1627/0/1627 |
| 原安装速度 | speed-rpc-dds-iox-64-n1-r2 | 完成 | 153631 | 153631 | 153631 | 0 | 0 | 0 | 0 | 0 | 5.47 | 11.24 | 1.0 | 1.000003 | 1.500121 | 1.49 | 0.57 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1537/0/1537 |
| 原安装速度 | speed-rpc-dds-iox-64-n1-r3 | 完成 | 158035 | 158035 | 158035 | 0 | 0 | 0 | 0 | 0 | 5.29 | 12.97 | 1.0 | 1.000004 | 1.500139 | 1.43 | 0.60 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1581/0/1581 |
| 原安装速度 | speed-rpc-dds-iox-65536-n1-r1 | 失败 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-dds-iox-65536-n1-r2 | 失败 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-dds-iox-65536-n1-r3 | 完成 | 30706 | 30706 | 30706 | 0 | 0 | 0 | 0 | 0 | 31.46 | 62.46 | 1.0 | 1.000016 | 1.500367 | 1.14 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 308/0/308 |
| 原安装速度 | speed-rpc-dds-iox-8-n1-r1 | 完成 | 157654 | 157654 | 157654 | 0 | 0 | 0 | 0 | 0 | 5.35 | 11.47 | 1.0 | 1.000001 | 1.501034 | 1.51 | 0.53 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1577/0/1577 |
| 原安装速度 | speed-rpc-dds-iox-8-n1-r2 | 完成 | 168211 | 168211 | 168211 | 0 | 0 | 0 | 0 | 0 | 5.29 | 11.46 | 1.0 | 1.000004 | 1.500559 | 1.42 | 0.60 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1683/0/1683 |
| 原安装速度 | speed-rpc-dds-iox-8-n1-r3 | 完成 | 172248 | 172248 | 172248 | 0 | 0 | 0 | 0 | 0 | 5.32 | 11.14 | 1.0 | 1.000002 | 1.500511 | 1.44 | 0.58 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1723/0/1723 |
| 原安装速度 | speed-rpc-dds-iox-8192-n1-r1 | 完成 | 182383 | 182383 | 182383 | 0 | 0 | 0 | 0 | 0 | 5.24 | 11.22 | 1.0 | 1.000002 | 1.500252 | 1.38 | 1.00 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1825/0/1825 |
| 原安装速度 | speed-rpc-dds-iox-8192-n1-r2 | 完成 | 137096 | 137096 | 137096 | 0 | 0 | 0 | 0 | 0 | 7.80 | 11.57 | 1.0 | 1.000002 | 1.500874 | 1.50 | 0.85 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1372/0/1372 |
| 原安装速度 | speed-rpc-dds-iox-8192-n1-r3 | 完成 | 179283 | 179283 | 179283 | 0 | 0 | 0 | 0 | 0 | 5.25 | 9.42 | 1.0 | 1.000002 | 1.500265 | 1.42 | 0.98 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 1794/0/1794 |
| 原安装速度 | speed-rpc-dds-udp-1024-n1-r1 | 完成 | 49339 | 49339 | 49339 | 0 | 0 | 0 | 0 | 0 | 17.82 | 40.48 | 1.0 | 1.000012 | 1.500241 | 1.28 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/494 |
| 原安装速度 | speed-rpc-dds-udp-1024-n1-r2 | 完成 | 50793 | 50793 | 50793 | 0 | 0 | 0 | 0 | 0 | 17.72 | 38.21 | 1.0 | 1.000004 | 1.500672 | 1.26 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/509 |
| 原安装速度 | speed-rpc-dds-udp-1024-n1-r3 | 完成 | 46749 | 46749 | 46749 | 0 | 0 | 0 | 0 | 0 | 18.34 | 41.47 | 1.0 | 1.000003 | 1.500361 | 1.29 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/468 |
| 原安装速度 | speed-rpc-dds-udp-1048576-n1-r1 | 完成 | 694 | 694 | 694 | 0 | 0 | 0 | 0 | 0 | 1,423.75 | 1,816.54 | 1.0 | 1.000150 | 1.500812 | 1.01 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/8 |
| 原安装速度 | speed-rpc-dds-udp-1048576-n1-r2 | 完成 | 708 | 708 | 708 | 0 | 0 | 0 | 0 | 0 | 1,395.26 | 1,725.33 | 1.0 | 1.000173 | 1.500624 | 1.01 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/8 |
| 原安装速度 | speed-rpc-dds-udp-1048576-n1-r3 | 完成 | 680 | 680 | 680 | 0 | 0 | 0 | 0 | 0 | 1,436.15 | 1,885.90 | 1.0 | 1.000739 | 1.500942 | 1.02 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/8 |
| 原安装速度 | speed-rpc-dds-udp-128-n1-r1 | 完成 | 50888 | 50888 | 50888 | 0 | 0 | 0 | 0 | 0 | 17.97 | 34.04 | 1.0 | 1.000006 | 1.500595 | 1.29 | 0.53 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/510 |
| 原安装速度 | speed-rpc-dds-udp-128-n1-r2 | 完成 | 51673 | 51673 | 51673 | 0 | 0 | 0 | 0 | 0 | 18.06 | 35.39 | 1.0 | 1.000015 | 1.500025 | 1.25 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/518 |
| 原安装速度 | speed-rpc-dds-udp-128-n1-r3 | 完成 | 51755 | 51755 | 51755 | 0 | 0 | 0 | 0 | 0 | 17.40 | 36.93 | 1.0 | 1.000012 | 1.500573 | 1.26 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/518 |
| 原安装速度 | speed-rpc-dds-udp-131072-n1-r1 | 完成 | 8147 | 8147 | 8147 | 0 | 0 | 0 | 0 | 0 | 121.34 | 146.99 | 1.0 | 1.000027 | 1.501009 | 1.04 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/82 |
| 原安装速度 | speed-rpc-dds-udp-131072-n1-r2 | 完成 | 8041 | 8041 | 8041 | 0 | 0 | 0 | 0 | 0 | 122.49 | 153.45 | 1.0 | 1.000046 | 1.500232 | 1.04 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/81 |
| 原安装速度 | speed-rpc-dds-udp-131072-n1-r3 | 完成 | 8101 | 8101 | 8101 | 0 | 0 | 0 | 0 | 0 | 120.95 | 185.15 | 1.0 | 1.000016 | 1.500747 | 1.06 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/82 |
| 原安装速度 | speed-rpc-dds-udp-16-n1-r1 | 完成 | 54298 | 54298 | 54298 | 0 | 0 | 0 | 0 | 0 | 17.72 | 28.25 | 1.0 | 1.000017 | 1.500545 | 1.25 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/544 |
| 原安装速度 | speed-rpc-dds-udp-16-n1-r2 | 完成 | 48056 | 48056 | 48056 | 0 | 0 | 0 | 0 | 0 | 21.12 | 37.76 | 1.0 | 1.000005 | 1.500453 | 1.34 | 0.49 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/481 |
| 原安装速度 | speed-rpc-dds-udp-16-n1-r3 | 完成 | 49866 | 49866 | 49866 | 0 | 0 | 0 | 0 | 0 | 17.34 | 38.87 | 1.0 | 1.000006 | 1.500678 | 1.26 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/499 |
| 原安装速度 | speed-rpc-dds-udp-16384-n1-r1 | 完成 | 33311 | 33311 | 33311 | 0 | 0 | 0 | 0 | 0 | 29.95 | 49.74 | 1.0 | 1.000006 | 1.500769 | 1.24 | 0.74 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/334 |
| 原安装速度 | speed-rpc-dds-udp-16384-n1-r2 | 完成 | 35586 | 35586 | 35586 | 0 | 0 | 0 | 0 | 0 | 26.96 | 44.31 | 1.0 | 1.000018 | 1.500552 | 1.17 | 0.79 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/357 |
| 原安装速度 | speed-rpc-dds-udp-16384-n1-r3 | 完成 | 36156 | 36156 | 36156 | 0 | 0 | 0 | 0 | 0 | 26.27 | 45.97 | 1.0 | 1.000007 | 1.500742 | 1.18 | 0.80 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/362 |
| 原安装速度 | speed-rpc-dds-udp-2048-n1-r1 | 完成 | 51491 | 51491 | 51491 | 0 | 0 | 0 | 0 | 0 | 17.97 | 41.34 | 1.0 | 1.000012 | 1.500167 | 1.24 | 0.58 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/516 |
| 原安装速度 | speed-rpc-dds-udp-2048-n1-r2 | 完成 | 48626 | 48626 | 48626 | 0 | 0 | 0 | 0 | 0 | 18.25 | 40.10 | 1.0 | 1.000009 | 1.500665 | 1.25 | 0.60 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/487 |
| 原安装速度 | speed-rpc-dds-udp-2048-n1-r3 | 完成 | 49383 | 49383 | 49383 | 0 | 0 | 0 | 0 | 0 | 18.13 | 38.51 | 1.0 | 1.000029 | 1.500601 | 1.26 | 0.59 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/495 |
| 原安装速度 | speed-rpc-dds-udp-256-n1-r1 | 完成 | 51819 | 51819 | 51819 | 0 | 0 | 0 | 0 | 0 | 18.03 | 39.49 | 1.0 | 1.000018 | 1.500363 | 1.25 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/519 |
| 原安装速度 | speed-rpc-dds-udp-256-n1-r2 | 完成 | 51677 | 51677 | 51677 | 0 | 0 | 0 | 0 | 0 | 17.69 | 37.83 | 1.0 | 1.000017 | 1.500081 | 1.25 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/518 |
| 原安装速度 | speed-rpc-dds-udp-256-n1-r3 | 完成 | 48577 | 48577 | 48577 | 0 | 0 | 0 | 0 | 0 | 17.84 | 38.75 | 1.0 | 1.000007 | 1.500396 | 1.26 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/487 |
| 原安装速度 | speed-rpc-dds-udp-262144-n1-r1 | 完成 | 4179 | 4179 | 4179 | 0 | 0 | 0 | 0 | 0 | 234.55 | 463.85 | 1.0 | 1.000145 | 1.500381 | 1.03 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/43 |
| 原安装速度 | speed-rpc-dds-udp-262144-n1-r2 | 完成 | 4206 | 4206 | 4206 | 0 | 0 | 0 | 0 | 0 | 235.76 | 282.88 | 1.0 | 1.000141 | 1.500125 | 1.03 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/43 |
| 原安装速度 | speed-rpc-dds-udp-262144-n1-r3 | 完成 | 4206 | 4206 | 4206 | 0 | 0 | 0 | 0 | 0 | 236.16 | 270.99 | 1.0 | 1.000163 | 1.500434 | 1.05 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/43 |
| 原安装速度 | speed-rpc-dds-udp-32-n1-r1 | 完成 | 48767 | 48767 | 48767 | 0 | 0 | 0 | 0 | 0 | 17.57 | 39.97 | 1.0 | 1.000014 | 1.500646 | 1.28 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/488 |
| 原安装速度 | speed-rpc-dds-udp-32-n1-r2 | 完成 | 52185 | 52185 | 52185 | 0 | 0 | 0 | 0 | 0 | 17.55 | 35.51 | 1.0 | 1.000012 | 1.500118 | 1.25 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/523 |
| 原安装速度 | speed-rpc-dds-udp-32-n1-r3 | 完成 | 52085 | 52085 | 52085 | 0 | 0 | 0 | 0 | 0 | 17.64 | 36.56 | 1.0 | 1.000017 | 1.500082 | 1.26 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/522 |
| 原安装速度 | speed-rpc-dds-udp-32768-n1-r1 | 完成 | 27458 | 27458 | 27458 | 0 | 0 | 0 | 0 | 0 | 34.25 | 65.91 | 1.0 | 1.000107 | 1.500825 | 1.15 | 0.92 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/275 |
| 原安装速度 | speed-rpc-dds-udp-32768-n1-r2 | 完成 | 25555 | 25555 | 25555 | 0 | 0 | 0 | 0 | 0 | 36.68 | 68.51 | 1.0 | 1.000012 | 1.500725 | 1.13 | 0.88 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/256 |
| 原安装速度 | speed-rpc-dds-udp-32768-n1-r3 | 完成 | 25639 | 25639 | 25639 | 0 | 0 | 0 | 0 | 0 | 37.05 | 66.50 | 1.0 | 1.000006 | 1.500543 | 1.13 | 0.86 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/257 |
| 原安装速度 | speed-rpc-dds-udp-4096-n1-r1 | 完成 | 49807 | 49807 | 49807 | 0 | 0 | 0 | 0 | 0 | 18.48 | 37.45 | 1.0 | 1.000002 | 1.500030 | 1.24 | 0.63 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/499 |
| 原安装速度 | speed-rpc-dds-udp-4096-n1-r2 | 完成 | 45106 | 45106 | 45106 | 0 | 0 | 0 | 0 | 0 | 18.48 | 42.19 | 1.0 | 1.000007 | 1.500681 | 1.26 | 0.64 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/452 |
| 原安装速度 | speed-rpc-dds-udp-4096-n1-r3 | 完成 | 51445 | 51445 | 51445 | 0 | 0 | 0 | 0 | 0 | 18.64 | 34.51 | 1.0 | 1.000019 | 1.500435 | 1.24 | 0.61 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/515 |
| 原安装速度 | speed-rpc-dds-udp-512-n1-r1 | 完成 | 49642 | 49642 | 49642 | 0 | 0 | 0 | 0 | 0 | 17.88 | 37.08 | 1.0 | 1.000024 | 1.500581 | 1.26 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/497 |
| 原安装速度 | speed-rpc-dds-udp-512-n1-r2 | 完成 | 50140 | 50140 | 50140 | 0 | 0 | 0 | 0 | 0 | 17.83 | 39.37 | 1.0 | 1.000010 | 1.500803 | 1.26 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/502 |
| 原安装速度 | speed-rpc-dds-udp-512-n1-r3 | 完成 | 51068 | 51068 | 51068 | 0 | 0 | 0 | 0 | 0 | 17.58 | 39.92 | 1.0 | 1.000012 | 1.500666 | 1.26 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/512 |
| 原安装速度 | speed-rpc-dds-udp-524288-n1-r1 | 完成 | 2129 | 2129 | 2129 | 0 | 0 | 0 | 0 | 0 | 466.39 | 515.28 | 1.0 | 1.000508 | 1.500018 | 1.03 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-rpc-dds-udp-524288-n1-r2 | 完成 | 2096 | 2096 | 2096 | 0 | 0 | 0 | 0 | 0 | 472.45 | 561.20 | 1.0 | 1.000055 | 1.500795 | 1.02 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-rpc-dds-udp-524288-n1-r3 | 完成 | 2079 | 2079 | 2079 | 0 | 0 | 0 | 0 | 0 | 468.71 | 942.62 | 1.0 | 1.000120 | 1.500630 | 1.02 | 1.01 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/22 |
| 原安装速度 | speed-rpc-dds-udp-64-n1-r1 | 完成 | 50997 | 50997 | 50997 | 0 | 0 | 0 | 0 | 0 | 17.49 | 38.66 | 1.0 | 1.000009 | 1.500933 | 1.25 | 0.57 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/511 |
| 原安装速度 | speed-rpc-dds-udp-64-n1-r2 | 完成 | 52996 | 52996 | 52996 | 0 | 0 | 0 | 0 | 0 | 17.58 | 34.27 | 1.0 | 1.000019 | 1.500517 | 1.27 | 0.54 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/531 |
| 原安装速度 | speed-rpc-dds-udp-64-n1-r3 | 完成 | 47463 | 47463 | 47463 | 0 | 0 | 0 | 0 | 0 | 17.66 | 41.16 | 1.0 | 1.000009 | 1.500042 | 1.25 | 0.59 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/475 |
| 原安装速度 | speed-rpc-dds-udp-65536-n1-r1 | 完成 | 17115 | 17115 | 17115 | 0 | 0 | 0 | 0 | 0 | 56.85 | 85.54 | 1.0 | 1.000007 | 1.500376 | 1.16 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/172 |
| 原安装速度 | speed-rpc-dds-udp-65536-n1-r2 | 完成 | 16511 | 16511 | 16511 | 0 | 0 | 0 | 0 | 0 | 58.13 | 108.84 | 1.0 | 1.000055 | 1.500989 | 1.14 | 1.00 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/166 |
| 原安装速度 | speed-rpc-dds-udp-65536-n1-r3 | 完成 | 16711 | 16711 | 16711 | 0 | 0 | 0 | 0 | 0 | 57.71 | 99.80 | 1.0 | 1.000014 | 1.500772 | 1.14 | 0.99 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/168 |
| 原安装速度 | speed-rpc-dds-udp-8-n1-r1 | 完成 | 54018 | 54018 | 54018 | 0 | 0 | 0 | 0 | 0 | 17.52 | 33.68 | 1.0 | 1.000012 | 1.500821 | 1.25 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/541 |
| 原安装速度 | speed-rpc-dds-udp-8-n1-r2 | 完成 | 50910 | 50910 | 50910 | 0 | 0 | 0 | 0 | 0 | 17.85 | 38.81 | 1.0 | 1.000016 | 1.500031 | 1.25 | 0.55 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/510 |
| 原安装速度 | speed-rpc-dds-udp-8-n1-r3 | 完成 | 51911 | 51911 | 51911 | 0 | 0 | 0 | 0 | 0 | 17.79 | 36.20 | 1.0 | 1.000005 | 1.500237 | 1.25 | 0.56 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/520 |
| 原安装速度 | speed-rpc-dds-udp-8192-n1-r1 | 完成 | 46193 | 46193 | 46193 | 0 | 0 | 0 | 0 | 0 | 19.56 | 42.95 | 1.0 | 1.000010 | 1.500891 | 1.26 | 0.68 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/463 |
| 原安装速度 | speed-rpc-dds-udp-8192-n1-r2 | 完成 | 50729 | 50729 | 50729 | 0 | 0 | 0 | 0 | 0 | 18.71 | 35.00 | 1.0 | 1.000007 | 1.500559 | 1.24 | 0.69 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/508 |
| 原安装速度 | speed-rpc-dds-udp-8192-n1-r3 | 完成 | 49432 | 49432 | 49432 | 0 | 0 | 0 | 0 | 0 | 19.22 | 35.79 | 1.0 | 1.000008 | 1.501021 | 1.24 | 0.68 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/495 |
| 原安装速度 | speed-rpc-prebuilt-1024-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-1024-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-1024-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-1048576-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-1048576-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-1048576-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-128-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-128-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-128-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-131072-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-131072-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-131072-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16384-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16384-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-16384-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-2048-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-2048-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-2048-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-256-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-256-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-256-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-262144-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-262144-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-262144-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32768-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32768-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-32768-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-4096-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-4096-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-4096-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-512-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-512-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-512-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-524288-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-524288-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-524288-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-64-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-64-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-64-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-65536-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-65536-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-65536-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8192-n1-r1 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8192-n1-r2 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-prebuilt-8192-n1-r3 | 接口不支持 | — | — | — | — | — | — | — | — | — | — | 1.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装速度 | speed-rpc-shm-1024-n1-r1 | 有异常 | 7857 | 7857 | 7856 | 0 | 0 | 1 | 0 | 0 | 11.30 | 15.32 | 1.0 | 1.091080 | 1.500350 | 0.06 | 0.09 | 2/4 | 0/0/0/0 | 0/7857 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-1024-n1-r2 | 有异常 | 34298 | 34298 | 34297 | 0 | 0 | 1 | 0 | 0 | 5.50 | 10.60 | 1.0 | 1.226229 | 1.500976 | 0.21 | 0.25 | 2/4 | 0/0/0/0 | 0/34298 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-1024-n1-r3 | 完成 | 125904 | 125904 | 125904 | 0 | 0 | 0 | 0 | 0 | 8.19 | 13.62 | 1.0 | 1.000002 | 1.501017 | 0.80 | 0.97 | 2/4 | 0/0/0/0 | 0/125904 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-1048576-n1-r1 | 完成 | 6734 | 6734 | 6734 | 0 | 0 | 0 | 0 | 0 | 135.91 | 238.65 | 1.0 | 1.000044 | 1.501034 | 0.58 | 0.47 | 2/4 | 0/0/0/0 | 0/6734 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-1048576-n1-r2 | 完成 | 7311 | 7311 | 7311 | 0 | 0 | 0 | 0 | 0 | 134.24 | 184.67 | 1.0 | 1.000106 | 1.500675 | 0.63 | 0.43 | 2/4 | 0/0/0/0 | 0/7311 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-1048576-n1-r3 | 完成 | 6845 | 6845 | 6845 | 0 | 0 | 0 | 0 | 0 | 136.95 | 253.95 | 1.0 | 1.000018 | 1.500469 | 0.61 | 0.44 | 2/4 | 0/0/0/0 | 0/6845 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-128-n1-r1 | 有异常 | 45523 | 45523 | 45522 | 0 | 0 | 1 | 0 | 0 | 5.96 | 12.37 | 1.0 | 1.333317 | 1.500080 | 0.28 | 0.33 | 2/4 | 0/0/0/0 | 0/45523 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-128-n1-r2 | 有异常 | 11910 | 11910 | 11909 | 0 | 0 | 1 | 0 | 0 | 9.14 | 14.14 | 1.0 | 1.106955 | 1.500157 | 0.08 | 0.11 | 2/4 | 0/0/0/0 | 0/11910 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-128-n1-r3 | 有异常 | 2539 | 2539 | 2538 | 0 | 0 | 1 | 0 | 0 | 11.72 | 19.14 | 1.0 | 1.033163 | 1.500155 | 0.02 | 0.04 | 2/4 | 0/0/0/0 | 0/2539 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-131072-n1-r1 | 完成 | 36950 | 36950 | 36950 | 0 | 0 | 0 | 0 | 0 | 25.55 | 41.62 | 1.0 | 1.000021 | 1.500369 | 0.64 | 0.60 | 2/4 | 0/0/0/0 | 0/36950 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-131072-n1-r2 | 完成 | 38337 | 38337 | 38337 | 0 | 0 | 0 | 0 | 0 | 24.20 | 37.70 | 1.0 | 1.000001 | 1.500538 | 0.67 | 0.58 | 2/4 | 0/0/0/0 | 0/38337 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-131072-n1-r3 | 有异常 | 18007 | 18007 | 18006 | 0 | 0 | 1 | 0 | 0 | 26.18 | 36.67 | 1.0 | 1.486570 | 1.500032 | 0.30 | 0.31 | 2/4 | 0/0/0/0 | 0/18007 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16-n1-r1 | 有异常 | 17865 | 17865 | 17864 | 0 | 0 | 1 | 0 | 0 | 8.68 | 12.84 | 1.0 | 1.163077 | 1.500656 | 0.12 | 0.16 | 2/4 | 0/0/0/0 | 0/17865 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16-n1-r2 | 有异常 | 95043 | 95043 | 95042 | 0 | 0 | 1 | 0 | 0 | 4.59 | 10.28 | 1.0 | 1.489009 | 1.500269 | 0.47 | 0.51 | 2/4 | 0/0/0/0 | 0/95043 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16-n1-r3 | 有异常 | 48 | 48 | 47 | 0 | 0 | 1 | 0 | 0 | 8.44 | 42.27 | 1.0 | 1.000861 | 1.500855 | 0.00 | 0.01 | 2/4 | 0/0/0/0 | 0/48 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16384-n1-r1 | 有异常 | 67542 | 67542 | 67541 | 0 | 0 | 1 | 0 | 0 | 9.87 | 15.46 | 1.0 | 1.737972 | 1.738278 | 0.58 | 0.59 | 2/4 | 0/0/0/0 | 0/67542 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16384-n1-r2 | 有异常 | 29831 | 29831 | 29830 | 0 | 0 | 1 | 0 | 0 | 9.58 | 15.21 | 1.0 | 1.309096 | 1.500733 | 0.25 | 0.25 | 2/4 | 0/0/0/0 | 0/29831 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-16384-n1-r3 | 有异常 | 76115 | 76115 | 76114 | 0 | 0 | 1 | 0 | 0 | 9.87 | 15.58 | 1.0 | 1.809177 | 1.809280 | 0.65 | 0.64 | 2/4 | 0/0/0/0 | 0/76115 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-2048-n1-r1 | 有异常 | 45966 | 45966 | 45965 | 0 | 0 | 1 | 0 | 0 | 5.73 | 12.85 | 1.0 | 1.343592 | 1.500869 | 0.29 | 0.35 | 2/4 | 0/0/0/0 | 0/45966 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-2048-n1-r2 | 有异常 | 53046 | 53046 | 53045 | 0 | 0 | 1 | 0 | 0 | 9.63 | 14.26 | 1.0 | 1.486331 | 1.500859 | 0.37 | 0.46 | 2/4 | 0/0/0/0 | 0/53046 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-2048-n1-r3 | 有异常 | 68750 | 68750 | 68749 | 0 | 0 | 1 | 0 | 0 | 5.44 | 10.87 | 1.0 | 1.487331 | 1.500874 | 0.43 | 0.49 | 2/4 | 0/0/0/0 | 0/68750 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-256-n1-r1 | 有异常 | 96427 | 96427 | 96426 | 0 | 0 | 1 | 0 | 0 | 5.17 | 10.06 | 1.0 | 1.568371 | 1.568762 | 0.54 | 0.61 | 2/4 | 0/0/0/0 | 0/96427 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-256-n1-r2 | 有异常 | 115365 | 115365 | 115364 | 0 | 0 | 1 | 0 | 0 | 5.03 | 9.42 | 1.0 | 1.613797 | 1.614357 | 0.60 | 0.66 | 2/4 | 0/0/0/0 | 0/115365 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-256-n1-r3 | 有异常 | 27798 | 27798 | 27797 | 0 | 0 | 1 | 0 | 0 | 4.97 | 10.25 | 1.0 | 1.170687 | 1.500262 | 0.15 | 0.18 | 2/4 | 0/0/0/0 | 0/27798 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-262144-n1-r1 | 完成 | 23839 | 23839 | 23839 | 0 | 0 | 0 | 0 | 0 | 37.57 | 64.70 | 1.0 | 1.000012 | 1.500493 | 0.63 | 0.53 | 2/4 | 0/0/0/0 | 0/23839 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-262144-n1-r2 | 完成 | 24365 | 24365 | 24365 | 0 | 0 | 0 | 0 | 0 | 37.87 | 69.12 | 1.0 | 1.000030 | 1.500953 | 0.58 | 0.58 | 2/4 | 0/0/0/0 | 0/24365 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-262144-n1-r3 | 完成 | 24604 | 24604 | 24604 | 0 | 0 | 0 | 0 | 0 | 37.40 | 68.70 | 1.0 | 1.000011 | 1.500216 | 0.60 | 0.57 | 2/4 | 0/0/0/0 | 0/24604 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32-n1-r1 | 有异常 | 6091 | 6091 | 6090 | 0 | 0 | 1 | 0 | 0 | 9.97 | 12.45 | 1.0 | 1.058278 | 1.500911 | 0.04 | 0.06 | 2/4 | 0/0/0/0 | 0/6091 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32-n1-r2 | 完成 | 154082 | 154082 | 154082 | 0 | 0 | 0 | 0 | 0 | 4.97 | 11.27 | 1.0 | 1.000006 | 1.500638 | 0.87 | 0.99 | 2/4 | 0/0/0/0 | 0/154082 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32-n1-r3 | 有异常 | 36733 | 36733 | 36732 | 0 | 0 | 1 | 0 | 0 | 4.97 | 10.04 | 1.0 | 1.239214 | 1.500969 | 0.21 | 0.24 | 2/4 | 0/0/0/0 | 0/36733 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32768-n1-r1 | 有异常 | 21445 | 21445 | 21444 | 0 | 0 | 1 | 0 | 0 | 14.78 | 20.82 | 1.0 | 1.319554 | 1.500737 | 0.21 | 0.25 | 2/4 | 0/0/0/0 | 0/21445 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32768-n1-r2 | 有异常 | 66185 | 66185 | 66184 | 0 | 0 | 1 | 0 | 0 | 12.03 | 17.74 | 1.0 | 1.852894 | 1.853325 | 0.64 | 0.65 | 2/4 | 0/0/0/0 | 0/66185 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-32768-n1-r3 | 有异常 | 46630 | 46630 | 46629 | 0 | 0 | 1 | 0 | 0 | 13.88 | 20.26 | 1.0 | 1.647336 | 1.648533 | 0.46 | 0.50 | 2/4 | 0/0/0/0 | 0/46630 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-4096-n1-r1 | 有异常 | 34111 | 34111 | 34110 | 0 | 0 | 1 | 0 | 0 | 10.19 | 16.16 | 1.0 | 1.320584 | 1.500852 | 0.26 | 0.30 | 2/4 | 0/0/0/0 | 0/34111 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-4096-n1-r2 | 完成 | 120450 | 120450 | 120450 | 0 | 0 | 0 | 0 | 0 | 6.64 | 13.56 | 1.0 | 1.000012 | 1.500584 | 0.82 | 0.95 | 2/4 | 0/0/0/0 | 0/120450 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-4096-n1-r3 | 有异常 | 15229 | 15229 | 15228 | 0 | 0 | 1 | 0 | 0 | 10.42 | 17.01 | 1.0 | 1.172217 | 1.500410 | 0.13 | 0.16 | 2/4 | 0/0/0/0 | 0/15229 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-512-n1-r1 | 有异常 | 177316 | 177316 | 177315 | 0 | 0 | 1 | 0 | 0 | 5.01 | 9.31 | 1.0 | 1.925667 | 1.926498 | 0.92 | 1.00 | 2/4 | 0/0/0/0 | 0/177316 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-512-n1-r2 | 有异常 | 11662 | 11662 | 11661 | 0 | 0 | 1 | 0 | 0 | 6.22 | 13.10 | 1.0 | 1.090383 | 1.500053 | 0.07 | 0.10 | 2/4 | 0/0/0/0 | 0/11662 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-512-n1-r3 | 有异常 | 3907 | 3907 | 3906 | 0 | 0 | 1 | 0 | 0 | 6.29 | 12.79 | 1.0 | 1.030516 | 1.500213 | 0.03 | 0.04 | 2/4 | 0/0/0/0 | 0/3907 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-524288-n1-r1 | 完成 | 13582 | 13582 | 13582 | 0 | 0 | 0 | 0 | 0 | 66.66 | 126.39 | 1.0 | 1.000083 | 1.501052 | 0.57 | 0.53 | 2/4 | 0/0/0/0 | 0/13582 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-524288-n1-r2 | 完成 | 15427 | 15427 | 15427 | 0 | 0 | 0 | 0 | 0 | 62.97 | 93.61 | 1.0 | 1.000062 | 1.500788 | 0.60 | 0.51 | 2/4 | 0/0/0/0 | 0/15427 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-524288-n1-r3 | 完成 | 15299 | 15299 | 15299 | 0 | 0 | 0 | 0 | 0 | 62.91 | 96.11 | 1.0 | 1.000020 | 1.500454 | 0.59 | 0.52 | 2/4 | 0/0/0/0 | 0/15299 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-64-n1-r1 | 有异常 | 122526 | 122526 | 122525 | 0 | 0 | 1 | 0 | 0 | 5.14 | 11.76 | 1.0 | 1.777814 | 1.777900 | 0.71 | 0.80 | 2/4 | 0/0/0/0 | 0/122526 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-64-n1-r2 | 有异常 | 8102 | 8102 | 8101 | 0 | 0 | 1 | 0 | 0 | 9.89 | 16.32 | 1.0 | 1.085451 | 1.500957 | 0.08 | 0.09 | 2/4 | 0/0/0/0 | 0/8102 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-64-n1-r3 | 有异常 | 44700 | 44700 | 44699 | 0 | 0 | 1 | 0 | 0 | 7.65 | 12.71 | 1.0 | 1.337577 | 1.500364 | 0.28 | 0.34 | 2/4 | 0/0/0/0 | 0/44700 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-65536-n1-r1 | 完成 | 55734 | 55734 | 55734 | 0 | 0 | 0 | 0 | 0 | 16.65 | 27.89 | 1.0 | 1.000014 | 1.500284 | 0.68 | 0.68 | 2/4 | 0/0/0/0 | 0/55734 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-65536-n1-r2 | 有异常 | 49864 | 49864 | 49863 | 0 | 0 | 1 | 0 | 0 | 16.55 | 26.26 | 1.0 | 1.891636 | 1.892383 | 0.65 | 0.60 | 2/4 | 0/0/0/0 | 0/49864 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-65536-n1-r3 | 有异常 | 31009 | 31009 | 31008 | 0 | 0 | 1 | 0 | 0 | 18.58 | 30.06 | 1.0 | 1.601986 | 1.602237 | 0.40 | 0.42 | 2/4 | 0/0/0/0 | 0/31009 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8-n1-r1 | 有异常 | 62450 | 62450 | 62449 | 0 | 0 | 1 | 0 | 0 | 7.36 | 10.86 | 1.0 | 1.452570 | 1.500538 | 0.39 | 0.43 | 2/4 | 0/0/0/0 | 0/62450 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8-n1-r2 | 完成 | 137655 | 137655 | 137655 | 0 | 0 | 0 | 0 | 0 | 5.71 | 12.19 | 1.0 | 1.000000 | 1.500986 | 0.80 | 0.96 | 2/4 | 0/0/0/0 | 0/137655 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8-n1-r3 | 有异常 | 112886 | 112886 | 112885 | 0 | 0 | 1 | 0 | 0 | 4.67 | 10.81 | 1.0 | 1.588102 | 1.589099 | 0.56 | 0.61 | 2/4 | 0/0/0/0 | 0/112886 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8192-n1-r1 | 有异常 | 3390 | 3390 | 3389 | 0 | 0 | 1 | 0 | 0 | 10.63 | 16.58 | 1.0 | 1.039795 | 1.500104 | 0.03 | 0.04 | 2/4 | 0/0/0/0 | 0/3390 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8192-n1-r2 | 有异常 | 69687 | 69687 | 69686 | 0 | 0 | 1 | 0 | 0 | 8.89 | 16.79 | 1.0 | 1.656751 | 1.657403 | 0.58 | 0.57 | 2/4 | 0/0/0/0 | 0/69687 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-shm-8192-n1-r3 | 完成 | 108109 | 108109 | 108109 | 0 | 0 | 0 | 0 | 0 | 8.67 | 13.26 | 1.0 | 1.000009 | 1.500518 | 0.80 | 0.87 | 2/4 | 0/0/0/0 | 0/108109 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1024-n1-r1 | 完成 | 13255 | 13255 | 13255 | 0 | 0 | 0 | 0 | 0 | 64.14 | 162.15 | 1.0 | 1.000001 | 1.500520 | 1.52 | 2.05 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1024-n1-r2 | 完成 | 13085 | 13085 | 13085 | 0 | 0 | 0 | 0 | 0 | 63.27 | 157.63 | 1.0 | 1.000016 | 1.500389 | 1.51 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1024-n1-r3 | 完成 | 13096 | 13096 | 13096 | 0 | 0 | 0 | 0 | 0 | 66.54 | 169.12 | 1.0 | 1.000030 | 1.500441 | 1.49 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1048576-n1-r1 | 完成 | 74 | 74 | 74 | 0 | 0 | 0 | 0 | 0 | 12,402.65 | 21,586.71 | 1.0 | 1.012058 | 1.500624 | 1.39 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1048576-n1-r2 | 完成 | 61 | 61 | 61 | 0 | 0 | 0 | 0 | 0 | 15,451.61 | 23,103.55 | 1.0 | 1.012287 | 1.500038 | 1.44 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-1048576-n1-r3 | 完成 | 57 | 57 | 57 | 0 | 0 | 0 | 0 | 0 | 17,341.08 | 22,509.64 | 1.0 | 1.006880 | 1.500372 | 1.41 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-128-n1-r1 | 完成 | 12387 | 12387 | 12387 | 0 | 0 | 0 | 0 | 0 | 67.56 | 175.59 | 1.0 | 1.000058 | 1.500363 | 1.51 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-128-n1-r2 | 完成 | 13026 | 13026 | 13026 | 0 | 0 | 0 | 0 | 0 | 65.83 | 169.33 | 1.0 | 1.000014 | 1.500488 | 1.49 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-128-n1-r3 | 完成 | 14491 | 14491 | 14491 | 0 | 0 | 0 | 0 | 0 | 62.38 | 155.92 | 1.0 | 1.000029 | 1.500499 | 1.50 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-131072-n1-r1 | 完成 | 363 | 363 | 363 | 0 | 0 | 0 | 0 | 0 | 2,798.09 | 3,333.10 | 1.0 | 1.000756 | 1.500409 | 1.38 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-131072-n1-r2 | 完成 | 373 | 373 | 373 | 0 | 0 | 0 | 0 | 0 | 2,810.88 | 3,466.07 | 1.0 | 1.002351 | 1.500255 | 1.36 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-131072-n1-r3 | 完成 | 362 | 362 | 362 | 0 | 0 | 0 | 0 | 0 | 2,798.73 | 3,251.36 | 1.0 | 1.001059 | 1.500997 | 1.38 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16-n1-r1 | 完成 | 12668 | 12668 | 12668 | 0 | 0 | 0 | 0 | 0 | 65.62 | 173.73 | 1.0 | 1.000062 | 1.501014 | 1.51 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16-n1-r2 | 完成 | 14489 | 14489 | 14489 | 0 | 0 | 0 | 0 | 0 | 61.10 | 148.44 | 1.0 | 1.000011 | 1.500077 | 1.49 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16-n1-r3 | 完成 | 14138 | 14138 | 14138 | 0 | 0 | 0 | 0 | 0 | 60.53 | 159.80 | 1.0 | 1.000038 | 1.500304 | 1.52 | 2.04 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16384-n1-r1 | 完成 | 2819 | 2819 | 2819 | 0 | 0 | 0 | 0 | 0 | 362.03 | 704.19 | 1.0 | 1.002302 | 1.500797 | 1.43 | 2.28 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16384-n1-r2 | 完成 | 2360 | 2360 | 2360 | 0 | 0 | 0 | 0 | 0 | 435.29 | 643.10 | 1.0 | 1.000029 | 1.500892 | 1.41 | 2.25 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-16384-n1-r3 | 完成 | 3264 | 3264 | 3264 | 0 | 0 | 0 | 0 | 0 | 244.04 | 672.84 | 1.0 | 1.000214 | 1.500746 | 1.43 | 2.28 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-2048-n1-r1 | 完成 | 9725 | 9725 | 9725 | 0 | 0 | 0 | 0 | 0 | 87.16 | 222.30 | 1.0 | 1.000152 | 1.500464 | 1.48 | 2.10 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-2048-n1-r2 | 完成 | 9534 | 9534 | 9534 | 0 | 0 | 0 | 0 | 0 | 86.24 | 223.17 | 1.0 | 1.000017 | 1.500743 | 1.47 | 2.13 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-2048-n1-r3 | 完成 | 11166 | 11166 | 11166 | 0 | 0 | 0 | 0 | 0 | 75.98 | 198.62 | 1.0 | 1.000050 | 1.500702 | 1.47 | 2.12 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-256-n1-r1 | 完成 | 14231 | 14231 | 14231 | 0 | 0 | 0 | 0 | 0 | 63.09 | 151.06 | 1.0 | 1.000037 | 1.500676 | 1.47 | 2.10 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-256-n1-r2 | 完成 | 14215 | 14215 | 14215 | 0 | 0 | 0 | 0 | 0 | 63.01 | 175.43 | 1.0 | 1.000053 | 1.500820 | 1.51 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-256-n1-r3 | 完成 | 11936 | 11936 | 11936 | 0 | 0 | 0 | 0 | 0 | 71.12 | 174.65 | 1.0 | 1.000029 | 1.500049 | 1.49 | 2.08 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-262144-n1-r1 | 完成 | 210 | 210 | 210 | 0 | 0 | 0 | 0 | 0 | 5,189.87 | 5,921.59 | 1.0 | 1.003534 | 1.500386 | 1.40 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-262144-n1-r2 | 完成 | 202 | 202 | 202 | 0 | 0 | 0 | 0 | 0 | 5,100.24 | 6,049.47 | 1.0 | 1.000124 | 1.500344 | 1.40 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-262144-n1-r3 | 完成 | 257 | 257 | 257 | 0 | 0 | 0 | 0 | 0 | 3,407.54 | 5,784.31 | 1.0 | 1.001248 | 1.500534 | 1.38 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32-n1-r1 | 完成 | 14253 | 14253 | 14253 | 0 | 0 | 0 | 0 | 0 | 61.19 | 154.41 | 1.0 | 1.000021 | 1.500283 | 1.49 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32-n1-r2 | 完成 | 13810 | 13810 | 13810 | 0 | 0 | 0 | 0 | 0 | 62.28 | 159.54 | 1.0 | 1.000045 | 1.500639 | 1.50 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32-n1-r3 | 完成 | 11917 | 11917 | 11917 | 0 | 0 | 0 | 0 | 0 | 75.86 | 178.99 | 1.0 | 1.000019 | 1.500930 | 1.49 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32768-n1-r1 | 完成 | 1428 | 1428 | 1428 | 0 | 0 | 0 | 0 | 0 | 736.39 | 1,086.77 | 1.0 | 1.000396 | 1.500841 | 1.44 | 2.32 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32768-n1-r2 | 完成 | 1822 | 1822 | 1822 | 0 | 0 | 0 | 0 | 0 | 444.42 | 981.67 | 1.0 | 1.000561 | 1.500069 | 1.45 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-32768-n1-r3 | 完成 | 1599 | 1599 | 1599 | 0 | 0 | 0 | 0 | 0 | 708.57 | 977.92 | 1.0 | 1.000728 | 1.500566 | 1.41 | 2.33 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-4096-n1-r1 | 完成 | 8652 | 8652 | 8652 | 0 | 0 | 0 | 0 | 0 | 101.10 | 268.10 | 1.0 | 1.000025 | 1.500267 | 1.44 | 2.15 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-4096-n1-r2 | 完成 | 6936 | 6936 | 6936 | 0 | 0 | 0 | 0 | 0 | 156.42 | 247.98 | 1.0 | 1.000021 | 1.500290 | 1.46 | 2.16 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-4096-n1-r3 | 完成 | 6733 | 6733 | 6733 | 0 | 0 | 0 | 0 | 0 | 162.72 | 282.36 | 1.0 | 1.000100 | 1.500231 | 1.47 | 2.15 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-512-n1-r1 | 完成 | 14146 | 14146 | 14146 | 0 | 0 | 0 | 0 | 0 | 62.13 | 150.41 | 1.0 | 1.000060 | 1.500717 | 1.49 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-512-n1-r2 | 完成 | 11501 | 11501 | 11501 | 0 | 0 | 0 | 0 | 0 | 81.28 | 197.84 | 1.0 | 1.000009 | 1.500142 | 1.49 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-512-n1-r3 | 完成 | 14255 | 14255 | 14255 | 0 | 0 | 0 | 0 | 0 | 63.55 | 150.04 | 1.0 | 1.000040 | 1.500606 | 1.47 | 2.09 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-524288-n1-r1 | 完成 | 105 | 105 | 105 | 0 | 0 | 0 | 0 | 0 | 10,149.16 | 11,328.94 | 1.0 | 1.006003 | 1.500742 | 1.40 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-524288-n1-r2 | 完成 | 145 | 145 | 145 | 0 | 0 | 0 | 0 | 0 | 6,120.69 | 11,480.53 | 1.0 | 1.006312 | 1.500971 | 1.39 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-524288-n1-r3 | 完成 | 101 | 101 | 101 | 0 | 0 | 0 | 0 | 0 | 10,822.79 | 11,641.00 | 1.0 | 1.009487 | 1.500094 | 1.38 | 2.36 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-64-n1-r1 | 完成 | 13965 | 13965 | 13965 | 0 | 0 | 0 | 0 | 0 | 63.95 | 148.88 | 1.0 | 1.000044 | 1.501014 | 1.50 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-64-n1-r2 | 完成 | 14220 | 14220 | 14220 | 0 | 0 | 0 | 0 | 0 | 57.36 | 157.75 | 1.0 | 1.000045 | 1.500361 | 1.53 | 2.03 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-64-n1-r3 | 完成 | 13309 | 13309 | 13309 | 0 | 0 | 0 | 0 | 0 | 62.58 | 171.11 | 1.0 | 1.000059 | 1.500981 | 1.51 | 2.04 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-65536-n1-r1 | 完成 | 715 | 715 | 715 | 0 | 0 | 0 | 0 | 0 | 1,403.52 | 1,681.96 | 1.0 | 1.000006 | 1.500087 | 1.40 | 2.35 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-65536-n1-r2 | 完成 | 779 | 779 | 779 | 0 | 0 | 0 | 0 | 0 | 1,394.16 | 1,853.92 | 1.0 | 1.000120 | 1.500419 | 1.40 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-65536-n1-r3 | 完成 | 713 | 713 | 713 | 0 | 0 | 0 | 0 | 0 | 1,394.67 | 1,804.57 | 1.0 | 1.000969 | 1.500065 | 1.41 | 2.34 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8-n1-r1 | 完成 | 12183 | 12183 | 12183 | 0 | 0 | 0 | 0 | 0 | 68.03 | 174.97 | 1.0 | 1.000019 | 1.500158 | 1.50 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8-n1-r2 | 完成 | 11948 | 11948 | 11948 | 0 | 0 | 0 | 0 | 0 | 68.83 | 175.83 | 1.0 | 1.000044 | 1.500232 | 1.50 | 2.07 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8-n1-r3 | 完成 | 13983 | 13983 | 13983 | 0 | 0 | 0 | 0 | 0 | 62.68 | 164.24 | 1.0 | 1.000024 | 1.500675 | 1.50 | 2.06 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8192-n1-r1 | 完成 | 4937 | 4937 | 4937 | 0 | 0 | 0 | 0 | 0 | 172.78 | 372.53 | 1.0 | 1.000118 | 1.500610 | 1.43 | 2.23 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8192-n1-r2 | 完成 | 5304 | 5304 | 5304 | 0 | 0 | 0 | 0 | 0 | 154.11 | 369.13 | 1.0 | 1.000001 | 1.501019 | 1.42 | 2.23 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |
| 原安装速度 | speed-rpc-socket-8192-n1-r3 | 完成 | 4877 | 4877 | 4877 | 0 | 0 | 0 | 0 | 0 | 190.00 | 391.40 | 1.0 | 1.000038 | 1.500122 | 1.43 | 2.23 | 2/4 | 0/0/0/0 | 0/0 | 0/0 | 0/0/0 |

| 配置 | case id | 状态 | 计划 | 尝试 | 发/请求成功 | 收 | 窗内收 | 发失败 | 坏样本 | 重复 | p50µs | p99µs | 计划窗 s | 发实耗 s | 收观测 s | 发CPU s | 收CPU s | 发/收线程 | TLV/A/B/Prebuilt | DZFlat/回退 | 请求/响应view | DDS发SHM/收SHM/收堆 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 原安装压力 | stress-stress-dds-iox-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 7.73 | 84.79 | 10.0 | 9.999026 | 10.500682 | 0.51 | 0.16 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 原安装压力 | stress-stress-dds-iox-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.06 | 25.56 | 10.0 | 9.999023 | 10.500680 | 0.94 | 0.13 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 原安装压力 | stress-stress-dds-iox-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.24 | 49.64 | 10.0 | 9.999120 | 10.500721 | 0.90 | 0.16 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 原安装压力 | stress-stress-dds-iox-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 28.55 | 117.05 | 10.0 | 9.999205 | 10.500079 | 5.03 | 1.59 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 原安装压力 | stress-stress-dds-iox-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 16.74 | 120.74 | 10.0 | 9.999127 | 10.500421 | 4.49 | 1.47 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 原安装压力 | stress-stress-dds-iox-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 10.27 | 115.14 | 10.0 | 9.999132 | 10.500582 | 4.96 | 1.34 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r1 | 失败 | — | — | — | — | — | — | — | — | — | — | 10.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r2 | 失败 | — | — | — | — | — | — | — | — | — | — | 10.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r3 | 失败 | — | — | — | — | — | — | — | — | — | — | 10.0 | — | — | — | — | —/— | —/—/—/— | —/— | —/— | —/—/— |
| 原安装压力 | stress-stress-dds-udp-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 21.99 | 99.25 | 10.0 | 9.999115 | 10.500690 | 0.81 | 0.20 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 原安装压力 | stress-stress-dds-udp-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 14.95 | 71.32 | 10.0 | 9.999113 | 10.500043 | 1.08 | 0.19 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 原安装压力 | stress-stress-dds-udp-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 21.58 | 102.14 | 10.0 | 9.999130 | 10.500419 | 0.63 | 0.16 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 原安装压力 | stress-stress-dds-udp-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 43.01 | 1,674.45 | 10.0 | 9.999568 | 10.501057 | 23.27 | 6.31 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 原安装压力 | stress-stress-dds-udp-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 999689 | 0 | 0 | 0 | 51.30 | 7,100.45 | 10.0 | 9.999717 | 10.500193 | 25.21 | 7.40 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 原安装压力 | stress-stress-dds-udp-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 37.65 | 1,457.52 | 10.0 | 9.999603 | 10.500592 | 23.50 | 6.10 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 原安装压力 | stress-stress-dds-udp-64-n1000-r1 | 完成 | 10000000 | 1787450 | 1787450 | 1787450 | 1653803 | 0 | 0 | 0 | 441,525.24 | 742,844.54 | 10.0 | 10.000041 | 10.501108 | 40.06 | 10.51 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/18500 |
| 原安装压力 | stress-stress-dds-udp-64-n1000-r2 | 完成 | 10000000 | 1638022 | 1638022 | 1638022 | 1509110 | 0 | 0 | 0 | 453,078.95 | 789,582.31 | 10.0 | 10.000043 | 10.501074 | 40.07 | 10.54 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/17000 |
| 原安装压力 | stress-stress-dds-udp-64-n1000-r3 | 完成 | 10000000 | 1766417 | 1766417 | 1766417 | 1634355 | 0 | 0 | 0 | 483,224.83 | 742,595.39 | 10.0 | 10.000040 | 10.500536 | 40.06 | 10.52 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/18250 |
| 原安装压力 | stress-stress-shm-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.00 | 129.91 | 10.0 | 9.999022 | 10.500000 | 0.80 | 10.62 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 11.19 | 89.55 | 10.0 | 9.999254 | 10.500000 | 0.54 | 10.61 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.92 | 36.92 | 10.0 | 9.999030 | 10.500000 | 0.98 | 10.63 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 9.14 | 49.45 | 10.0 | 9.999197 | 10.500001 | 5.05 | 19.24 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 9.01 | 30.36 | 10.0 | 9.999196 | 10.500001 | 6.56 | 18.57 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 9.31 | 57.39 | 10.0 | 9.999171 | 10.500000 | 5.61 | 20.18 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n1000-r1 | 完成 | 10000000 | 10000000 | 10000000 | 9999998 | 9999998 | 0 | 0 | 0 | 23.94 | 83.99 | 10.0 | 9.999826 | 10.500012 | 34.98 | 116.84 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n1000-r2 | 完成 | 10000000 | 10000000 | 10000000 | 9999994 | 9999994 | 0 | 0 | 0 | 22.21 | 78.62 | 10.0 | 9.999937 | 10.500004 | 35.36 | 118.54 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |
| 原安装压力 | stress-stress-shm-64-n1000-r3 | 完成 | 10000000 | 10000000 | 10000000 | 10000000 | 10000000 | 0 | 0 | 0 | 21.79 | 80.56 | 10.0 | 9.999898 | 10.500002 | 34.77 | 116.17 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |

| 配置 | case id | 状态 | 计划 | 尝试 | 发/请求成功 | 收 | 窗内收 | 发失败 | 坏样本 | 重复 | p50µs | p99µs | 计划窗 s | 发实耗 s | 收观测 s | 发CPU s | 收CPU s | 发/收线程 | TLV/A/B/Prebuilt | DZFlat/回退 | 请求/响应view | DDS发SHM/收SHM/收堆 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 扩容压力 | stress-stress-dds-iox-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 10.70 | 38.69 | 10.0 | 9.999021 | 10.500877 | 0.78 | 0.18 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 扩容压力 | stress-stress-dds-iox-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 10.43 | 56.55 | 10.0 | 9.999026 | 10.500098 | 0.94 | 0.19 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 扩容压力 | stress-stress-dds-iox-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 15.37 | 46.23 | 10.0 | 9.999082 | 10.500521 | 0.62 | 0.24 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101/0/101 |
| 扩容压力 | stress-stress-dds-iox-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 33.17 | 153.29 | 10.0 | 9.999118 | 10.500144 | 5.25 | 1.66 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 扩容压力 | stress-stress-dds-iox-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 25.39 | 198.46 | 10.0 | 9.999143 | 10.500768 | 4.49 | 1.39 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 扩容压力 | stress-stress-dds-iox-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 43.79 | 131.90 | 10.0 | 9.999144 | 10.500878 | 4.74 | 1.78 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 10100/0/10100 |
| 扩容压力 | stress-stress-dds-iox-64-n1000-r1 | 完成 | 10000000 | 10000000 | 10000000 | 10000000 | 10000000 | 0 | 0 | 0 | 132.48 | 7,715.63 | 10.0 | 9.999767 | 10.501127 | 23.88 | 8.79 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101000/0/101000 |
| 扩容压力 | stress-stress-dds-iox-64-n1000-r2 | 完成 | 10000000 | 10000000 | 10000000 | 10000000 | 10000000 | 0 | 0 | 0 | 110.13 | 315.43 | 10.0 | 9.999669 | 10.500863 | 23.93 | 8.61 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101000/0/101000 |
| 扩容压力 | stress-stress-dds-iox-64-n1000-r3 | 完成 | 10000000 | 10000000 | 10000000 | 10000000 | 10000000 | 0 | 0 | 0 | 118.60 | 1,149.21 | 10.0 | 9.999638 | 10.501098 | 23.90 | 8.61 | 10/10 | 0/0/0/0 | 0/0 | 0/0 | 101000/0/101000 |
| 扩容压力 | stress-stress-dds-udp-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 23.97 | 70.95 | 10.0 | 9.999060 | 10.500594 | 1.08 | 0.17 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 扩容压力 | stress-stress-dds-udp-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 22.45 | 103.71 | 10.0 | 9.999133 | 10.500605 | 0.61 | 0.16 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 扩容压力 | stress-stress-dds-udp-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 21.99 | 73.08 | 10.0 | 9.999120 | 10.500786 | 1.11 | 0.16 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/101 |
| 扩容压力 | stress-stress-dds-udp-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 37.43 | 1,886.53 | 10.0 | 9.999628 | 10.500299 | 22.87 | 5.70 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 扩容压力 | stress-stress-dds-udp-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 36.26 | 1,038.00 | 10.0 | 9.999661 | 10.501020 | 23.13 | 5.88 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 扩容压力 | stress-stress-dds-udp-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 999971 | 0 | 0 | 0 | 36.80 | 452.80 | 10.0 | 9.999664 | 10.500702 | 22.88 | 5.85 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/10100 |
| 扩容压力 | stress-stress-dds-udp-64-n1000-r1 | 完成 | 10000000 | 1751310 | 1751310 | 1751310 | 1608932 | 0 | 0 | 0 | 431,905.58 | 798,550.46 | 10.0 | 10.000050 | 10.501046 | 40.05 | 10.54 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/18250 |
| 扩容压力 | stress-stress-dds-udp-64-n1000-r2 | 完成 | 10000000 | 1747137 | 1747137 | 1747137 | 1585619 | 0 | 0 | 0 | 550,073.08 | 920,522.35 | 10.0 | 10.000052 | 10.547113 | 40.06 | 10.60 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/18000 |
| 扩容压力 | stress-stress-dds-udp-64-n1000-r3 | 完成 | 10000000 | 1801064 | 1801064 | 1801064 | 1608815 | 0 | 0 | 0 | 591,261.47 | 1,046,708.95 | 10.0 | 10.000045 | 10.652014 | 40.06 | 10.70 | 8/8 | 0/0/0/0 | 0/0 | 0/0 | 0/0/18500 |
| 扩容压力 | stress-stress-shm-64-n1-r1 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.85 | 85.21 | 10.0 | 9.999153 | 10.500000 | 0.99 | 10.62 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n1-r2 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 9.86 | 72.96 | 10.0 | 9.999199 | 10.500000 | 0.56 | 10.61 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n1-r3 | 完成 | 10000 | 10000 | 10000 | 10000 | 10000 | 0 | 0 | 0 | 8.16 | 48.04 | 10.0 | 9.999025 | 10.500000 | 0.86 | 10.65 | 2/3 | 10000/0/0/0 | 0/10000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n100-r1 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 8.77 | 43.62 | 10.0 | 9.999131 | 10.500001 | 6.10 | 19.41 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n100-r2 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 8.93 | 33.09 | 10.0 | 9.999163 | 10.500001 | 5.21 | 18.94 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n100-r3 | 完成 | 1000000 | 1000000 | 1000000 | 1000000 | 1000000 | 0 | 0 | 0 | 9.28 | 53.11 | 10.0 | 9.999192 | 10.500000 | 6.00 | 19.73 | 2/34 | 1000000/0/0/0 | 0/1000000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n1000-r1 | 完成 | 10000000 | 10000000 | 10000000 | 9999998 | 9999998 | 0 | 0 | 0 | 21.80 | 80.62 | 10.0 | 9.999873 | 10.500001 | 35.03 | 117.02 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n1000-r2 | 完成 | 10000000 | 10000000 | 10000000 | 9999996 | 9999996 | 0 | 0 | 0 | 21.23 | 84.83 | 10.0 | 9.999966 | 10.500010 | 34.55 | 118.34 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |
| 扩容压力 | stress-stress-shm-64-n1000-r3 | 完成 | 10000000 | 10000000 | 10000000 | 9999994 | 9999977 | 0 | 0 | 0 | 21.74 | 112.70 | 10.0 | 9.999913 | 10.500005 | 34.90 | 117.98 | 2/34 | 10000000/0/0/0 | 0/10000000 | 0/0 | 0/0/0 |


## 异常记录

| 配置 | case id | 原因 |
|---|---|---|
| 原安装速度 | speed-pubsub-b-262144-n1-r2 | 速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-b-32768-n1-r2 | 速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-prebuilt-32768-n1-r1 | 速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-prebuilt-8192-n1-r1 | 速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-1048576-n1-r2 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-1048576-n1-r3 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-131072-n1-r1 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-131072-n1-r3 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r1 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r2 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-262144-n1-r3 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r1 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r2 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-pubsub-socket-524288-n1-r3 | 载荷错误或重复；速度测试发送接收数不守恒 |
| 原安装速度 | speed-rpc-a-1024-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-1024-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-128-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-128-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-16-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-16-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-16384-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-16384-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-2048-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-256-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-256-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-256-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-32-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-32-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-32768-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-32768-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-4096-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-4096-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-4096-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-512-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-512-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-64-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-64-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-65536-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-8-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-8192-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-a-8192-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-dds-iox-65536-n1-r1 | 测试失败：预热发送失败 |
| 原安装速度 | speed-rpc-dds-iox-65536-n1-r2 | 测试失败：预热发送失败 |
| 原安装速度 | speed-rpc-shm-1024-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-1024-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-128-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-128-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-128-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-131072-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16384-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16384-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-16384-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-2048-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-2048-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-2048-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-256-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-256-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-256-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-32-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-32-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-32768-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-32768-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-32768-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-4096-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-4096-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-512-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-512-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-512-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-64-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-64-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-64-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-65536-n1-r2 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-65536-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-8-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-8-n1-r3 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-8192-n1-r1 | 发送/请求失败 |
| 原安装速度 | speed-rpc-shm-8192-n1-r2 | 发送/请求失败 |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r1 | 原安装发布者/通知器容量耗尽（512 发布者、256 通知器） |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r2 | 原安装发布者/通知器容量耗尽（512 发布者、256 通知器） |
| 原安装压力 | stress-stress-dds-iox-64-n1000-r3 | 原安装发布者/通知器容量耗尽（512 发布者、256 通知器） |


## 交付源码指纹

| 文件 | SHA-256 |
|---|---|
| CMakeLists.txt | 5a403e994b962d84f00862a97ca18976588c602aa72e89c925f9d0ad5bda6aba |
| audit.py | c09aea788afdcbe655693f82f2b7f9267453239c76efbdf731dcb31f2d67984d |
| comparison.cpp | 3220fcd055a84ac05e1b452f99413d6496a729c8e569c2ad1487730e7ec5d265 |
| execute_all.sh | f9f84a64328a25a665a6e315d94ff720342415835e601b37089847abd7bd5c83 |
| generate_types.py | 730ce23d40d6650adc39d8b0c0b2831868f778e9b9466722fe107873ff134efe |
| isolated.sh | 37af643f72dd1278b39ba81f50d4dfa2094d62a7d8ef73dcd756593dd5349936 |
| prepare_roudi.py | 7d57be86cbba2f552472da2e20e1e9205a2df63a07abb5fc7cbef4c20ff7c505 |
| prepare_scaled_deps.sh | 01893f4e22a5a0dbc74b99aeb73537feec256c700e9f73eb86422989821d29e8 |
| report.py | fd900724553b7cc86907c6e338d37ced7e6011b79b43ed63dad65fe4f8d88317 |
| run.py | 7352f0fbd946cbe9cd2699fbaacd0983adad4a6eb8a09eb7be80b03d4eb46355 |
| test_orchestrator.py | 69c2f4763c1321cacf54fa18cbd8e759e4abf4ed6f1aec110dbfded757170204 |

<!-- REPAIR-20261002 -->

## 2026-10-02 修复执行追加

C1（空读确认竞态）与 C2（跨话题分片缓存串用）已修复并通过确定性反例验证。Socket 仅修复已知结束边界，旧协议大包问题仍未闭环；O1 未达收益门槛，已撤回。整体方案尚未通过最终验收。

最新测试覆盖、逐轮状态、修复边界、构建指纹及复现步骤见 [修复结果](repair_results.md) 与 [修复账本](repair_ledger.md)。以下历史账本保持原版本口径。
