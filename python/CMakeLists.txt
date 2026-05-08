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
# Unicode Support
add_definitions(-DUNICODE -D_UNICODE)

pybind11_add_module(dzipc MODULE src/interface.cc)
add_dependencies(dzipc generate_ipc_messages)

target_link_libraries(dzipc PRIVATE ipc)
target_include_directories(dzipc PRIVATE ${LIBIPC_PROJECT_DIR}/include)

target_compile_features(dzipc PRIVATE cxx_std_17)
if (MSVC)
    target_compile_options(dzipc PRIVATE /utf-8)
endif()
set_target_properties(dzipc PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python
)

if (NOT ROSBUILD)
    install(TARGETS dzipc
        LIBRARY DESTINATION lib/python
        RUNTIME DESTINATION lib/python
        ARCHIVE DESTINATION lib/python
    )
endif()
