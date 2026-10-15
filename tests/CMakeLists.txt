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

# Every ctest entry gets its own block of 64 domains through RANT_TEST_DOMAIN, so entries run
# in parallel. A build tree takes one of eight slots by its path, so two trees can run ctest
# at the same time.
string(MD5 _rant_tree "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_rant_tree}" 0 4 _rant_tree)
math(EXPR _rant_slot "0x${_rant_tree} % 8")
set_property(GLOBAL PROPERTY RANT_TEST_BLOCKS 0)
function(rant_test_domain test)
  get_property(_block GLOBAL PROPERTY RANT_TEST_BLOCKS)
  if(_block GREATER_EQUAL 120)
    message(FATAL_ERROR "tests/CMakeLists.txt: more than 120 ctest entries share the domain slots")
  endif()
  math(EXPR _domain "1000 + (${_rant_slot} * 120 + ${_block}) * 64")
  math(EXPR _block "${_block} + 1")
  set_property(GLOBAL PROPERTY RANT_TEST_BLOCKS ${_block})
  set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "RANT_TEST_DOMAIN=${_domain}")
endfunction()

# rant_c_test(<kind> <name> [SHM_ONLY]) builds <kind>/<name>.c as ctest <kind>.<name>, and
# again with RANT_NO_SHM as <kind>.<name>.noshm unless the program needs shared memory.
function(rant_c_test kind name)
  cmake_parse_arguments(ARG "SHM_ONLY" "" "" ${ARGN})
  set(_flavours default)
  if(NOT ARG_SHM_ONLY)
    list(APPEND _flavours noshm)
  endif()
  foreach(_flavour ${_flavours})
    set(_target ${kind}_${name})
    set(_test ${kind}.${name})
    if(_flavour STREQUAL "noshm")
      set(_target ${_target}_noshm)
      set(_test ${_test}.noshm)
    endif()
    add_executable(${_target} ${kind}/${name}.c support/harness.c)
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${_target} PRIVATE rant_wire_tap)
    add_dependencies(${_target} rant_dist)
    if(_flavour STREQUAL "noshm")
      target_compile_definitions(${_target} PRIVATE RANT_NO_SHM)
    endif()
    add_test(NAME ${_test} COMMAND ${_target})
    set(_labels ${kind})
    if(_flavour STREQUAL "noshm")
      list(APPEND _labels noshm)
    endif()
    if(kind STREQUAL "unit")
      set(_timeout 60)
    else()
      set(_timeout 300)
    endif()
    set_tests_properties(${_test} PROPERTIES LABELS "${_labels}" TIMEOUT ${_timeout}
                         SKIP_RETURN_CODE 77)
    rant_test_domain(${_test})
  endforeach()
endfunction()

# Sans IO: no sockets, a controlled clock, milliseconds per program.
rant_c_test(unit transport)
rant_c_test(unit discovery)
rant_c_test(unit shm SHM_ONLY)
rant_c_test(unit schema)
rant_c_test(unit detail)

# Real nodes on loopback.
rant_c_test(e2e stream)
rant_c_test(e2e node)
rant_c_test(e2e schema)
rant_c_test(e2e detail)
rant_c_test(e2e shm SHM_ONLY)
rant_c_test(e2e queues)
rant_c_test(e2e patterns)
rant_c_test(e2e tasks)
rant_c_test(e2e entities)
rant_c_test(e2e matchwait)
rant_c_test(e2e relay)
rant_c_test(e2e timestamps)
rant_c_test(e2e threads)

# The steady state of a send must not allocate. memscale exits 1 when one did.
add_test(NAME bench.memscale COMMAND rant_bench memscale)
set_tests_properties(bench.memscale PROPERTIES LABELS bench TIMEOUT 120)

if(TARGET rant_cpp_test)
  add_test(NAME rant_cpp_test COMMAND rant_cpp_test)
endif()
