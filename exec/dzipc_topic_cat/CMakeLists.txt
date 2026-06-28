project(dzipc_topic_cat)

add_executable(dzipc_topic_cat src/shm_sniffer.cc src/main.cc src/socket_sniffer.cc)

target_include_directories(dzipc_topic_cat
    PRIVATE
        ${LIBIPC_PROJECT_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(dzipc_topic_cat
    PRIVATE
        ipc
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:pthread>
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)

if (MSVC)
    target_compile_options(dzipc_topic_cat PRIVATE /utf-8 /wd4251)
endif()

set_target_properties(dzipc_topic_cat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${APP_OUTPUT_PATH}
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")

if (WIN32 AND LIBIPC_BUILD_SHARED_LIBS)
    add_custom_command(TARGET dzipc_topic_cat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ipc>
            $<TARGET_FILE_DIR:dzipc_topic_cat>
    )
endif()