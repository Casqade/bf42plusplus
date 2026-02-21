cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

cmake_policy(SET CMP0091 NEW)

set(TARGET bf42plus)
project(${TARGET} VERSION 1.0 LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
option(AUTOUPDATER_ENABLED "Enable autoupdater" ON)


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

  src/crashreport.cpp
  src/debug.cpp
  src/dllmain.cpp
  src/hooks.c
#  src/pch.cpp
  src/settings.cpp
  src/updater.cpp
  src/util.cpp

  res/bf42plus.def
)


set_target_properties( ${TARGET} PROPERTIES
  C_STANDARD_REQUIRED ON
  CXX_STANDARD 17

  CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20
)


target_compile_definitions( ${TARGET} PUBLIC
  UNICODE
  _UNICODE
)


target_compile_options( ${TARGET} PUBLIC
  /Zc:threadSafeInit

  /wd4584 # multiple inheritance warnings
#  /wd4731 # frame pointer modified in inline assembly

#  /wd5105 # defined in .vcxproj
#  /wd4201 # defined in .vcxproj
)


if (${AUTOUPDATER_ENABLED})
  target_compile_definitions( ${TARGET} PUBLIC
    AUTOUPDATER_ENABLED
  )
endif()

target_link_directories( ${TARGET} PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/libs/libsodium
)


target_link_libraries( ${TARGET} PUBLIC
  kernel32
  user32
  winhttp
  shell32
  Shlwapi
  libsodium
)


include(GNUInstallDirs)
target_include_directories( ${TARGET} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/fpng>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/stb>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/simpleini>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libs/libsodium/include>
)
