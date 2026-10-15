# Everything under tests/: the wire tap, rant_bench, the test programs and every ctest entry.
# spec/testing.md says what each one covers.

# The POSIX feature macros must precede the first system header, and the wire tap includes
# the socket headers before the implementation does.
add_library(rant_test_flags INTERFACE)
if(NOT WIN32)
  target_compile_definitions(rant_test_flags INTERFACE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
endif()
target_link_libraries(rant_test_flags INTERFACE rant rant_platform)

add_library(rant_wire_tap STATIC support/wire_tap.c)
target_link_libraries(rant_wire_tap PUBLIC rant_test_flags)

# rant_bench ships in the release zip, so it lands in bin/ with the other programs.
add_executable(rant_bench
  bench/main.c bench/node.c bench/sweep.c bench/sendbench.c
  bench/memscale.c bench/threadbench.c bench/queuebench.c)
target_link_libraries(rant_bench PRIVATE rant_wire_tap)
add_dependencies(rant_bench rant_dist)

# Test executables stay in the build tree, out of bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_BINARY_DIR}/bin")
endforeach()

add_executable(rant_test rant_test.c)
target_link_libraries(rant_test PRIVATE rant_wire_tap)
target_include_directories(rant_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(rant_test rant_dist)

add_executable(rant_test_noshm rant_test.c)
target_link_libraries(rant_test_noshm PRIVATE rant_wire_tap)
target_include_directories(rant_test_noshm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(rant_test_noshm PRIVATE RANT_NO_SHM)
add_dependencies(rant_test_noshm rant_dist)

add_test(NAME rant_test       COMMAND rant_test selftest)
add_test(NAME rant_test_noshm COMMAND rant_test_noshm selftest)
if(TARGET rant_cpp_test)
  add_test(NAME rant_cpp_test COMMAND rant_cpp_test)
endif()
