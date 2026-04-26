cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

cmake_policy(SET CMP0091 NEW)

set(TARGET bf42plusplus)
project(${TARGET} VERSION 1.0 LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

# GetAsyncKeyState is detected as keylogging behaviour by AVs
option(REDUCE_AV_SCORE "Remove functionality suspicious to AVs" ON)


add_library(${TARGET} SHARED)

target_sources( ${TARGET} PRIVATE
  src/bfhook.cpp
  src/bf/console.cpp
  src/bf/gameevent.cpp
  src/bf/generic.cpp
  src/bf/net.cpp
  src/bf/object.cpp
  src/bf/renderer.cpp
  src/bf/stl.cpp
  src/bf/ui.cpp

  src/debug.cpp
  src/dllmain.cpp
  src/hooks.c
  src/settings.cpp
  src/updater.cpp
  src/util.cpp

  src/blake2b/blake2b-ref.c

  res/bf42plus.def
  res/bf42plus.rc
)


set_target_properties( ${TARGET} PROPERTIES
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17

  CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20

  RUNTIME_OUTPUT_NAME "bf42++"
)


target_compile_definitions( ${TARGET} PUBLIC
  UNICODE
  _UNICODE
)


if (${REDUCE_AV_SCORE})
  target_compile_definitions( ${TARGET} PUBLIC
    REDUCE_AV_SCORE
  )
endif()


target_compile_options( ${TARGET} PUBLIC
  /Zc:threadSafeInit

  /wd4584 # multiple inheritance warnings
#  /wd4731 # frame pointer modified in inline assembly

#  /wd5105 # defined in .vcxproj
#  /wd4201 # defined in .vcxproj
)


target_link_libraries( ${TARGET} PUBLIC
  kernel32
  user32
  shell32
  Shlwapi
)


include(GNUInstallDirs)
target_include_directories( ${TARGET} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/fpng>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/stb>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/simpleini>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/libsodium/include>
)


add_executable( bf42plusplus_loader WIN32
  src/loader.cpp
  src/hooks.c
)

set_target_properties( bf42plusplus_loader PROPERTIES
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17

  CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20

  RUNTIME_OUTPUT_NAME "bf42++"
)

target_compile_definitions( bf42plusplus_loader PUBLIC
  UNICODE
  _UNICODE
)

target_include_directories( bf42plusplus_loader PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/simpleini>
)


add_subdirectory(BlackScreen)
