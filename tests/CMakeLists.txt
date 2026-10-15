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

# MSVC ASan programs load the runtime DLL beside the linker, which is not on PATH outside a
# developer prompt, so it goes next to every program.
if(MSVC AND CMAKE_C_FLAGS MATCHES "/fsanitize=address")
  get_filename_component(_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
  file(COPY "${_msvc_bin}/clang_rt.asan_dynamic-x86_64.dll"
       DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/bin")
  file(COPY "${_msvc_bin}/clang_rt.asan_dynamic-x86_64.dll" DESTINATION "${RANT_BIN_DIR}")
endif()

# Every ctest entry gets its own block of 64 domains through RANT_TEST_DOMAIN, so entries run
# in parallel. A build tree takes one of eight slots by its path, so two trees can run ctest
# at the same time.
string(MD5 _rant_tree "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_rant_tree}" 0 4 _rant_tree)
math(EXPR _rant_slot "0x${_rant_tree} % 8")
set_property(GLOBAL PROPERTY RANT_TEST_BLOCKS 0)
# rant_test_domain(<test> [<variable>]): the variable is RANT_TEST_DOMAIN unless named.
function(rant_test_domain test)
  set(_var RANT_TEST_DOMAIN)
  if(ARGC GREATER 1)
    set(_var ${ARGV1})
  endif()
  get_property(_block GLOBAL PROPERTY RANT_TEST_BLOCKS)
  if(_block GREATER_EQUAL 120)
    message(FATAL_ERROR "tests/CMakeLists.txt: more than 120 ctest entries share the domain slots")
  endif()
  math(EXPR _domain "1000 + (${_rant_slot} * 120 + ${_block}) * 64")
  math(EXPR _block "${_block} + 1")
  set_property(GLOBAL PROPERTY RANT_TEST_BLOCKS ${_block})
  set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "${_var}=${_domain}")
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
    target_compile_definitions(${_target} PRIVATE
      RANT_TEST_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/golden/schemas.txt")
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

# Compile checks: the implementation alone with every warning an error, C99 pedantic on GCC
# and Clang, /W4 on MSVC, in each feature combination a cut down build uses. The build.strict
# entry builds them all in one call, so an experiment never breaks the normal build and one
# build tree never runs two builds at once. embedded is the Yaskawa module's.
if(MSVC)
  set(_strict_c /W4 /WX /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS)
  set(_strict_cxx ${_strict_c})
else()
  set(_strict_c -std=c99 -pedantic-errors -Wall -Wextra -Wno-missing-field-initializers -Werror)
  set(_strict_cxx -pedantic-errors -Wall -Wextra -Wno-missing-field-initializers -Werror)
endif()
set(_strict_default "")
set(_strict_no_shm RANT_NO_SHM)
set(_strict_no_threads RANT_NO_THREADS)
set(_strict_embedded RANT_PLAT_CUSTOM RANT_NO_SHM RANT_NO_THREADS)
set(_strict_minimal RANT_NO_PATTERNS RANT_NO_STDTYPES)
set(_strict_targets)
foreach(_variant default no_shm no_threads embedded minimal cpp)
  if(_variant STREQUAL "cpp")
    if(NOT CMAKE_CXX_COMPILER)
      continue()
    endif()
    add_library(rant_strict_cpp OBJECT EXCLUDE_FROM_ALL support/strict.cpp)
    target_compile_features(rant_strict_cpp PRIVATE cxx_std_17)
    target_compile_options(rant_strict_cpp PRIVATE ${_strict_cxx})
  else()
    add_library(rant_strict_${_variant} OBJECT EXCLUDE_FROM_ALL support/strict.c)
    target_compile_options(rant_strict_${_variant} PRIVATE ${_strict_c})
    target_compile_definitions(rant_strict_${_variant} PRIVATE ${_strict_${_variant}})
    set_target_properties(rant_strict_${_variant} PROPERTIES C_EXTENSIONS OFF)
  endif()
  target_link_libraries(rant_strict_${_variant} PRIVATE rant)
  add_dependencies(rant_strict_${_variant} rant_dist)
  list(APPEND _strict_targets rant_strict_${_variant})
endforeach()
add_test(NAME build.strict COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
         --target ${_strict_targets} --config $<CONFIG>)
set_tests_properties(build.strict PROPERTIES LABELS build TIMEOUT 600)

# The steady state of a send must not allocate. memscale exits 1 when one did.
add_test(NAME bench.memscale COMMAND rant_bench memscale)
set_tests_properties(bench.memscale PROPERTIES LABELS bench TIMEOUT 120)

# The layout of every public struct a binding mirrors, written at build time for the layout
# legs of the C# and Python suites.
add_executable(rant_layout support/layout.c)
target_link_libraries(rant_layout PRIVATE rant)
add_dependencies(rant_layout rant_dist)
set(RANT_LAYOUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/layout.txt")
add_custom_command(OUTPUT "${RANT_LAYOUT_FILE}" COMMAND rant_layout "${RANT_LAYOUT_FILE}"
                   DEPENDS rant_layout VERBATIM)
add_custom_target(rant_layout_file ALL DEPENDS "${RANT_LAYOUT_FILE}")

# The binding suites, one entry each. Every leg runs and the result names the failed ones.
function(rant_binding_test test)
  set_tests_properties(${test} PROPERTIES LABELS bindings TIMEOUT 300)
  set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "RANT_LAYOUT=${RANT_LAYOUT_FILE}")
  rant_test_domain(${test})
endfunction()

if(CMAKE_CXX_COMPILER)
  rant_cpp_program(rant_cpp_test "${PROJECT_SOURCE_DIR}/bindings/cpp/test.cpp")
  target_compile_definitions(rant_cpp_test PRIVATE
    RANT_TEST_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/golden/schemas.txt")
  add_test(NAME bindings.cpp COMMAND rant_cpp_test)
  rant_binding_test(bindings.cpp)
endif()

# Python and C# load the shared library the build copies into dist/native.
if(TARGET rant_shared AND RANT_STAGE_NATIVE)
  set(Python3_FIND_REGISTRY LAST)       # the Python a shell runs, as the wheel targets do
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME bindings.python
             COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/bindings/python/test.py")
    rant_binding_test(bindings.python)
  endif()

  # dotnet builds the test only when ctest runs, as a fixture, never in the normal build.
  find_program(DOTNET_EXECUTABLE dotnet)
  if(DOTNET_EXECUTABLE)
    set(_csharp_out "${CMAKE_CURRENT_BINARY_DIR}/csharp")
    add_test(NAME bindings.csharp.build
             COMMAND "${DOTNET_EXECUTABLE}" build "${PROJECT_SOURCE_DIR}/bindings/csharp/test"
                     -c Release -o "${_csharp_out}" --nologo -v q)
    set_tests_properties(bindings.csharp.build PROPERTIES FIXTURES_SETUP csharp
                         LABELS bindings TIMEOUT 300)
    add_test(NAME bindings.csharp COMMAND "${DOTNET_EXECUTABLE}" "${_csharp_out}/rant_csharp_test.dll")
    set_tests_properties(bindings.csharp PROPERTIES FIXTURES_REQUIRED csharp)
    rant_binding_test(bindings.csharp)
  endif()
endif()

# Interop: every language's tick program publishes to every other one's, through
# rant_interop_pair, on the domain RANT_DOMAIN names. C# builds as a fixture like its suite.
add_executable(rant_interop_pair interop/pair.c)
target_link_libraries(rant_interop_pair PRIVATE rant_test_flags)
add_executable(rant_tick_c interop/tick.c)
target_link_libraries(rant_tick_c PRIVATE rant_host)
if(NOT WIN32)
  target_link_libraries(rant_tick_c PRIVATE m)
endif()
# the node name each language's publisher takes, which its subscriber checks
set(_tick_name_c c-tick-pub)
set(_tick_name_cpp cpp-tick-pub)
set(_tick_name_python py-tick-pub)
set(_tick_name_csharp cs-tick-pub)
set(_tick_langs c)
set(_tick_c "$<TARGET_FILE:rant_tick_c>")
if(CMAKE_CXX_COMPILER)
  rant_cpp_program(rant_tick_cpp "${CMAKE_CURRENT_SOURCE_DIR}/interop/tick.cpp")
  list(APPEND _tick_langs cpp)
  set(_tick_cpp "$<TARGET_FILE:rant_tick_cpp>")
endif()
if(TARGET rant_shared AND RANT_STAGE_NATIVE AND Python3_Interpreter_FOUND)
  list(APPEND _tick_langs python)
  set(_tick_python "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/interop/tick.py")
endif()
if(TARGET rant_shared AND RANT_STAGE_NATIVE AND DOTNET_EXECUTABLE)
  list(APPEND _tick_langs csharp)
  set(_tick_csharp "${DOTNET_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/interop_csharp/rant_tick.dll")
  add_test(NAME interop.csharp.build
           COMMAND "${DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/interop/csharp"
                   -c Release -o "${CMAKE_CURRENT_BINARY_DIR}/interop_csharp" --nologo -v q)
  set_tests_properties(interop.csharp.build PROPERTIES FIXTURES_SETUP interop_csharp
                       LABELS interop TIMEOUT 300)
endif()
foreach(_pub ${_tick_langs})
  foreach(_sub ${_tick_langs})
    if(NOT _pub STREQUAL _sub)
      set(_test interop.${_pub}_to_${_sub})
      add_test(NAME ${_test} COMMAND rant_interop_pair ${_tick_${_pub}} pub 30 --
                                     ${_tick_${_sub}} sub 20 ${_tick_name_${_pub}})
      set_tests_properties(${_test} PROPERTIES LABELS interop TIMEOUT 90)
      if(_pub STREQUAL "csharp" OR _sub STREQUAL "csharp")
        set_tests_properties(${_test} PROPERTIES FIXTURES_REQUIRED interop_csharp)
      endif()
      rant_test_domain(${_test} RANT_DOMAIN)
    endif()
  endforeach()
endforeach()
