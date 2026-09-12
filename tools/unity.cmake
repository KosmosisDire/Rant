# The Unity package. Assembles dist/com.rant.dart/, which Unity adds from disk and the
# release pushes to the upm branch, and dist/dart-<version>.unitypackage, from
# csharp/Dart.cs, csharp/unity/Runtime/ and the libraries in dist/native/. Unity's
# immutable package cache synthesizes no .meta files, so every entry gets one here, with a
# GUID that is the md5 of its path so an upgrade keeps references (spec/bindings.md).
#   cmake -P tools/unity.cmake        (or the unity_package target)

cmake_minimum_required(VERSION 3.18)

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(STRINGS "${ROOT}/VERSION" VERSION LIMIT_COUNT 1)
set(PKG com.rant.dart)
set(OUT "${ROOT}/dist/${PKG}")
set(SRC "${ROOT}/csharp/unity")

# LF whatever the host, so the upm branch is identical from every machine.
function(write_lf f text)
  file(CONFIGURE OUTPUT "${f}" CONTENT "${text}" @ONLY NEWLINE_STYLE LF)
endfunction()

set(META_FOLDER "folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData:
")
set(META_CS "MonoImporter:
  externalObjects: {}
  serializedVersion: 2
  defaultReferences: []
  executionOrder: 0
  icon: {instanceID: 0}
  userData:
")
set(META_ASMDEF "AssemblyDefinitionImporter:
  externalObjects: {}
  userData:
")
set(META_PACKAGE "PackageManifestImporter:
  externalObjects: {}
  userData:
")
set(META_TEXT "TextScriptImporter:
  externalObjects: {}
  userData:
")

# A native plugin enabled for exactly one platform: the Editor on that OS and its
# standalone player. Three libraries share the name dart, so nothing may say Any.
function(plugin_meta out os)
  set(cpu_win None)
  set(cpu_linux None)
  set(cpu_osx None)
  if(os STREQUAL "Windows")
    set(cpu x86_64)
    set(cpu_win x86_64)
  elseif(os STREQUAL "Linux")
    set(cpu x86_64)
    set(cpu_linux x86_64)
  else()
    set(cpu AnyCPU)
    set(cpu_osx AnyCPU)
  endif()
  foreach(p win linux osx)
    if(cpu_${p} STREQUAL "None")
      set(on_${p} 0)
    else()
      set(on_${p} 1)
    endif()
  endforeach()
  set(${out} "PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: ${cpu}
        DefaultValueInitialized: true
        OS: ${os}
  - first:
      Standalone: Linux64
    second:
      enabled: ${on_linux}
      settings:
        CPU: ${cpu_linux}
  - first:
      Standalone: OSXUniversal
    second:
      enabled: ${on_osx}
      settings:
        CPU: ${cpu_osx}
  - first:
      Standalone: Win64
    second:
      enabled: ${on_win}
      settings:
        CPU: ${cpu_win}
  userData:
" PARENT_SCOPE)
endfunction()

# The importer for one entry of the package, by what it is and where it sits.
function(meta_body out rel)
  if(IS_DIRECTORY "${OUT}/${rel}")
    set(body "${META_FOLDER}")
  elseif(rel STREQUAL "package.json")
    set(body "${META_PACKAGE}")
  elseif(rel MATCHES "[.]cs$")
    set(body "${META_CS}")
  elseif(rel MATCHES "[.]asmdef$")
    set(body "${META_ASMDEF}")
  elseif(rel MATCHES "Plugins/win-")
    plugin_meta(body Windows)
  elseif(rel MATCHES "Plugins/linux-")
    plugin_meta(body Linux)
  elseif(rel MATCHES "Plugins/osx")
    plugin_meta(body OSX)
  else()
    set(body "${META_TEXT}")
  endif()
  set(${out} "${body}" PARENT_SCOPE)
endfunction()

# 1. The package tree.
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/Runtime/Plugins")
write_lf("${OUT}/package.json" "{
  \"name\": \"${PKG}\",
  \"version\": \"${VERSION}\",
  \"displayName\": \"DART\",
  \"description\": \"Discovery And Realtime Transport: peer discovery over UDP multicast plus reliable realtime UDP pub/sub, with typed messages. Desktop standalone (Windows, macOS, Linux) and the Editor.\",
  \"unity\": \"2021.3\",
  \"keywords\": [\"networking\", \"pubsub\", \"udp\", \"discovery\", \"realtime\", \"middleware\", \"dart\", \"rant\"],
  \"author\": { \"name\": \"Nathan George\" }
}
")
file(COPY "${SRC}/README.md" DESTINATION "${OUT}")
file(GLOB runtime_files "${SRC}/Runtime/*")
file(COPY "${ROOT}/csharp/Dart.cs" ${runtime_files} DESTINATION "${OUT}/Runtime")

# Unity targets these three desktops. win-arm64 and linux-arm64 would collide on the
# library name, and no standalone player exists for them.
foreach(entry "win-x64/dart.dll" "linux-x64/libdart.so" "osx/libdart.dylib")
  if(EXISTS "${ROOT}/dist/native/${entry}")
    get_filename_component(rid "${entry}" DIRECTORY)
    file(COPY "${ROOT}/dist/native/${entry}" DESTINATION "${OUT}/Runtime/Plugins/${rid}")
  else()
    message(STATUS "unity: no dist/native/${entry}, that platform gets no plugin")
  endif()
endforeach()

# 2. A .meta beside every file and folder. The package root needs none.
file(GLOB_RECURSE entries LIST_DIRECTORIES true RELATIVE "${OUT}" "${OUT}/*")
foreach(rel IN LISTS entries)
  string(MD5 guid "${PKG}/${rel}")
  meta_body(body "${rel}")
  write_lf("${OUT}/${rel}.meta" "fileFormatVersion: 2\nguid: ${guid}\n${body}")
endforeach()
message(STATUS "wrote ${OUT}")

# 3. The .unitypackage: a gzipped tar of one folder per asset holding its path, its
#    bytes and its .meta, imported under Assets/Dart.
set(WORK "${ROOT}/build/unitypackage")
file(REMOVE_RECURSE "${WORK}")
file(GLOB_RECURSE assets LIST_DIRECTORIES true RELATIVE "${OUT}/Runtime" "${OUT}/Runtime/*")
list(FILTER assets EXCLUDE REGEX "[.]meta$")
set(paths "Assets/Dart")
foreach(rel IN LISTS assets)
  list(APPEND paths "Assets/Dart/${rel}")
endforeach()
foreach(apath IN LISTS paths)
  string(REGEX REPLACE "^Assets/Dart" "Runtime" rel "${apath}")
  string(MD5 guid "${apath}")
  file(MAKE_DIRECTORY "${WORK}/${guid}")
  file(WRITE "${WORK}/${guid}/pathname" "${apath}")
  if(NOT IS_DIRECTORY "${OUT}/${rel}")
    configure_file("${OUT}/${rel}" "${WORK}/${guid}/asset" COPYONLY)
  endif()
  meta_body(body "${rel}")
  write_lf("${WORK}/${guid}/asset.meta" "fileFormatVersion: 2\nguid: ${guid}\n${body}")
endforeach()
file(GLOB guid_dirs RELATIVE "${WORK}" "${WORK}/*")
set(UNITYPACKAGE "${ROOT}/dist/dart-${VERSION}.unitypackage")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${UNITYPACKAGE}" ${guid_dirs}
                WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "unity: tar failed (${rc})")
endif()
message(STATUS "wrote ${UNITYPACKAGE}")
