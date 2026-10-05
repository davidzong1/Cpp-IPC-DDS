project(test-ipc)

# 配置解析及明确失败分支在后端 OFF 时仍须可链接、可测试。

if(NOT MSVC)
  add_compile_options(
    -Wno-attributes 
    -Wno-missing-field-initializers 
    -Wno-unused-variable 
    -Wno-unused-function)
endif()

include_directories(
    ${LIBIPC_PROJECT_DIR}/include 
    ${LIBIPC_PROJECT_DIR}/src
    ${LIBIPC_PROJECT_DIR}/test
    ${LIBIPC_PROJECT_DIR}/3rdparty
    ${LIBIPC_PROJECT_DIR}/3rdparty/gtest/include
    # dzipc_topic_cat 的只读握手观测探针(handshake_probe.h / info.h)。
    # 只有一个测试(test_handshake_probe.cpp)用它, 但 include_directories 是全局的,
    # 加在这里比给单个 target 加更省事、也不会改变任何既有 target 的行为。
    ${LIBIPC_PROJECT_DIR}/exec/dzipc_topic_cat/include
    ${LIBIPC_PROJECT_DIR})

file(GLOB SRC_FILES
    ${LIBIPC_PROJECT_DIR}/test/*.cpp
    # ${LIBIPC_PROJECT_DIR}/test/profiler/*.cpp
    )
# Exclude benchmark from auto-test list (has its own main, not a gtest)
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/ipc_benchmark.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/dzipc_perf_benchmark.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/dzflat_benchmark.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/dzflat_tx_benchmark.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/dzflat_py_publisher.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/pool_attribution_benchmark.cpp")
# W02 统一跨进程基准: 自带 main(), 同上必须在 GLOB 列表里剔除, 否则会与 gtest_main 冲突。
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/xproc_benchmark.cpp")
# W02 自检用例也是显式 add_executable（要注入 W02_BENCH_BIN 与依赖基准 target），
# 所以同样从 GLOB 里剔除，避免重复定义 target。
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/test_w02_xproc_benchmark.cpp")
file(GLOB HEAD_FILES ${LIBIPC_PROJECT_DIR}/test/*.h)
if(NOT DZIPC_BUILD_SHARED_NET)
    foreach(name test_shared_net_wire test_shared_net_blob test_shared_net_local_protocol test_shared_net_endpoint test_shared_net_io_lifecycle test_shared_net_gateway_lock test_shared_net_client_lifecycle test_shared_net_outbox test_shared_net_outbox_failure test_shared_net_shm_bridge test_shared_net_local_direct test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota)
        list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/${name}.cpp")
    endforeach()
endif()

# 查找 pthread 库
find_package(Threads REQUIRED)

if(ENABLE_DEBUG_INFO)
# 添加调试文件
set(DEBUG_TARGETS test_dzipc)
endif()

# 为每个 .cpp 文件创建单独的可执行文件
foreach(src_file ${SRC_FILES})
    # 获取文件名（不带路径和扩展名）
    get_filename_component(test_name ${src_file} NAME_WE)
    
    # 创建可执行文件
    add_executable(${test_name} ${src_file} ${HEAD_FILES})
     # 检查是否是需要调试的目标
    if(${test_name} IN_LIST DEBUG_TARGETS)
        message(STATUS "Setting debug flags for: ${test_name}")
        if(MSVC)
            target_compile_options(${test_name} PRIVATE
                /Zi          # 完整调试信息(PDB)
                /Od          # 关闭优化
                /Oy-         # 保留帧指针
            )
            target_compile_definitions(${test_name} PRIVATE DEBUG)
            target_link_options(${test_name} PRIVATE /DEBUG)
        else()
            target_compile_options(${test_name} PRIVATE
                -g3          # 最大调试信息
                -ggdb        # GDB专用调试信息
                -O0          # 无优化
                -fno-omit-frame-pointer  # 保留帧指针
            )
            target_compile_definitions(${test_name} PRIVATE DEBUG)
            target_link_options(${test_name} PRIVATE -g)
        endif()
    else()
        # 非调试目标：Debug 配置不强行开启 /O2，避免与 /RTC1 冲突
        if(MSVC)
            target_compile_options(${test_name} PRIVATE
                $<$<CONFIG:Release>:/O2>
                $<$<CONFIG:RelWithDebInfo>:/O2>
                $<$<CONFIG:MinSizeRel>:/O2>
            )
        else()
            target_compile_options(${test_name} PRIVATE
                $<$<CONFIG:Release>:-O2>
                $<$<CONFIG:RelWithDebInfo>:-O2>
                $<$<CONFIG:MinSizeRel>:-O2>
            )
        endif()
        target_compile_definitions(${test_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
    endif()
    if (MSVC)
        target_compile_options(${test_name} PRIVATE /utf-8 /wd4251)
    endif()

    # 链接库 - 注意链接顺序很重要
    target_link_libraries(${test_name} 
        ipc
        gtest
        gtest_main
        Threads::Threads
    )
    if(!ROSBUILD)
        # 设置输出目录
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    else()
        # install(TARGETS ${test_name} DESTINATION lib/test)
    endif()
        
    message(STATUS "Created test executable: ${test_name}")
endforeach()

# ================================================================
# CTest 注册(按需登记, 不是全量)
# ----------------------------------------------------------------
# 上面 file(GLOB) 造出来的 target 默认**不进 CTest**; enable_testing() 在顶层
# CMakeLists.txt 的 LIBIPC_BUILD_TESTS 块里(必须在构建根调用)。
#
# 这里只登记 test_udp_port_boundary 一个: 它此前**没有复现入口** —— 只能手工构建
# build/bin/test_udp_port_boundary 来跑, 而 file(GLOB) 不重跑 cmake 就不会把新
# .cpp 编进构建树, 于是源码改了二进制还是旧的(见共享区 udp_port_overflow_fix_架构评审.md)。
# 其余 target 仍未登记, 不在本次射程 —— ctest 目前只跑这一个。
#
# 命名规范: 与 target 名一致(本项目 target 名 = 源文件 basename, test_ 前缀), 不另起别名。
#
# 用法: cmake -S . -B build && cmake --build build --target test_udp_port_boundary \
#         && ctest --test-dir build -R test_udp_port_boundary --output-on-failure
add_test(NAME test_udp_port_boundary COMMAND test_udp_port_boundary)

# 阶段 1 控制面调度器聚焦用例 (16 条, 见 test/test_shm_control_scheduler.cpp 抬头注释)。
# 它**必须**进 CTest: 该用例是并发注销协议(I1-I4)的回归闸, 靠手工跑
# build/bin/test_shm_control_scheduler 无法进入任何自动回路; 而调度器一旦回归,
# 症状是"已析构宿主被回调"(UAF) 或"注销后计数悄悄多跑", 都是静默失效 —— 只有
# 常驻的自动化判据才能守住。target 由上面 file(GLOB) 自动生成, 无需 add_executable。
# 命名规范同前: 与 target 名一致, 不另起别名。
add_test(NAME test_shm_control_scheduler COMMAND test_shm_control_scheduler)

# MPMC shared-memory queue contract: concurrent writers must preserve every
# writer/sequence pair for each broadcast receiver.  Keep this focused test in
# CTest so the topology cannot regress while the higher-level publisher
# registry is still being implemented.
add_test(NAME test_shm_mpmc_channel COMMAND test_shm_mpmc_channel)
set_tests_properties(test_shm_mpmc_channel PROPERTIES TIMEOUT 30)

# Type-adapter contract: RouteSession and LoanedMessage can carry the explicit
# MPMC channel without changing the legacy route aliases.
add_test(NAME test_shm_mpmc_types COMMAND test_shm_mpmc_types)
set_tests_properties(test_shm_mpmc_types PROPERTIES TIMEOUT 30)

# MPMC mode parsing is deliberately pure so deployment scripts can validate
# the opt-in contract without creating a shared-memory object.
add_test(NAME test_shm_mpmc_config COMMAND test_shm_mpmc_config)
set_tests_properties(test_shm_mpmc_config PROPERTIES TIMEOUT 10)

# Publisher registry lease contract: shared generation, identity-guarded
# heartbeat/leave, coordinator lease transfer, and stale slot reclamation.
add_test(NAME test_shm_mpmc_control_plane COMMAND test_shm_mpmc_control_plane)
set_tests_properties(test_shm_mpmc_control_plane PROPERTIES TIMEOUT 30)

add_test(NAME test_shm_multi_publisher COMMAND test_shm_multi_publisher)
set_tests_properties(test_shm_multi_publisher PROPERTIES TIMEOUT 60 ENVIRONMENT "DZIPC_SHM_MPMC=1")

# 用例内含 4 线程 churn 与 250ms 慢回调, 实测全套约 3s; 给 120s 只为在死锁
# (本用例要抓的失败模式之一: 注销等待在途回调时自锁) 时让 ctest 报超时而非永久
# 阻塞构建机, 不是性能判据。
set_tests_properties(test_shm_control_scheduler PROPERTIES TIMEOUT 120)

# ================================================================
# W05 控制面 stale 门控**承重用例**登记（W12 T03 / 架构负责人 = CTest 登记归口）
# ----------------------------------------------------------------
# 依据：方案 §5.2.4（架构与接口负责人）+ 队长裁定 §4.4 / R1-W05-F1(high) 的
#       W04 接口修订路径（`has_peers()==false` 时**不得**跳过 stale 扫描, 只按
#       `peer_dead_timeout`(2s) 同量级**低频兜底**扫一次）+ W05 的 add_test 需求
#       （`W05/add_test需求_t77_stale门控用例.md`）。
# ⛔ 低频兜底**不是**降频/省 CPU 优化：扫 64 个槽位是常数开销(µs 级), 它的全部
#    意义是**正确性**（`collect_stale_peers()` 判死的是 PeerSlot 而非 peer_count;
#    `remove_peer()`→`release_peer_slot()` 两步之间的窗口会留下
#    `peer_count==0 + slot.in_use==1 + 心跳陈旧`, 整条跳过 ⇒ 永不回收并最终误断活订阅者）。
#
# 两个用例各守一层, 缺一不可：
#   · test_w05_stale_slot_gate     : peer_count()==0 + 陈旧 in_use 槽位 ⇒ ①必须回收
#                                    ②随后挂上的活订阅者**不得**被误断（端到端）；
#   · test_w05_stale_slot_gate_arm : 同上两条判据, 但**按臂各跑一次**（默认臂 = 进程级
#                                    调度器；L1 臂 = DZIPC_SHM_CONTROL_SCHEDULER=1 的
#                                    每话题兼容线程）。两条驱动源共用唯一判据
#                                    `pub_control_tick()` ⇒ 结构上不可再分叉。
# 反向验证（"判据的判据"）：把任一驱动源的判据改回"无 peer 就跳过" ⇒ 对应用例**必须变红**
# （消融实测见 artifacts/perf/20261001-t77-W05-F1/negative/ 与
#  artifacts/perf/20261001-t84-W05-F1/negative/）。⛔ 本条是承重用例进入自动回路的唯一理由。
#
# 为什么 RUN_SERIAL：两用例都**真实建控制面段**并等到 `peer_dead_timeout`(2s) 量级
# （占 /dev/shm 与 notify 唤醒），与其它建段用例并发跑会互相干扰（同 W04 对共享段类
# 用例的结论, 见本文件上方 D-13 块）。TIMEOUT 120 只为卡死时 ctest 报超时而非永久
# 阻塞构建机, **不是**性能判据。
#
# ⛔ 已知限制（如实登记, ⛔ 不得据此声称 ctest 恒全绿）────────────────────────────
#   W12 T02(t3) 实测: **L1 回退臂**在 158 次中有 1 次返回 `rx=99/100`
#   （判据①陈旧槽位回收**已通过**, 失败在判据②消息计数）；双臂用例的 L1 臂另有
#   2/26 次 `rc=2`（活订阅者被误断）。判别实验（干净路径 50/50、门控逐条诊断
#   52/52、32 路 CPU 加压 12/12）全部通过, **机制未定位, 未声称已修复**。
#   证据: artifacts/perf/20261001-w12-T02/flake_observation.md、flake_stats.csv；
#   默认臂 78/78 与 26/26 全通过。
#   ⇒ 全量回归存在**已知的、非零的**偶发失败概率。⛔ **禁止**用 `RETRY` /
#     `--repeat until-pass` / `set_tests_properties(... RETRY ...)` 等重试机制
#     把该偶发掩盖成绿 —— 那正是"游戏化门禁"；本登记**不设**任何重试属性。
#     偶发一旦在 ctest 中变红, 必须按 §13 如实上报, 不得重跑取绿后宣称通过。
#
# 登记方式：target 由本文件上方 file(GLOB)(test/*.cpp) 自动生成（源文件已在树内,
# 故**不需** add_executable）；本条为**纯追加**, ⛔ 未改动任何既有行。
# 计数口径：**原有 29 项 + 新增 2 项 = 31 项**（⛔ 不得为凑 29 删 add_test）。
# ================================================================
add_test(NAME test_w05_stale_slot_gate COMMAND test_w05_stale_slot_gate)
add_test(NAME test_w05_stale_slot_gate_arm COMMAND test_w05_stale_slot_gate_arm)
set_tests_properties(
    test_w05_stale_slot_gate
    test_w05_stale_slot_gate_arm
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# 阶段 2 RouteSession 聚焦用例 (15 条, 见 test/test_shm_route_session.cpp 抬头注释)。
# 必须进 CTest 的理由同上一项: 本用例守的是 lease 配对与「release 不得与 recv 并发」
# 这两条**静默**失效 —— 漏配对 = wait_quiescent 永久挂死, 不等待在途 recv 就
# release = 偶发 SIGSEGV(功能测「最终又能收到消息」永远抓不到)。手工跑
# build/bin/test_shm_route_session 进不了任何自动回路。
# target 由上面 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
add_test(NAME test_shm_route_session COMMAND test_shm_route_session)

# 用例内含 4 读者 churn + 一次真 recv(2000) 等 disconnect 叫醒 + 若干 200ms 有界等待,
# 实测全套约 1.4s; 给 120s 只为在死锁(本用例要抓的失败模式: acquire/release 计数
# 不配对使 wait_quiescent 永不返回)时让 ctest 报超时而非永久阻塞构建机, 不是性能判据。
set_tests_properties(test_shm_route_session PROPERTIES TIMEOUT 120)

# 第 7 个用例 PreviouslyFatalTopicNowConnectsEndToEnd 起真 pub/sub: 1.5s 等握手 + 2.5s
# 发布, 正常约 5s; 组播不可用时它 GTEST_SKIP 而不是挂住。给 120s 只为卡死时 ctest 报
# 超时而非永久阻塞, 不是性能判据。
set_tests_properties(test_udp_port_boundary PROPERTIES TIMEOUT 120)

# 阶段 2 叫醒伪影守门聚焦用例 (10 条, 见 test/test_wakeup_artifact.cpp 抬头注释)。
# 必须进 CTest 的理由同前两项: 本用例守的是一条**只在叫醒那一瞬出现**的伪影
# (recv 阻塞中被 disconnect 叫醒 ⇒ 返回 64 字节全零、empty() == false)。手工跑
# build/bin/test_wakeup_artifact 进不了任何自动回路, 而它的两种失效都是静默的 ——
# 守门漏了 = 用户侧凭空收到全零假消息; 守门过宽 = 真消息被静默吃掉。
# target 由上面 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
add_test(NAME test_wakeup_artifact COMMAND test_wakeup_artifact)

# 层 1 有 2 条真 route 阻塞 + disconnect 叫醒(各 200ms), 层 2 有 3 轮起停 pub(3.6s)
# 与两次重建各带 300-600ms 有界等待, 实测全套约 5s; 给 120s 只为卡死时 ctest 报超时
# 而非永久阻塞构建机, 不是性能判据。
set_tests_properties(test_wakeup_artifact PROPERTIES TIMEOUT 120)

# 阶段 2 全量测试方案 §4.1 的 I5 确定性守门 (3 条, 见 test/test_shm_i5_pop_buffer.cpp
# 抬头注释)。必须进 CTest 的理由: 它守的是「recv 已弹出但尚未分流的 buffer 不得因
# generation 落后被丢」—— 这个窗口只有几条指令宽, 靠随机压力**不可能**命中, 是本套件
# 用 include/dzIPC/detail/shm_sub_seam.h 的测试缝把收包线程停住才做到的。它一旦回归,
# 症状是"重建期间偶发丢消息", 端到端功能测永远抓不到。
# 用例内含真实 pub/sub + 两次 generation 推进, 实测全套约 4.3s; 给 120s 只为卡死时
# ctest 报超时而非永久阻塞构建机, 不是性能判据。
add_test(NAME test_shm_i5_pop_buffer COMMAND test_shm_i5_pop_buffer)
set_tests_properties(test_shm_i5_pop_buffer PROPERTIES TIMEOUT 120)

# 阶段 2 全量测试方案 §4.2 的析构唤醒集成守门 (2 条, 见 test/test_shm_sub_dtor_gate.cpp
# 抬头注释)。必须进 CTest 的理由: 删掉 ~shm_sub_ipc 里的 route_session_.stop_and_wake()
# 时, 既有 4 套件 79 条**全绿**(v4 只读验收实测) —— 因为收包线程本来就会在 recv(50)
# 超时后自己返回, 功能上看不出区别。本套件用子进程 + 钩子记录析构各阶段的**因果顺序**
# 与「connected_id()==0 的 recv 返回」把这一步钉死。
# 两条用例各 fork 一个子进程(父进程 15s 硬超时), 实测全套约 1s; 给 120s 只为子进程
# 卡死时 ctest 报超时而非永久阻塞构建机, 不是性能判据。
add_test(NAME test_shm_sub_dtor_gate COMMAND test_shm_sub_dtor_gate)
set_tests_properties(test_shm_sub_dtor_gate PROPERTIES TIMEOUT 120)

# 阶段 2 全量测试方案 §4.3 的 Ready 状态转换集成覆盖 (3 条, 见
# test/test_shm_ready_transition.cpp 抬头注释)。必须进 CTest 的理由: 它守的是
# 「离开 Ready ⇒ 释放 route 并回收 peer 登记」与「新 generation Ready ⇒ 恰好恢复一条」
# —— 前者回归就是「连不上的订阅者冒充在线」那个假成功态(docs/shm_defect_fixes.md 第 2 条),
# 后者回归是重建期丢消息或重复投递, 两者都不崩不报错。
# 三条用例各含真实 pub/sub 与 1.5-3s 的有界等待, 实测全套约 4.9s; 给 120s 同上。
add_test(NAME test_shm_ready_transition COMMAND test_shm_ready_transition)
set_tests_properties(test_shm_ready_transition PROPERTIES TIMEOUT 120)

# 阶段 4 recv_wait_set：只验证 token、sequence、动态注销/停止和 disconnect 唤醒；
# 不接入阶段 5 worker，也不改变现有 shm_sub_ipc 收包线程。
add_test(NAME test_recv_wait_set COMMAND test_recv_wait_set)
set_tests_properties(test_recv_wait_set PROPERTIES TIMEOUT 120)

# 阶段 5 共享等待层 / 固定收包 worker 的两个聚焦套件（见 test/test_recv_worker.cpp
# 与 test/test_socket_wait_set.cpp 抬头注释）。
# 必须进 CTest 的理由与上面几项同构 —— 它们守的失效方式**全部是静默**的：
#   · recv_worker：固定归属漂移 ⇒ thread_local 分片缓存换槽 ⇒ 偶发丢消息；
#     注销不等在途 recv_once ⇒ 偶发 SIGSEGV；预算耗尽当丢弃 ⇒ 热话题静默丢包；
#     单消费者互斥漏掉 ⇒ 兼容线程与 worker 双收同一条 route。
#   · socket_wait_set：remove/stop 不唤醒阻塞中的 wait ⇒ worker 睡过注销 ⇒ 宿主按
#     协议释放通道后被打（UAF）；摘除不彻底 ⇒ fd 复用把旧事件算到新通道 ⇒ 空转；
#     只消费事件不真读 ⇒ LT 下忙轮询吃满 CPU。
# 手工跑 build/bin/<name> 进不了任何自动回路，这两套件就是它们唯一的常驻判据。
# target 由上面 file(GLOB) 自动生成，无需 add_executable。命名规范同前。
#
# 用例内含真 pub/sub、多线程 churn 与若干秒级有界等待（idle exit 一项本身要等
# 1.1s 量级），实测两套件合计约 20s；给 120s 只为在死锁（本套件要抓的失败模式：
# 注销等待在途 recv 时自锁、wait 线程永不返回）时让 ctest 报超时而非永久阻塞构建机，
# 不是性能判据。
add_test(NAME test_recv_worker COMMAND test_recv_worker)
set_tests_properties(test_recv_worker PROPERTIES TIMEOUT 120)

add_test(NAME test_socket_wait_set COMMAND test_socket_wait_set)
set_tests_properties(test_socket_wait_set PROPERTIES TIMEOUT 120)

# 阶段 5 socket 侧共享收包 worker 的聚焦自检（见 test/test_socket_recv_worker.cpp
# 抬头注释）。它与 test_recv_worker 同构：socket 收包 worker 是 t4（socket_pub_sub）
# 与 t5（socket_ser_cli）共同消费的共享组件，四种失效方式（固定归属漂移 /
# 与兼容收包线程双收 / 注销不唤醒阻塞中的 wait / 空闲退出后不按需拉起）**全部静默**，
# 手工跑 build/bin/test_socket_recv_worker 进不了任何自动回路。
# 用例内含真组播通道与若干秒级有界等待，实测约 2s；给 120s 只为卡死时报超时。
add_test(NAME test_socket_recv_worker COMMAND test_socket_recv_worker)
set_tests_properties(test_socket_recv_worker PROPERTIES TIMEOUT 120)

# t12 追加：非阻塞可读判据 readable()/udp_node_readable 的聚焦判据（见
# test/test_socket_readable.cpp 抬头注释）。它是 socket 侧收包 worker 的**准入判据**：
# 没有它，recv_once() 只能直接调 chunk_rev_topic/chunk_rev_server，而两者一律带 tm 且
# 空闲时阻塞到 tm（50ms/200ms）⇒ 每次数据突发收尾都会让**共享** worker 线程空读阻塞
# 最长 tm。三档失效（该真时不真 / 该假时不假 / 变慢）全部静默，必须常驻自动回路。
# 用例内含真组播通道与毫秒级时间断言，实测约 0.1s；给 120s 只为卡死时报超时。
add_test(NAME test_socket_readable COMMAND test_socket_readable)
set_tests_properties(test_socket_readable PROPERTIES TIMEOUT 120)

# [t8] P0 并发回归（见 test/test_socket_ser_concurrency.cpp 抬头注释）。
# 存在理由：t7 的 C-01/C-02 是 shared_ptr 成员的数据竞争，而既有 test/ 对
# reset_message / reset_callback / stop_data_plane / restart_data_plane **零覆盖**
# => 功能用例全绿对该竞争不可证伪。本套件把这三条入口真正并发跑起来。
# 它**不**替代结构性访问点审查（那是 t9 的必查项），只回答"并发路径能跑通"。
add_test(NAME test_socket_ser_concurrency COMMAND test_socket_ser_concurrency)
set_tests_properties(test_socket_ser_concurrency PROPERTIES TIMEOUT 120)

# [t8-C05] SHM ser-cli 有界 FIFO 的**容量行为**对照（见 test/test_shm_ser_backpressure.cpp）。
# 存在理由：t8 给 SerState FIFO 落地了「入库前容量判定 + 零丢失背压」，但既有功能用例的请求
# 速率太低、FIFO 永远不会满 ⇒ 该性质**无任何判据**。本用例用慢 callback 把 FIFO 压满，
# 断言「所有请求最终都被应答」（零静默丢弃）。实测约 0.6s；给 120s 只为卡死时报超时。
add_test(NAME test_shm_ser_backpressure COMMAND test_shm_ser_backpressure)
set_tests_properties(test_shm_ser_backpressure PROPERTIES TIMEOUT 120)

#target_link_libraries(${PROJECT_NAME} tcmalloc_minimal)

# ================================================================
# Standalone benchmark binary (not a gtest)
# ================================================================
add_executable(ipc_benchmark ${LIBIPC_PROJECT_DIR}/test/ipc_benchmark.cpp ${HEAD_FILES})
target_compile_options(ipc_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(ipc_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(ipc_benchmark ipc Threads::Threads)
set_target_properties(ipc_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: ipc_benchmark")

# ================================================================
# dzIPC performance benchmark (pub-sub / ser-cli x SHM / SOCKET)
# Standalone binary with its own main(), not a gtest.
# ================================================================
add_executable(dzipc_perf_benchmark ${LIBIPC_PROJECT_DIR}/test/dzipc_perf_benchmark.cpp)
target_compile_options(dzipc_perf_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(dzipc_perf_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(dzipc_perf_benchmark ipc Threads::Threads)
if(UNIX AND NOT APPLE)
    # shm_open/shm_unlink 在旧 glibc 上位于 librt
    target_link_libraries(dzipc_perf_benchmark rt)
endif()
set_target_properties(dzipc_perf_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: dzipc_perf_benchmark")
# ================================================================
# 步骤② chunk 池占用归因基准 (docs/shm_chunk_pool_occupancy_plan.md §3 步骤②)
# 自带 main(), 不是 gtest。必须同时加进上面的 list(REMOVE_ITEM), 否则 GLOB 会
# 把它收成 gtest 目标并与自带的 main() 冲突。
# ================================================================
add_executable(pool_attribution_benchmark ${LIBIPC_PROJECT_DIR}/test/pool_attribution_benchmark.cpp)
target_compile_options(pool_attribution_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(pool_attribution_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(pool_attribution_benchmark ipc Threads::Threads)
if(UNIX AND NOT APPLE)
    # shm_open/shm_unlink 在旧 glibc 上位于 librt
    target_link_libraries(pool_attribution_benchmark rt)
endif()
set_target_properties(pool_attribution_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: pool_attribution_benchmark")
# ================================================================
# DZFlat 离线基准 (docs/dzflat_shm.md Step 1 验收门)
# 自带 main(), 不是 gtest; 覆写 global operator new 以统计堆分配次数。
# ================================================================
add_executable(dzflat_benchmark ${LIBIPC_PROJECT_DIR}/test/dzflat_benchmark.cpp)
target_compile_options(dzflat_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(dzflat_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(dzflat_benchmark ipc Threads::Threads)
set_target_properties(dzflat_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: dzflat_benchmark")

# ================================================================
# DZFlat 传输层基准 (docs/dzflat_shm.md Step 2 验收门)
# 走真实 SHM 链路, 自带 main(), 不是 gtest。
# ================================================================
add_executable(dzflat_tx_benchmark ${LIBIPC_PROJECT_DIR}/test/dzflat_tx_benchmark.cpp)
target_compile_options(dzflat_tx_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(dzflat_tx_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(dzflat_tx_benchmark ipc Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(dzflat_tx_benchmark rt)
endif()
set_target_properties(dzflat_tx_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: dzflat_tx_benchmark")

# ================================================================
# DZFlat 的 Python 侧验收用发布端 (docs/dzflat_shm.md Step 4)
# Python 发不出 DZFlat(GenericMessage 无 schema 写不出定长布局), 所以由这个小程序
# 充当发布端, 供 test/test_dzflat_python.py 订阅比对。自带 main(), 不是 gtest。
# ================================================================
add_executable(dzflat_py_publisher ${LIBIPC_PROJECT_DIR}/test/dzflat_py_publisher.cpp)
target_compile_options(dzflat_py_publisher PRIVATE $<$<CONFIG:Release>:-O2>)
target_link_libraries(dzflat_py_publisher ipc Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(dzflat_py_publisher rt)
endif()
set_target_properties(dzflat_py_publisher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created helper executable: dzflat_py_publisher")

# ================================================================
# W03 测量口径与路径观测 (docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md §4 W03)
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# 用例覆盖: 单机跨进程单调时钟一致性、已知线程活动的上下文切换计入(且必须超过
# /proc/<pid>/status 的主线程参考值)、新生/退出线程完整性标注、子进程 CPU/切换与
# wait4 rusage 对照、低开销计数与诊断门控、三种空闲状态分类、相位与 >=60s 窗口判定、
# 恢复首包延迟统计、路径证据不足标"未确认"、字段定义/JSON/CSV schema。
# ================================================================
add_test(NAME test_w03_measurement COMMAND test_w03_measurement)

# ================================================================
# W00 构建闸门补登记 (docs/消息接收架构改造/团队改造交付/基线与门槛.md §4.4)
# ----------------------------------------------------------------
# 缺口: test_dzflat_transport / test_dzflat_builder 此前只是 file(GLOB) 造出的
# target, **未进 CTest**; 前者含跨进程 B 路径用例
# (CrossProcessRoundTripProvesPositionIndependence, 见 W08/W00 复核),
# 后者覆盖 chunk 内指针区间/超预算失败/借样归还/B 就地构造。两者不进 CTest
# 意味着 **B 路径正确性从未被任何自动回归覆盖** —— 与方案 §10.6「假通过防线」
# 冲突, 故补登记。target 由上方 file(GLOB) 自动生成, 无需 add_executable。
#
# 命名规范与前 15 项一致: 与 target 名相同, 不另起别名。
# 时限 300s: 两者实测各约 3–4s(含 fork 与共享内存建段), 给足余量仅为让死锁
# 时报超时而非永久挂住构建机, 不是性能判据。
# 由架构负责人(W00)落地; socket与数据面负责人(W08)提出请求。
# ================================================================
add_test(NAME test_dzflat_transport COMMAND test_dzflat_transport)
add_test(NAME test_dzflat_builder   COMMAND test_dzflat_builder)
# ⚠️ 共享 chunk 池/DZFlat 段是**全机**资源(每尺寸档 40 块), 且 W08 的验收含
#    `dzflat_fallback == 0` 与"该路径条数=N、另两条=0"——若邻居把池吃干, 贷款失败会
#    表现为**假红**(fp 回退计数上升), 反之也可能掩盖真实回退。故三者串行化。
#    ⛔ 用 RUN_SERIAL 而非 RESOURCE_LOCK: RESOURCE_LOCK 只串行化**同样声明该锁**的用例,
#    挡不住未声明的邻居(该结论已由 W00 §4.4/D-10 实测确立并被队长采纳为团队判据)。
set_tests_properties(test_dzflat_transport test_dzflat_builder PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# ================================================================
# W04 SHM 接入契约复审 · 可执行竞态与容量偏斜判据
# (docs/消息接收架构改造/团队改造交付/接口与生命周期.md §6/§9)
# ----------------------------------------------------------------
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# 为什么必须进 CTest: W04 是 W05/W06 的**共同前置**, 而它守的失效方式全部静默 ——
#   ① 固定哈希偏斜 ⇒ 某 worker 先满("127 × worker_count 只是上界", 方案 §10.5);
#   ② 满载 failed 若不归还收包独占 ⇒ 重新注册永久 busy(症状是"某几个话题偶尔收不到");
#   ③ 恒真 has_pending 无上限 ⇒ 一个坏宿主实现把整个 worker 钉死(饿死邻居不报错);
#   ④ 一次耗时 recv_once 不可被 200µs 预算抢占 ⇒ 同 worker 邻居确定性延迟(方案 §10.3),
#      它不是可被容量/线程数"修好"的性能抖动。
# 手工跑 build/bin/test_lifecycle_contract 进不了任何自动回路。
#
# 用例内含真 pub/sub 与 127 条 route 的边界构造(全部落在同一 worker), 实测约 6s;
# 给 120s 只为在死锁(本套件要抓的失败模式: 注销等 in-flight 自锁)时报超时而非永久
# 阻塞构建机, 不是性能判据。
# ================================================================
add_test(NAME test_lifecycle_contract COMMAND test_lifecycle_contract)
set_tests_properties(test_lifecycle_contract PROPERTIES TIMEOUT 120)

# ================================================================
# W07 socket 容量与高 fd (docs/消息接收架构改造/团队改造交付/W07/socket容量与高fd修复_交付.md)
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# 为什么必须进 CTest: 这三个用例守的是**既有缺陷的回归闸** ——
#   ① select/FD_SET 在 fd 号 >= FD_SETSIZE(1024) 时 abort(实测 __fdelt_chk + SIGABRT),
#      千路订阅必踩; 这与"订阅个数"无关, 判据是 **fd 号**;
#   ② info_pool 注册表容量从 512 上调后, 旧/新布局共享段的混合挂载必须**显式拒绝**
#      而不是静默错解(读到别人的槽位);
#   ③ 容量/布局/版本三件事必须分别可验, 不能用一个总数掩盖。
# 手工跑 build/bin/test_socket_high_fd 进不了任何自动回路。
#
# ⚠️ 池是全机共享段: test_ipc_info_pool_version 会在**生产段名**上短暂预置一个旧布局段
#    (必须如此才测得到"拒绝而非静默错解"), 用例结束前已 unlink。故本组用例**不得并行**
#    —— 对会碰共享段状态的用例用 RUN_SERIAL(见下方说明: 本组里 RESOURCE_LOCK 挡不住
#    未声明邻居, 实测已复现 test_ipc_info_pool 并行必失败)。
# ================================================================
add_test(NAME test_ipc_info_pool COMMAND test_ipc_info_pool)   # 既有测试, 顺带登记(此前未入 ctest)
add_test(NAME test_ipc_info_pool_layout  COMMAND test_ipc_info_pool_layout)
add_test(NAME test_ipc_info_pool_version COMMAND test_ipc_info_pool_version)
add_test(NAME test_socket_high_fd        COMMAND test_socket_high_fd)
# ⚠️ 实测(2026-09-28, W00 登记复核): test_ipc_info_pool **必须 RUN_SERIAL**。
# 它的不变式是「尝试前表必须恰好满 kMaxEntries 条」(test_ipc_info_pool.cpp:436 的
# ASSERT_EQ(snapshot(false).size(), kMaxEntries)); 同机**任何**其他实例释放自己的槽,
# 该断言就失败并判为"本用例不成立"。`ctest -j4` 全量跑时它被 22 项里的其他套件干扰,
# 实测复现: Expected 3847 == 4096, 判 FAILED —— 而单独跑稳定 Passed(1.42s)。
# RESOURCE_LOCK 不够用: 它只串行化**同样声明该锁**的用例, 挡不住未声明的邻居;
# 用例自己就是为此写了"共享池被外部进程改动…请重跑"的断言, 属设计内可预见的干扰。
# RUN_SERIAL 把该用例与其余所有测试排开, 只加时间约束、不改用例语义。
set_tests_properties(test_ipc_info_pool PROPERTIES RUN_SERIAL TRUE)
# version 用例在生产段名上短暂预置旧布局段(结束前 unlink), 与 layout 同属段级互斥。
set_tests_properties(test_ipc_info_pool_version PROPERTIES RUN_SERIAL TRUE)
set_tests_properties(test_socket_high_fd PROPERTIES TIMEOUT 300)   # 实测 ~3.4s(含千路+高 fd 子进程)

# ================================================================
# W02 统一跨进程基准（docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md §4 W02）
# ----------------------------------------------------------------
# 自带 main()，不是 gtest；因此必须同时在上面的 file(GLOB) 列表里 REMOVE_ITEM，
# 否则会被 GLOB 收成 gtest 目标并与自带 main() 冲突（与 ipc_benchmark 同构）。
#
# 为什么目标名不叫 w02_benchmark：目录里已有的 w02_dds_types.{c,h} 是 idlc 生成的
# DDS 类型（同一工作包），target 名前缀保持一致便于检索。
#
# CycloneDDS 是**可选**依赖：找不到头/库时只编 dzIPC 三档（TLV / DZFlat A / DZFlat B），
# 并在配置输出里显式标注关闭（不做静默降级 —— 方案 §10.6 的假通过防线）。
# 用法：
#   cmake -S . -B build && cmake --build build --target xproc_benchmark
#   build/bin/xproc_benchmark --smoke
# 可选指定本机 CycloneDDS 前缀：
#   -DW02_CYCLONEDDS_ROOT=/path/to/cyclonedds
# ================================================================
set(W02_CYCLONEDDS_ROOT "/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2"
    CACHE PATH "W02 统一跨进程基准用的 CycloneDDS 前缀（包含 include/dds/dds.h 与 lib/libddsc.so）")
find_path(W02_DDS_INCLUDE dds/dds.h HINTS "${W02_CYCLONEDDS_ROOT}/include")
find_library(W02_DDS_LIB ddsc HINTS "${W02_CYCLONEDDS_ROOT}/lib")
if(W02_DDS_INCLUDE AND W02_DDS_LIB)
    add_executable(xproc_benchmark
        ${LIBIPC_PROJECT_DIR}/test/xproc_benchmark.cpp
        ${LIBIPC_PROJECT_DIR}/test/w02_dds_types.c)
    target_compile_definitions(xproc_benchmark PRIVATE W02_HAVE_DDS=1
        W02_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_include_directories(xproc_benchmark PRIVATE "${W02_DDS_INCLUDE}")
    message(STATUS "W02 xproc_benchmark: CycloneDDS 已启用 (${W02_DDS_LIB})")
else()
    add_executable(xproc_benchmark ${LIBIPC_PROJECT_DIR}/test/xproc_benchmark.cpp)
    target_compile_definitions(xproc_benchmark PRIVATE W02_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    message(STATUS "W02 xproc_benchmark: CycloneDDS 未找到 ⇒ 只编 TLV/DZFlat-A/DZFlat-B 三档 "
                   "(设 -DW02_CYCLONEDDS_ROOT=... 启用)")
endif()
target_compile_options(xproc_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_compile_definitions(xproc_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(xproc_benchmark ipc Threads::Threads)
if(W02_DDS_INCLUDE AND W02_DDS_LIB)
    target_link_libraries(xproc_benchmark "${W02_DDS_LIB}" m)
    # DDS 只作为**可选后端**链接：固定 rpath 便于记录"实际加载库路径"（方案 §12）。
    set_target_properties(xproc_benchmark PROPERTIES BUILD_RPATH "${W02_CYCLONEDDS_ROOT}/lib")
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(xproc_benchmark rt)
endif()
set_target_properties(xproc_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created benchmark executable: xproc_benchmark")

# 小规模正确性用例：**只在显式 -DW02_REGISTER_SMOKE_TEST=ON 时进 CTest**。
# 理由：--smoke 会真实建 SHM 段与两个子进程，本身 ~40s，且它测的是"基准可用性"
# 而不是产品行为；放进默认 ctest 会让其余用例的并行/段级隔离假设变复杂。
# 需要时显式打开，或用 test/perf 下的脚本跑（见 w02_benchmark.sh/README）。
# W02 小规模正确性用例（gtest）：它**自己**会拉起一次 0.3s 的跨进程基准运行，
# 然后从落盘产物反证"身份可核验 / 计时边界一致 / 失败与丢失被计数"三条验收。
# 依赖基准可执行文件路径 → 用 W02_BENCH_BIN 注入（绝对路径，不受 cwd 影响）。
add_executable(test_w02_xproc_benchmark
    ${LIBIPC_PROJECT_DIR}/test/test_w02_xproc_benchmark.cpp)
target_compile_definitions(test_w02_xproc_benchmark PRIVATE
    W02_BENCH_BIN="$<TARGET_FILE:xproc_benchmark>")
target_compile_options(test_w02_xproc_benchmark PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>)
target_compile_definitions(test_w02_xproc_benchmark PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_link_libraries(test_w02_xproc_benchmark ipc gtest gtest_main Threads::Threads)
set_target_properties(test_w02_xproc_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(test_w02_xproc_benchmark xproc_benchmark)
# 进 CTest 的理由：它守的三条正是 W02 的验收条件，而这些都是**静默失效**形态
# （身份括号写错 → 读数仍存在但不可信；边界混用 → 数字变了却没人报错；
#   失败被静默过滤 → 丢失看起来像"没有消息"）。实测约 2s；给 300s 只为在
# 基准死锁时报超时，不是性能判据。RUN_SERIAL：它会真建 SHM 段与两个子进程。
add_test(NAME test_w02_xproc_benchmark COMMAND test_w02_xproc_benchmark)
set_tests_properties(test_w02_xproc_benchmark PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
message(STATUS "Created W02 benchmark self-check: test_w02_xproc_benchmark")

# W02 产物结构闸（t47）：`summary.csv` 表头列数 / `write_summary_csv` 格式串字段数 / 数据行列数
# 三者必须相等。为什么必须进 CTest：t47 给 summary.csv 插了 6 列而**格式串没同步**，导致
# `fprintf` 读越界 ⇒ 进程 SIGSEGV（rc=139）且 summary.csv 为 0 字节。那个缺陷是靠"跑一遍就崩"
# 才发现的；本闸把它变成**每次 ctest 都能拦住的机械判据**（只查源码，毫秒级、无副作用）。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    # 静态半（t47 原有，⛔ 未改动）：表头列数 == `write_summary_csv` 的格式串字段数。
    add_test(NAME test_w02_summary_schema COMMAND ${Python3_EXECUTABLE}
             ${LIBIPC_PROJECT_DIR}/test/w02_summary_schema_check.py --source-only)
    set_tests_properties(test_w02_summary_schema PROPERTIES TIMEOUT 60)

    # 动态半（t50/R-1 新增）：用**固化小样本**跑「逐行 gate ↔ failure_reasons」「gate 取值域」
    # 「表头列数 ↔ 数据行列数」三条动态判据。
    #   为什么必须单独登记：t48/R1 的独立注入证明——只登记静态半时，注入 F（`gate=0` 而名点空，
    #   = 判定逻辑不一致）与注入 G（`gate=2` 越界取值会被读成「通过」）**都不会被自动拦住**。
    #   fixture 目录：`test/fixtures/w02_summary_schema/`（1 个正控 + 3 条注入 F/G/A + README）。
    #   ⛔ 不跑 75-case 矩阵（那是 `--smoke-dds`，不进 ctest）；本项只读 3 行固化 CSV。
    #   自带三条注入 ⇒ 任一形态逃过闸即 rc≠0 ⇒ 「闸失效」会被它自己抓出来（不是自述式通过）。
    #   无副作用：只读 fixture、不写仓库、不建 SHM 段 ⇒ **不需要 RUN_SERIAL**。
    add_test(NAME test_w02_summary_schema_dynamic COMMAND ${Python3_EXECUTABLE}
             ${LIBIPC_PROJECT_DIR}/test/w02_summary_schema_check.py --fixtures)
    set_tests_properties(test_w02_summary_schema_dynamic PROPERTIES TIMEOUT 60)
    message(STATUS "Created W02 summary schema gates: test_w02_summary_schema (+dynamic)")
else()
    message(STATUS "未找到 Python3 ⇒ 跳过 test_w02_summary_schema(_dynamic)（t47/t50 的结构闸）")
endif()

option(W02_REGISTER_SMOKE_TEST "把 W02 基准冒烟用例登记进 CTest（默认 OFF）" OFF)
if(W02_REGISTER_SMOKE_TEST)
    add_test(NAME w02_xproc_benchmark_smoke COMMAND xproc_benchmark --smoke
             --out-dir=${CMAKE_BINARY_DIR}/w02_smoke)
    set_tests_properties(w02_xproc_benchmark_smoke PROPERTIES RUN_SERIAL TRUE TIMEOUT 900)
endif()

# ================================================================
# W08 DZFlat A/B 专项验证 (docs/消息接收架构改造/团队改造交付/W08/DZFlat_A推广与B专项验证_交付.md)
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# ⚠️ @note 关键: 该用例**自带落盘**, 且 run_id 是**硬编码**的六个
#    (20260928-r05..r10-W08-*), repo_root() 由 /proc/self/exe 推回仓库根, 落盘一律
#    fopen(...,"wb") **截断**。若不改变落点, 每跑一次 ctest 就会**静默覆盖仓库内的 W08
#    原始证据**(实测: 跑两次后 r05/r06/r08 的 samples.csv 中位数已与交付文档记载不符,
#    违反方案 §12「结果目录只追加、不覆盖旧日志」)。
#    故此处用 ENVIRONMENT 把落点重定向到 build/ 下的 scratch(已 .gitignore):
#      ① 该用例在本闸门里的职责是**正确性**(三路径可分、载荷全量校验、回退行为),
#         不是性能证据 —— 性能证据必须按 §12 由显式运行生成**新 run_id**;
#      ② W08_ARTIFACT_ROOT 是测试自带的官方覆盖点(test_w08_dzflat_ab.cpp:223)。
#    ⛔ 不要删掉 ENVIRONMENT —— 删掉即恢复"每次 ctest 覆盖仓库证据"的行为。
#
# ⚠️ 为什么必须 RUN_SERIAL(t20 要求评估"是否天然隔离"—— 结论: **不隔离**, 三条实证):
#    ① run_id 硬编码且**无 PID/时间戳**: 20260928-r05..r10-W08-*(test_w08_dzflat_ab.cpp:673-679);
#    ② topic 由**进程内** static std::atomic<unsigned> serial 生成(同文件 :608-612),
#       两个并发实例各自从 0 起 ⇒ 序列逐一对齐, 必然同名;
#    ③ domain 固定 141(同文件 :74), 而 SHM 段名 = f(topic, domain)
#       (src/dzIPC/shm_pub_sub_ipc.cc:33,182) ⇒ 段名/TopicControl 段亦同名。
#    另有共享风险: chunk 池(每档 40 块)与 info_pool 注册表都是**全机共享**; 而 W08 的
#    断言含 `dzflat_fallback == 0` 与"该路径条数=N、另两条=0" ⇒ 邻居并发会造**假红/假绿**
#    (实测先例: test_ipc_info_pool 在 -j4 下报 3847≠4096 判 FAILED, 见 W00 §4.4/D-10)。
#    故用 RUN_SERIAL(不用 RESOURCE_LOCK —— 它只串行化同样声明该锁的用例, 挡不住未声明邻居)。
# TIMEOUT: 实测 5.81~5.86 s(5 次), 取 600 s 仅为死锁/挂起时报超时而非挂住构建机, 非性能判据。
# ================================================================
add_test(NAME test_w08_dzflat_ab COMMAND test_w08_dzflat_ab)
set_tests_properties(test_w08_dzflat_ab PROPERTIES
    TIMEOUT 600
    RUN_SERIAL TRUE
    ENVIRONMENT "W08_ARTIFACT_ROOT=${CMAKE_BINARY_DIR}/test-scratch/w08")

# ================================================================
# W09 chunk 容量 / 队列 / 背压联动 (docs/消息接收架构改造/团队改造交付/W09/容量与背压_交付.md)
# ----------------------------------------------------------------
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# 为什么必须进 CTest: 既有四个用例(test_chunk_hold / test_adopt_loan_quota /
# test_alloc_fault_inject / test_shm_ser_backpressure)覆盖的是**具体缺陷**,
# 而容量模型的判据没有一个常驻回路 —— 它们的失效方式全部静默:
#   ① 每档块数与"拒绝语义"若被改成阻塞/自动扩容, 功能用例照样全绿;
#   ② 池的**共享范围**(同 prefix 跨话题共享 / 不同 prefix 独立)读错不会报错;
#   ③ 尺寸档**双重取整**(loan 与 TLV 落不同档 ⇒ 同进程两个池)读错只会静默量错对象;
#   ④ 广播共享 chunk 若按订阅者重复计数, 容量模型会把余量少算 N 倍而无人发现;
#   ⑤ 「策略性回退」与「环覆写越界风险」混成一个"失败数" ⇒ 把内存安全问题当性能现象。
# 手工跑 build/bin/test_chunk_capacity_backpressure 进不了任何自动回路。
#
# 用例各自使用**独立 prefix** ⇒ 段名不同 ⇒ 物理上是不同的池, 不占用默认池的任何一档,
# 也不与并行跑的其它用例争抢(实测全套约 0.1s)。给 120s 只为死锁时报超时, 非性能判据。
# ================================================================
add_test(NAME test_chunk_capacity_backpressure COMMAND test_chunk_capacity_backpressure)
set_tests_properties(test_chunk_capacity_backpressure PROPERTIES TIMEOUT 120)

# ================================================================
# D-13：共享段用例 RUN_SERIAL 补登记（可机械判断规则）
# ----------------------------------------------------------------
# 【判据（机械可判）】一个已登记用例是否需要 RUN_SERIAL，取决于它是否触碰
#   **全机共享段**：
#     · chunk 池 `__IPC_SHM__CHUNK_INFO__<size>__C40`（**空前缀 = 全机共享**）；
#     · `dz_ipc_info_pool_v2`（进程级 4096 槽，但段是全机的）；
#     · 等待段 / 话题控制段（`__IPC_SHM__*_WAITER_*`、`__IPC_SHM__AC/CA/CONN_*`）。
#   判定命令（本案实际使用）：
#     for t in $(ctest -N | grep -oE 'test_[a-z0-9_]+'); do
#       grep -cE 'PublisherIPCPtrMake|SubscriberIPCPtrMake|shm::shm_(pub|sub)_ipc|IpcInfoPool|loan<|publish_loaned|RecvWorkerPool|SocketRecvWorkerPool|RouteSession|try_publish_dzflat' test/$t.cpp
#     done    # >0 ⇒ 触碰共享段 ⇒ 需 RUN_SERIAL
#
# ⛔ **反例声明（必须保留）**：RUN_SERIAL **只降低**并发串扰概率，**不能消除**因共享池
#    状态泄漏导致的失败。2026-09-28 实测（本机，25 项，`-j32` × 15 轮）：
#      · `test_dzflat_builder` **自己已是 RUN_SERIAL TRUE** 却 12/15 轮失败；
#      · `test_w08_dzflat_ab`（同为 RUN_SERIAL）11/15 轮失败；
#      · 同一断言：`chunk pool exhausted: kind=loan, chunk_size=132096, prefix=''`
#        （空前缀 = 全机共享）⇒ 失败源在**全机共享池状态**（含被 SIGKILL 进程遗留的
#        未发布借样：`reclaim_dead_chunks` 明确跳过 `published==0` 的块），**不在测试间调度**。
#      · 失败轮率 14/15（93%）。证据：build/test-scratch/t23_evidence/。
#    **根治归 t22（W09 容量/归因 + 异常退出回收）**；本块只做概率收敛，
#    使普通 `ctest -j4` 稳定（实测 6 轮 6/6 绿）。
#
# 名单（按上述机械判据 + 本轮实测受害者）：socket_recv_worker / lifecycle_contract /
#   socket_high_fd / shm_sub_dtor_gate / wakeup_artifact / shm_ser_backpressure /
#   shm_i5_pop_buffer / shm_ready_transition / shm_route_session / recv_worker /
#   udp_port_boundary / w03_measurement / dzflat_fallback_semantics（#26，新增于 21:2x，
#   同样消费默认共享池且断言 `fallback_total==0`，按同一机械判据纳入）。
# 不列入的例外（有据）：`test_shm_control_scheduler` / `test_recv_wait_set` /
#   `test_socket_wait_set` / `test_socket_readable` / `test_socket_ser_concurrency`
#   —— 机械判据计数为 0（不建共享段、不借 chunk、不用共享池）；
#   `test_chunk_capacity_backpressure` —— 各用例用**独立 prefix**（自有池，不占默认档）。
# ================================================================
set_tests_properties(
    test_socket_recv_worker
    test_lifecycle_contract
    test_socket_high_fd
    test_shm_sub_dtor_gate
    test_wakeup_artifact
    test_shm_ser_backpressure
    test_shm_i5_pop_buffer
    test_shm_ready_transition
    test_shm_route_session
    test_recv_worker
    test_udp_port_boundary
    test_w03_measurement
    PROPERTIES RUN_SERIAL TRUE)
# 注: `test_dzflat_fallback_semantics`（#26）同样命中本判据，但它的 add_test 位于本块**之后**，
#     CMake 要求先 add_test 再 set_tests_properties ⇒ 其 RUN_SERIAL 在其自身登记处单独设置。

# ================================================================
# W09/t19：DZFlat 发布尝试的**三种语义互不混算**（方案 §4 W09 第 5 条 + 队长裁决 D-16）
# (docs/消息接收架构改造/团队改造交付/W09/回退计数与三种语义分离.md)
# ----------------------------------------------------------------
# target 由上面的 file(GLOB) 自动生成, 无需 add_executable。命名规范同前。
#
# 为什么必须进 CTest: 判据全是**纯函数**层面的（classify_dzflat_attempt /
# note_dzflat_borrow_failed），毫秒级、无共享内存、无 flaky —— 而它守的失效方式静默:
#   ① `try_publish_dzflat()` 返回 bool, 从返回值看不出「路径选择 / 真回退 / B 借样失败」
#      ⇒ W08 的 tlv 档出现 `fallback_total=0` 与 `DzFlatFallbackCount=120` 并存的歧义;
#   ② 把「路径选择」计入 fallback ⇒ tlv 档凭空出现 N 次回退, W10 用假证据判规模;
#   ③ 把「B 借样失败」并进 fallback ⇒ fallback/(dzflat+fallback) 比值失真;
#   ④ 不可精确判定的原因若「猜一个」⇒ 报告给出不可复核的分解（本包强制落 *_reason_unknown）。
# 实测全套 0 ms（6 条）; 给 120s 只为异常时报超时, 非性能判据。
# ================================================================
# ================================================================
# `test_dzflat_rx` 登记（队长裁决，2026-09-28；Owner 转给架构负责人加行）
# ----------------------------------------------------------------
# 授权依据：W10 前置标定（验证与性能负责人）请求 + 队长书面批准。
# ⛔ **登记 ≠ D-6/D-14 已解决**：本行只把既有用例纳入自动回路，**不改变**
#    「共享池残留导致并发用例偶发失败」这一未关闭缺陷（见上方 D-13 块的反例声明）。
#
# 为什么最后才登记、且**不加 RUN_SERIAL**（三条实测理由均来自 W10 前置标定）：
#   ① 它确实在 build/bin 且可跑：3 次全 rc=0，单次约 1.7 s；
#   ② 有闸门价值：8 个用例覆盖「借样视图点进 chunk / 持样跨后续发布存活 /
#      TLV 不进视图路径 / DZFlat 只进视图路径 / prebuilt 段收发 / 无接收者返回 false」，
#      正是 W08 的 A/B 分流与 W06 接收侧语义的独立守门；
#   ③ **无副作用、且不进 132096 档**：不写仓库（无 ofstream/fopen/create_directories），
#      载荷最大 96×64×3 ≈ 18 KB（实测只落 `5120/6144/20480/58368/59392` 档），
#      故不受本轮 chunk 池陈旧段问题影响 ⇒ 无需 RUN_SERIAL。
# 复核（架构负责人）：单跑 8/8 PASSED（1.7 s）；与 builder/w08/socket_high_fd/recv_worker
#   并发 ×4 轮均 rc=0（余量足够）。
# 另注：它是**既有用例**（非本轮新增），此前一直只能手工跑。
# ================================================================
add_test(NAME test_dzflat_rx COMMAND test_dzflat_rx)
set_tests_properties(test_dzflat_rx PROPERTIES TIMEOUT 120)

add_test(NAME test_dzflat_fallback_semantics COMMAND test_dzflat_fallback_semantics)
# D-13（共享段用例 RUN_SERIAL 补登记）：本用例消费默认共享 chunk 池，且断言
# `fallback_total == 0` / 各 fallback_* 口径 —— 邻居把池吃干会造**假红**，
# 故与 D-13 名单同待遇。判据与反例声明见文件上方「D-13：共享段用例 RUN_SERIAL 补登记」块。
set_tests_properties(test_dzflat_fallback_semantics PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# ================================================================
# W10 验收工装纳入构建图（t40 / 队长派单；依据 R0-A1 硬闸 ③d、t37 的
# `团队改造交付/W10/R3_CMake登记需求.md`）
# ----------------------------------------------------------------
# 为什么必须进构建图（不是"顺手编一下"）：
#   `RecvWorkerPool::stats()` **按值返回** `RecvWorkerStats`，W06/t30 在结构体
#   **尾部追加**了 5 个 uint64。C++ 的 by-value 返回由**调用方**按它自己看到的
#   `sizeof` 分配返回槽 ⇒ **陈旧调用方（手工 `g++` 的工装）+ 新库 = 越界写**。
#   实测（t30 / t31-F4）：功能读数**全对**，退出时 `__libc_free ← Harness::run
#   ← main` ⇒ `rc=139`。⇒ 库一变更而未重编工装，就会产出**不可重放的 run**。
#   纳入 CMake 后「库变即重编」由**构建图**保证（根治）；t37 的
#   `test/perf/w10/build_harnesses.sh`（重编 + mtime 配对）与
#   `--require-lib-sha256/--require-bin-sha256`（运行时双指纹断言）**并存**，
#   作为不经 CMake 的直接调用路径上的**运行时兜底**。
#
# ⛔ 本块**只 add_executable，不 add_test**：工装是长跑规模验收（千路建连、
#    60 s+ 空闲窗口、故障注入），不是 CTest 用例。故本块**不适用** §4.4 的
#    ③b（共享段 RUN_SERIAL）与 ③c（自带落盘用例默认不得写仓库）——二者约束的是
#    **CTest 用例**；工装是**构建目标**，其证据落盘纪律由 t37 的
#    `w10_run_matrix.sh`（输出目录独占、拒绝覆盖、run_id 校验）单独承担。
#    ⛔ 不得据本块认为「W10 工装已进自动回路」——它们**不在** `ctest` 集内。
#
# 与 file(GLOB) 不冲突：上方 GLOB 是 `test/*.cpp`（**非递归**），不含 `test/perf/**`。
# 命名规范与既有显式块一致：target 名 = 源文件 basename。
# ================================================================
set(W10_HARNESSES
    w10_matrix w10_idle w10_scancost w10_threadscan w10_faults
    w10_wakescan w10_mixwake w10_tickcost w10_portconflict
    w10_portconflict_scope w10_rebuild_crash)
foreach(h ${W10_HARNESSES})
    add_executable(${h} ${LIBIPC_PROJECT_DIR}/test/perf/w10/${h}.cpp)
    # 编译选项与既有非调试目标同款（Release → -O2 + NDEBUG），保证与库同配置。
    target_compile_options(${h} PRIVATE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:MinSizeRel>:-O2>)
    target_compile_definitions(${h} PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
    # 链接形态按本文件既有显式块（`Threads::Threads`），而不是 t37 需求书里的裸
    # `pthread` —— 两者等价，但后者会在本项目的 CMake 版本告警。
    target_link_libraries(${h} ipc Threads::Threads)
    set_target_properties(${h} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    message(STATUS "Created W10 harness executable: ${h}")
endforeach()

# 纯对拍件：H1–H6/F4 的最小反例集合，**不链接 ipc**（刻意如此 —— 它必须能在
# 「库与工装不同步」时照常运行，才谈得上对照）。故不随库 ABI 变化而失效。
add_executable(w10_r3_counterexamples ${LIBIPC_PROJECT_DIR}/test/perf/w10/w10_r3_counterexamples.cpp)
target_compile_options(w10_r3_counterexamples PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>)
target_compile_definitions(w10_r3_counterexamples PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
set_target_properties(w10_r3_counterexamples PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created W10 harness executable: w10_r3_counterexamples (no libipc dependency)")

# ----------------------------------------------------------------
# W10-R2/R4b/R5 阶段新增工装（队长 t14 追加授权，2026-09-30：**显式清单，⛔ 不用 GLOB**）
# 所属任务号逐项记录，便于回溯：t42（R2 补齐 / §6.2 最小验证矩阵缺失四项）、
# t43（R4b 计数闭环复验）、t38（R5 扫描与三态复验）。
# ⛔ 仍只 add_executable / add_library，**不 add_test**（工装是构建目标，见上方归类声明）。
# ----------------------------------------------------------------
set(W10_HARNESSES_LATER
    w10_r2_interval        # t42
    w10_r2_minimal         # t42
    w10_r4b_firstfail      # t43
    w10_r4b_runtimetruth   # t43
    w10_r5_matrix          # t38
    w10_r5_minimal         # t38
    )
foreach(h ${W10_HARNESSES_LATER})
    add_executable(${h} ${LIBIPC_PROJECT_DIR}/test/perf/w10/${h}.cpp)
    target_compile_options(${h} PRIVATE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:MinSizeRel>:-O2>)
    target_compile_definitions(${h} PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
    target_link_libraries(${h} ipc Threads::Threads)
    set_target_properties(${h} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    message(STATUS "Created W10 harness executable: ${h} (later phase)")
endforeach()

# t42 的故障注入器：**共享库**（`LD_PRELOAD` 用），不是可执行文件 ⇒ 必须 SHARED。
# 它**不链接 ipc**（自行声明被抢占的符号，见该文件头注释），故不随库 ABI 变化。
add_library(force_backend_unavailable SHARED
            ${LIBIPC_PROJECT_DIR}/test/perf/w10/force_backend_unavailable.cpp)
target_compile_options(force_backend_unavailable PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>)
target_compile_definitions(force_backend_unavailable PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
# 输出名与 t42 驱动脚本 `test/perf/w10/w10_r2_driver.sh` 里写死的 `build/bin/libforce_backend.so`
# 保持一致 —— 否则脚本会去重新手工编译一份，构建图与实际加载的库再次分叉。
set_target_properties(force_backend_unavailable PROPERTIES
                      OUTPUT_NAME force_backend
                      PREFIX "lib"
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "Created W10 fault injector shared library: libforce_backend.so (t42)")

target_compile_definitions(test_shared_net_config PRIVATE
    DZIPC_TEST_SHARED_NET_BUILT=$<BOOL:${DZIPC_BUILD_SHARED_NET}>)
add_test(NAME test_shared_net_config COMMAND test_shared_net_config)
set_tests_properties(test_shared_net_config PROPERTIES LABELS shared_net TIMEOUT 30)
if(DZIPC_BUILD_SHARED_NET)
    foreach(name test_shared_net_wire test_shared_net_blob test_shared_net_local_protocol test_shared_net_endpoint test_shared_net_io_lifecycle test_shared_net_gateway_lock test_shared_net_client_lifecycle test_shared_net_outbox test_shared_net_outbox_failure test_shared_net_shm_bridge test_shared_net_local_direct test_shared_net_reassembly test_shared_net_dedup test_shared_net_quota)
        target_compile_definitions(${name} PRIVATE
            DZIPC_GOLDEN_VECTORS="${LIBIPC_PROJECT_DIR}/test/shared_net/golden_vectors.json")
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES LABELS shared_net TIMEOUT 30)
    endforeach()
    add_test(NAME test_shared_net_golden_vectors
        COMMAND ${Python3_EXECUTABLE} ${LIBIPC_PROJECT_DIR}/test/shared_net/generate_vectors.py --check)
    set_tests_properties(test_shared_net_golden_vectors PROPERTIES LABELS shared_net TIMEOUT 30)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_test(NAME test_shared_net_shm_capacity COMMAND test_shared_net_shm_capacity)
    set_tests_properties(test_shared_net_shm_capacity PROPERTIES LABELS shared_net TIMEOUT 30)
endif()

if(DZIPC_BUILD_SHARED_NET)
    target_compile_definitions(test_shared_net_gateway_lock PRIVATE DZIPC_GATEWAY_BIN="$<TARGET_FILE:dzipc_gateway>")
    add_dependencies(test_shared_net_gateway_lock dzipc_gateway)
endif()

if(DZIPC_BUILD_SHARED_NET)
    target_compile_definitions(test_shared_net_outbox_failure PRIVATE DZIPC_GATEWAY_BIN="$<TARGET_FILE:dzipc_gateway>")
    add_dependencies(test_shared_net_outbox_failure dzipc_gateway)
endif()
