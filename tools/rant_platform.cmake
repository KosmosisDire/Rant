# The OS libraries a desktop Rant build links, written once. The root build includes this,
# and so does a standalone Rant Web configure, which has no root to inherit.

if(TARGET rant_platform)
  return()
endif()

add_library(rant_platform INTERFACE)
add_library(rant::rant_platform ALIAS rant_platform)

find_package(Threads REQUIRED)
target_link_libraries(rant_platform INTERFACE Threads::Threads)

if(WIN32)
  # ws2_32: sockets   bcrypt: entropy   winmm: timeBeginPeriod (rant_test)
  target_link_libraries(rant_platform INTERFACE ws2_32 bcrypt winmm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(rant_platform INTERFACE rt)     # shm_open for the SHM fast path
endif()
