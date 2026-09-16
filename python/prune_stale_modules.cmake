# 删除 python/dzipc/ 下除 KEEP_NAME 之外的所有 _dzipc_core*.so。
#
# 由 python/CMakeLists.txt 的 POST_BUILD 步骤调用。存在的理由见那里的注释:
# 同一包目录里并存多个 Python 版本的模块时, 用错解释器就会静默加载到陈旧的那个,
# 而症状(DZFlat 消息读不出来)看起来完全像通道故障。
#
# 只删本次构建之外的同名模块; 其它文件一律不碰。这些 .so 被 python/.gitignore 忽略。
file(GLOB _stale "${PKG_DIR}/_dzipc_core*.so" "${PKG_DIR}/_dzipc_core*.pyd")
foreach(_f ${_stale})
    get_filename_component(_n "${_f}" NAME)
    if(NOT _n STREQUAL "${KEEP_NAME}")
        message(STATUS "移除陈旧的 Python 模块: ${_n}")
        file(REMOVE "${_f}")
    endif()
endforeach()
