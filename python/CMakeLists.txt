cmake_minimum_required(VERSION 3.10)

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
if (MSVC)
    set(CompilerFlags
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
        )
    if (LIBIPC_USE_STATIC_CRT)
        foreach(CompilerFlag ${CompilerFlags})
            string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
            string(REPLACE "/MDd" "/MTd" ${CompilerFlag} "${${CompilerFlag}}")
        endforeach()
    else()
        foreach(CompilerFlag ${CompilerFlags})
            string(REPLACE "/MT" "/MD" ${CompilerFlag} "${${CompilerFlag}}")
            string(REPLACE "/MTd" "/MDd" ${CompilerFlag} "${${CompilerFlag}}")
        endforeach()
    endif()
endif()

# Prefer pybind11 discovered from current Python environment when available.
execute_process(
    COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE PYBIND11_CMAKE_DIR_RESULT
)
if (PYBIND11_CMAKE_DIR_RESULT EQUAL 0 AND EXISTS "${PYBIND11_CMAKE_DIR}")
    list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
endif()

find_package(pybind11 CONFIG REQUIRED)
if (Python3_VERSION VERSION_GREATER_EQUAL "3.11" AND pybind11_VERSION VERSION_LESS "2.10")
    message(FATAL_ERROR
        "Python ${Python3_VERSION} requires pybind11 >= 2.10 for this binding; "
        "found pybind11 ${pybind11_VERSION}. Install pybind11 in the selected "
        "Python environment or pass -Dpybind11_DIR=<path>.")
endif()
message(STATUS "Building _dzipc_core with Python ${Python3_VERSION} and pybind11 ${pybind11_VERSION}")

# Unicode Support
add_definitions(-DUNICODE -D_UNICODE)

pybind11_add_module(_dzipc_core MODULE src/interface.cc)
add_dependencies(_dzipc_core generate_ipc_messages)

target_link_libraries(_dzipc_core PRIVATE ipc)
target_include_directories(_dzipc_core PRIVATE ${LIBIPC_PROJECT_DIR}/include)

target_compile_features(_dzipc_core PRIVATE cxx_std_17)
if (MSVC)
    target_compile_options(_dzipc_core PRIVATE /utf-8)
endif()
set_target_properties(_dzipc_core PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python
)

# 构建完直接放进源码树的包目录。
#
# 为什么必须有这一步: python/dzipc/ 是 `import dzipc` 实际加载的包(纯 Python 部分就在
# 那里), 但构建产物落在 ${CMAKE_BINARY_DIR}/python, install 目标又是 lib/python/dzipc
# —— 两者都不是它。于是那里的 .so 只能靠手工拷贝维持, 必然陈旧。
#
# 陈旧的后果极具误导性: `import dzipc` 照常成功、纯 Python 的 schema 注册表照常加载,
# 只是缺了新增的 C++ 绑定, 于是 DZFlat 消息全部读不出来, 症状看起来像"通道不通"。
# 详见 docs/dzflat_known_issues.md 第 1 条。
add_custom_command(TARGET _dzipc_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:_dzipc_core>
            ${LIBIPC_PROJECT_DIR}/python/dzipc/$<TARGET_FILE_NAME:_dzipc_core>
    COMMENT "同步 _dzipc_core 到 python/dzipc/ (供源码树内直接 import)"
    VERBATIM
)

# 清掉**其他 Python 版本**的陈旧模块。
#
# 同一个包目录里若同时躺着 cpython-310/311/312 三个 .so, 用哪个解释器就加载哪个 ——
# 而只有本次构建的那个是新的。这些文件被 python/.gitignore 忽略, 属本地产物, 删掉无副作用;
# 真要多版本共存, 应当每个版本各建一棵构建树并各自跑一次本步骤。
add_custom_command(TARGET _dzipc_core POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DPKG_DIR=${LIBIPC_PROJECT_DIR}/python/dzipc
            -DKEEP_NAME=$<TARGET_FILE_NAME:_dzipc_core>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/prune_stale_modules.cmake
    COMMENT "清理 python/dzipc/ 下其他 Python 版本的陈旧 _dzipc_core"
    VERBATIM
)

if (NOT ROSBUILD)
    install(TARGETS _dzipc_core
        LIBRARY DESTINATION lib/python/dzipc
        RUNTIME DESTINATION lib/python/dzipc
        ARCHIVE DESTINATION lib/python/dzipc
    )
endif()
