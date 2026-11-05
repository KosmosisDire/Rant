# A throwaway project that consumes Rant the way a user does, so CI notices when the
# consumption path breaks. Nothing here is installed or shipped.
#
# Default: the CPM and FetchContent path over this source tree.
# RANT_CONSUMER_FIND_PACKAGE=ON: an installed Rant found through its config package.

cmake_minimum_required(VERSION 3.15)
project(rant_consumer C)

option(RANT_CONSUMER_FIND_PACKAGE "Consume an installed Rant instead of this source tree" OFF)

if(RANT_CONSUMER_FIND_PACKAGE)
  find_package(rant CONFIG REQUIRED)
else()
  include(FetchContent)
  get_filename_component(RANT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  FetchContent_Declare(rant SOURCE_DIR "${RANT_ROOT}")
  FetchContent_MakeAvailable(rant)
endif()

add_executable(rant_consumer main.c)
target_link_libraries(rant_consumer PRIVATE rant::rant_host)
