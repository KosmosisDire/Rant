# A throwaway project that consumes DART the way a user does, so CI notices when the
# consumption path breaks. Nothing here is installed or shipped.
#
# Default: the CPM and FetchContent path over this source tree.
# DART_CONSUMER_FIND_PACKAGE=ON: an installed DART found through its config package.

cmake_minimum_required(VERSION 3.15)
project(dart_consumer C)

option(DART_CONSUMER_FIND_PACKAGE "Consume an installed DART instead of this source tree" OFF)

if(DART_CONSUMER_FIND_PACKAGE)
  # CONFIG is required: below cmake 3.27 a case insensitive filesystem finds CMake's own
  # unrelated FindDart module first. See docs/building.md.
  find_package(dart CONFIG REQUIRED)
else()
  include(FetchContent)
  get_filename_component(DART_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  FetchContent_Declare(dart SOURCE_DIR "${DART_ROOT}")
  FetchContent_MakeAvailable(dart)
endif()

add_executable(dart_consumer main.c)
target_link_libraries(dart_consumer PRIVATE dart::dart_host)
