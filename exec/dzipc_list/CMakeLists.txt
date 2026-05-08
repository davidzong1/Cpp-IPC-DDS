project(dzipc_list)

add_executable(dzipc_list main.cc)

target_include_directories(dzipc_list
    PRIVATE
        ${LIBIPC_PROJECT_DIR}/include)

target_link_libraries(dzipc_list
    PRIVATE
        ipc
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:pthread>
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)

if (MSVC)
    target_compile_options(dzipc_list PRIVATE /utf-8)
endif()

set_target_properties(dzipc_list PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${APP_OUTPUT_PATH})

if (WIN32 AND LIBIPC_BUILD_SHARED_LIBS)
    add_custom_command(TARGET dzipc_list POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ipc>
            $<TARGET_FILE_DIR:dzipc_list>
    )
endif()
