# A throwaway project that consumes Ramble the way a user does, so CI notices when the
# consumption path breaks. Nothing here is installed or shipped.
#
# Default: the CPM and FetchContent path over this source tree.
# RAMBLE_CONSUMER_FIND_PACKAGE=ON: an installed Ramble found through its config package.

cmake_minimum_required(VERSION 3.15)
project(ramble_consumer C)

option(RAMBLE_CONSUMER_FIND_PACKAGE "Consume an installed Ramble instead of this source tree" OFF)

if(RAMBLE_CONSUMER_FIND_PACKAGE)
  find_package(ramble CONFIG REQUIRED)
else()
  include(FetchContent)
  get_filename_component(RAMBLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  FetchContent_Declare(ramble SOURCE_DIR "${RAMBLE_ROOT}")
  FetchContent_MakeAvailable(ramble)
endif()

add_executable(ramble_consumer main.c)
target_link_libraries(ramble_consumer PRIVATE ramble::ramble_host)
