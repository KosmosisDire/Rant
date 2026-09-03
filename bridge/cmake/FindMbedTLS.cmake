# MbedTLS is fetched and built as part of this tree (see ../CMakeLists.txt), never
# looked up on the system. libdatachannel wants the MbedTLS::MbedTLS / MbedCrypto /
# MbedX509 imported targets and libSRTP the MBEDTLS_INCLUDE_DIRS / MBEDTLS_LIBRARIES
# variables: both are mapped onto the fetched targets here. First on CMAKE_MODULE_PATH,
# so it shadows the finders both projects ship.
if(NOT TARGET mbedtls)
  message(FATAL_ERROR "FindMbedTLS: the fetched mbedtls target is missing (fetch it first)")
endif()
foreach(_pair "MbedTLS;mbedtls" "MbedCrypto;mbedcrypto" "MbedX509;mbedx509")
  list(GET _pair 0 _name)
  list(GET _pair 1 _target)
  if(NOT TARGET MbedTLS::${_name})
    add_library(MbedTLS::${_name} INTERFACE IMPORTED)
    target_link_libraries(MbedTLS::${_name} INTERFACE ${_target})
  endif()
endforeach()
get_target_property(MBEDTLS_INCLUDE_DIRS mbedtls INTERFACE_INCLUDE_DIRECTORIES)
set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
set(MbedTLS_FOUND TRUE)
set(MbedTLS_VERSION "3.6.4")
