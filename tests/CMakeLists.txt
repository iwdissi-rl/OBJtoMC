add_executable(test_toolchain test_toolchain.cpp)
target_link_libraries(test_toolchain PRIVATE ZLIB::ZLIB)
add_test(NAME toolchain_smoke COMMAND test_toolchain)

add_executable(test_nbt_worked_example test_nbt_worked_example.cpp)
target_link_libraries(test_nbt_worked_example PRIVATE nbt)
add_test(NAME nbt_worked_example COMMAND test_nbt_worked_example)

# Independent Python oracle (twoolie/NBT, pinned 1.5.1).  Parses the C++
# writer's --dump output back to a logical tree and asserts it matches the
# proposal §5 contract.  See tests/oracle/test_nbt_worked_example.py.
add_test(
    NAME nbt_python_oracle
    COMMAND ${CMAKE_COMMAND} -E env
        PATH_TO_BINARY=$<TARGET_FILE:test_nbt_worked_example>
        ${CMAKE_SOURCE_DIR}/tests/oracle_venv/bin/python
        ${CMAKE_SOURCE_DIR}/tests/oracle/test_nbt_worked_example.py
)
set_tests_properties(nbt_python_oracle PROPERTIES LABELS "oracle")
