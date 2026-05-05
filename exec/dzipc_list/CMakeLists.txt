project(dzipc_list)

add_executable(dzipc_list main.cc)

target_include_directories(dzipc_list
    PRIVATE
        ${LIBIPC_PROJECT_DIR}/include)

target_link_libraries(dzipc_list
    PRIVATE
        ipc
        $<$<NOT:$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>:pthread>
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)

set_target_properties(dzipc_list PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
