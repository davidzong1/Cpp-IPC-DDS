cmake_minimum_required(VERSION 3.10)
project(cpp-ipc)

# 标准安装目录(Linux: bin/lib/include; Windows: bin/lib/include 相对 CMAKE_INSTALL_PREFIX)
include(GNUInstallDirs)

# 生成消息头文件
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(generate_ipc_messages)
    message(STATUS "运行消息生成器...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} generator/batch_msg_srv_generator.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    
    if(NOT result)
        message(STATUS "消息生成成功")
    endif()
endfunction()
add_custom_target(generate_ipc_messages ALL
    COMMAND ${Python3_EXECUTABLE} generator/batch_msg_srv_generator.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "运行消息生成器..."
    VERBATIM
)
set_target_properties(generate_ipc_messages PROPERTIES
    EXCLUDE_FROM_ALL FALSE
    EXCLUDE_FROM_DEFAULT_BUILD FALSE
)


option(ROSBUILD                 "Build with ROS2 support."                              OFF)
option(LIBIPC_BUILD_TESTS       "Build all of libipc's own tests."                      ON)
option(ENABLE_DEBUG_INFO        "Enable debug info "                                    OFF)
option(LIBIPC_BUILD_DEMOS       "Build all of libipc's own demos."                      OFF)
option(LIBIPC_BUILD_SHARED_LIBS "Build shared libraries (DLLs)."                        ON)
option(LIBIPC_USE_STATIC_CRT    "Set to ON to build with static CRT on Windows (/MT)."  OFF)
option(OPEN_DEBUG_MODE          "Enable debug mode in PROJECT."                         OFF)
option(LIBIPC_BUILD_PYTHON      "Build pybind11 Python module dzipc_cpp."               ON)
option(UPDATA_MSG_SRV_GENERATOR    "Update msg/srv generator scripts."                  ON)


set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG")

if (UPDATA_MSG_SRV_GENERATOR)
    generate_ipc_messages()
endif()

if (OPEN_DEBUG_MODE)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        # 调试模式(Debug)下强制使用 -O0 编译并保留调试信息
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
    else()
        # 非调试模式下使用 -O2 优化
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
    endif()
endif()

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

set(LIBRARY_OUTPUT_PATH    ${CMAKE_BINARY_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(LIBIPC_PROJECT_DIR     ${PROJECT_SOURCE_DIR})
set(APP_OUTPUT_PATH        ${CMAKE_BINARY_DIR}/app)

# Unicode Support
add_definitions(-DUNICODE -D_UNICODE)

# ROS2构建支持
if(ROSBUILD)
    find_package(ament_cmake REQUIRED)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_subdirectory(src)

if (LIBIPC_BUILD_PYTHON)
    add_subdirectory(python)
endif()

add_subdirectory(exec/dzipc_list)
add_subdirectory(exec/dzipc_topic_cat)

if (LIBIPC_BUILD_TESTS)
    set(GOOGLETEST_VERSION 1.10.0)
    if (LIBIPC_USE_STATIC_CRT)
        set(gtest_force_shared_crt OFF)
    else()
        set(gtest_force_shared_crt ON)
    endif()
    if(ROSBUILD)
    cmake_policy(SET CMP0148 NEW)
    endif()
    add_subdirectory(3rdparty/gtest)
    add_subdirectory(test)
endif()

if (LIBIPC_BUILD_DEMOS)
    add_subdirectory(demo/chat)
    add_subdirectory(demo/msg_que)
    add_subdirectory(demo/send_recv)
    if (MSVC)
        add_subdirectory(demo/win_service/service)
        add_subdirectory(demo/win_service/client)
    else()
        add_subdirectory(demo/linux_service/service)
        add_subdirectory(demo/linux_service/client)
    endif()
endif()

if(NOT ROSBUILD)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS dzipc_topic_cat dzipc_list
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(EXPORT cpp-ipc-targets
        FILE cpp-ipc-targets.cmake
        NAMESPACE cpp-ipc::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake/cmake_uninstall.cmake)
else()
    # 安装头文件
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/
    )

    # 只有当ipc目标存在时才处理导出
    if(TARGET ipc)
        install(TARGETS ipc
            EXPORT ipc-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        install(EXPORT ipc-targets
            FILE ipc-targets.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
        )
        install(TARGETS dzipc_topic_cat
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        install(TARGETS dzipc_list
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
        ament_export_targets(ipc-targets HAS_LIBRARY_TARGET)
        ament_export_libraries(ipc)
    endif()

    ament_export_dependencies()
    ament_export_include_directories(include)
    ament_package()
endif()
