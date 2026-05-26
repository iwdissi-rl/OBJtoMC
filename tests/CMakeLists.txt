add_executable(test_toolchain test_toolchain.cpp)
target_link_libraries(test_toolchain PRIVATE ZLIB::ZLIB)
add_test(NAME toolchain_smoke COMMAND test_toolchain)
