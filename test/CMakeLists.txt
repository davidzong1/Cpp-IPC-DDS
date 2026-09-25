project(test-ipc)

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
file(GLOB HEAD_FILES ${LIBIPC_PROJECT_DIR}/test/*.h)

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

# 用例内含 4 线程 churn 与 250ms 慢回调, 实测全套约 3s; 给 120s 只为在死锁
# (本用例要抓的失败模式之一: 注销等待在途回调时自锁) 时让 ctest 报超时而非永久
# 阻塞构建机, 不是性能判据。
set_tests_properties(test_shm_control_scheduler PROPERTIES TIMEOUT 120)

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
