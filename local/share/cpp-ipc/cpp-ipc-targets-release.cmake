#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cpp-ipc::ipc" for configuration "Release"
set_property(TARGET cpp-ipc::ipc APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(cpp-ipc::ipc PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libipc.so.1.3.0"
  IMPORTED_SONAME_RELEASE "libipc.so.3"
  )

list(APPEND _IMPORT_CHECK_TARGETS cpp-ipc::ipc )
list(APPEND _IMPORT_CHECK_FILES_FOR_cpp-ipc::ipc "${_IMPORT_PREFIX}/lib/libipc.so.1.3.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
