project(dzipc_pub)

add_executable(dzipc_pub src/main.cc)

target_include_directories(dzipc_pub
    PRIVATE
        ${LIBIPC_PROJECT_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # shared CLI helpers (argparser.h, msg_type_identify.h)
        ${LIBIPC_PROJECT_DIR}/exec/dzipc_topic_cat/include)

target_compile_definitions(dzipc_pub
    PRIVATE
        DZIPC_MSG_DIR="${LIBIPC_PROJECT_DIR}/msg")

target_link_libraries(dzipc_pub
    PRIVATE
        ipc
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:pthread>
        $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)

if (MSVC)
    target_compile_options(dzipc_pub PRIVATE /utf-8 /wd4251)
endif()

set_target_properties(dzipc_pub PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${APP_OUTPUT_PATH}
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")

if (WIN32 AND LIBIPC_BUILD_SHARED_LIBS)
    add_custom_command(TARGET dzipc_pub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ipc>
            $<TARGET_FILE_DIR:dzipc_pub>
    )
endif()
