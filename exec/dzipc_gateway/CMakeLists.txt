add_executable(dzipc_gateway src/main.cc)
target_link_libraries(dzipc_gateway PRIVATE ipc)
set_target_properties(dzipc_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
install(TARGETS dzipc_gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
