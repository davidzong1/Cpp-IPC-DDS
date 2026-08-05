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
    ${LIBIPC_PROJECT_DIR})

file(GLOB SRC_FILES
    ${LIBIPC_PROJECT_DIR}/test/*.cpp
    # ${LIBIPC_PROJECT_DIR}/test/profiler/*.cpp
    )
# Exclude benchmark from auto-test list (has its own main, not a gtest)
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/ipc_benchmark.cpp")
list(REMOVE_ITEM SRC_FILES "${LIBIPC_PROJECT_DIR}/test/dzipc_perf_benchmark.cpp")
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