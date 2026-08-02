project(ipc)

set (PACKAGE_VERSION 1.3.0)

if(ROSBUILD)
find_package(ament_cmake REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

aux_source_directory(${LIBIPC_PROJECT_DIR}/src/libipc SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/libipc/sync SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/libipc/platform SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/libipc/socket SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/dzIPC SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/dzIPC/common SRC_FILES)
aux_source_directory(${LIBIPC_PROJECT_DIR}/src/dzIPC/logger SRC_FILES)

file(GLOB HEAD_FILES
  ${LIBIPC_PROJECT_DIR}/include/libipc/*.h
  ${LIBIPC_PROJECT_DIR}/src/libipc/*.h
  ${LIBIPC_PROJECT_DIR}/src/libipc/*.inc
  ${LIBIPC_PROJECT_DIR}/src/libipc/circ/*.h
  ${LIBIPC_PROJECT_DIR}/src/libipc/memory/*.h
  ${LIBIPC_PROJECT_DIR}/src/libipc/platform/*.h
  ${LIBIPC_PROJECT_DIR}/src/libipc/utility/*.h
  )
include_directories(${LIBIPC_PROJECT_DIR}/include)

if (LIBIPC_BUILD_SHARED_LIBS)
  add_library(${PROJECT_NAME} SHARED ${SRC_FILES} ${HEAD_FILES})
  target_compile_definitions(${PROJECT_NAME}
    INTERFACE
      LIBIPC_LIBRARY_SHARED_USING__
    PRIVATE
      LIBIPC_LIBRARY_SHARED_BUILDING__)
else()
  add_library(${PROJECT_NAME} STATIC ${SRC_FILES} ${HEAD_FILES})
endif()

# 如果强制开启，直接为目标添加调试选项；否则按配置添加（Debug 配置时生效）
if(ENABLE_DEBUG_INFO)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -g -O0)
    # 链接器通常不需要额外 debug flag，但可根据需要添加
  elseif (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /Zi /Od)
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG)
  endif()
else()
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
      $<$<CONFIG:Debug>:-g -O0>
      $<$<CONFIG:RelWithDebInfo>:-g -O2>
      $<$<CONFIG:Release>:-O3>
    )
  elseif (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
      $<$<CONFIG:Debug>:/Zi /Od>
      $<$<CONFIG:Release>:/O2>
    )
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
  endif()
endif()
# set output directory
set_target_properties(${PROJECT_NAME}
	PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# set version
set_target_properties(${PROJECT_NAME}
    PROPERTIES
    VERSION ${PACKAGE_VERSION}
    SOVERSION 3
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")

target_include_directories(${PROJECT_NAME}
  PUBLIC
    "$<BUILD_INTERFACE:${LIBIPC_PROJECT_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE ${LIBIPC_PROJECT_DIR}/src
          $<$<BOOL:${UNIX}>:${LIBIPC_PROJECT_DIR}/src/libipc/platform/linux>)

if(MSVC)
  target_link_libraries(${PROJECT_NAME} PUBLIC ws2_32)
else()
  target_link_libraries(${PROJECT_NAME} PUBLIC
    $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:pthread>
    $<$<NOT:$<OR:$<STREQUAL:${CMAKE_SYSTEM_NAME},Windows>,$<STREQUAL:${CMAKE_SYSTEM_NAME},QNX>>>:rt>)
endif()


if(ROSBUILD)
  install(TARGETS ${PROJECT_NAME}
  EXPORT cpp-ipc-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    # RUNTIME DESTINATION bin
  )
  # 生成卸载脚本并添加 uninstall target
  # configure_file(
  #   "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
  #   "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
  #   IMMEDIATE @ONLY)
  # add_custom_target(uninstall
  #   COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

else()
  install(
    TARGETS ${PROJECT_NAME}
    EXPORT cpp-ipc-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  install(EXPORT cpp-ipc-targets
    FILE cpp-ipc-targets.cmake
    NAMESPACE cpp-ipc::
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cpp-ipc
  )

  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake.in"
  [[include(CMakeFindDependencyMacro)
  include("${CMAKE_CURRENT_LIST_DIR}/cpp-ipc-targets.cmake")
  ]])
  configure_file("${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake" @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cpp-ipc-config.cmake DESTINATION ${CMAKE_INSTALL_DATADIR}/cpp-ipc)

  include(CMakePackageConfigHelpers)
  write_basic_package_version_file(
    cppIpcConfigVersion.cmake
    VERSION ${PACKAGE_VERSION}
    COMPATIBILITY AnyNewerVersion
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cppIpcConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_DATADIR}/cpp-ipc)

endif()