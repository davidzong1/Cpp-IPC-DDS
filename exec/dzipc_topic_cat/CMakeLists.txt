project(dzipc_topic_cat)

add_executable(dzipc_topic_cat src/shm_sniffer.cc src/main.cc src/socket_sniffer.cc)

target_include_directories(dzipc_topic_cat
    PRIVATE
        ${LIBIPC_PROJECT_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(dzipc_topic_cat
    PRIVATE
        ipc
        $<$<NOT:$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>:pthread>
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)

set_target_properties(dzipc_topic_cat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
