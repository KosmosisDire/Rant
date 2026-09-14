# The OS libraries a desktop Ramble build links, written once. The root build includes this,
# and so does a standalone explore/ or bridge/ configure, which has no root to inherit.

if(TARGET ramble_platform)
  return()
endif()

add_library(ramble_platform INTERFACE)
add_library(ramble::ramble_platform ALIAS ramble_platform)

find_package(Threads REQUIRED)
target_link_libraries(ramble_platform INTERFACE Threads::Threads)

if(WIN32)
  # ws2_32: sockets   bcrypt: entropy   winmm: timeBeginPeriod (ramble_test)
  target_link_libraries(ramble_platform INTERFACE ws2_32 bcrypt winmm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(ramble_platform INTERFACE rt)   # shm_open for the SHM fast path
endif()
