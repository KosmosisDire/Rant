# The Arduino library: dist/arduino/Rant/ and dist/rant-<version>-arduino.zip, which Arduino and
# PlatformIO take as they are. rant.cpp is its one implementation file, as rant.c beside it would emit Rant twice.

cmake_minimum_required(VERSION 3.18)

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(STRINGS "${ROOT}/VERSION" VERSION LIMIT_COUNT 1)
set(STAGE "${ROOT}/dist/arduino")
set(OUT "${STAGE}/Rant")

file(REMOVE_RECURSE "${STAGE}")
foreach(f rant.h rant.hpp rant_types.h rant_types.hpp rant.cpp)
  if(NOT EXISTS "${ROOT}/dist/${f}")
    message(FATAL_ERROR "arduino: no dist/${f}, build rant_dist first")
  endif()
  file(COPY "${ROOT}/dist/${f}" DESTINATION "${OUT}/src")
endforeach()
file(COPY "${ROOT}/LICENCE.md" DESTINATION "${OUT}")
file(CONFIGURE OUTPUT "${OUT}/library.properties" CONTENT "name=Rant
version=@VERSION@
author=KosmosisDire
maintainer=KosmosisDire
sentence=Nodes that find each other on the network and share topics, variables, functions and tasks.
paragraph=The Rant middleware for ESP32 boards. Include rant.hpp for C++ or rant.h for C.
category=Communication
url=https://github.com/KosmosisDire/Rant
architectures=esp32
includes=rant.hpp
" @ONLY NEWLINE_STYLE LF)

set(ZIP "${ROOT}/dist/rant-${VERSION}-arduino.zip")
file(REMOVE "${ZIP}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${ZIP}" --format=zip Rant
  WORKING_DIRECTORY "${STAGE}" RESULT_VARIABLE failed)
if(failed)
  message(FATAL_ERROR "arduino: cannot write ${ZIP}")
endif()
message(STATUS "arduino: ${ZIP}")
