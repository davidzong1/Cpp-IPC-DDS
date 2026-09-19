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

# 第 7 个用例 PreviouslyFatalTopicNowConnectsEndToEnd 起真 pub/sub: 1.5s 等握手 + 2.5s
# 发布, 正常约 5s; 组播不可用时它 GTEST_SKIP 而不是挂住。给 120s 只为卡死时 ctest 报
# 超时而非永久阻塞, 不是性能判据。
set_tests_properties(test_udp_port_boundary PROPERTIES TIMEOUT 120)

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
