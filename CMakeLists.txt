cmake_minimum_required(VERSION 3.20)

# RT_DIST=ON: the shareable build. macOS: "Replay Trader.app", universal (arm64 + x86_64), no paths from this
# machine baked in. Windows: one static .exe with no console window. See README "Sharing".
option(RT_DIST "Build the shareable app (bundle / static exe, no local font)" OFF)
if(RT_DIST AND APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(replay_trader VERSION 1.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(RT_DIST)  # no build-machine paths (assert messages use __FILE__) in a binary that gets shared
  add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=build" "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=src")
endif()

# BUI (Blender's UI as a C library): only the C kit, no bindings, tests or examples.
include(FetchContent)
set(BUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUI_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
set(BUI_BUILD_IMGUI OFF CACHE BOOL "" FORCE)
set(BUI_BUILD_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(bui GIT_REPOSITORY https://github.com/ppmpreetham/BUI.git GIT_TAG v0.5.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(bui)

set(RT_SOURCES
  src/main.cpp
  src/panels.cpp
  src/setup.cpp
  src/chart.cpp
  src/app.cpp
  src/market.cpp
  src/broker.cpp
  src/datafeed.cpp
  src/http.cpp)

if(RT_DIST AND APPLE)
  add_executable(replay_trader MACOSX_BUNDLE ${RT_SOURCES})
  set_target_properties(replay_trader PROPERTIES
    OUTPUT_NAME "Replay Trader"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Replay Trader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.teamsafi.replay-trader"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
elseif(RT_DIST AND WIN32)
  add_executable(replay_trader WIN32 ${RT_SOURCES})  # GUI subsystem: no console window
  set_target_properties(replay_trader PROPERTIES OUTPUT_NAME "ReplayTrader")
  if(MSVC)
    target_link_options(replay_trader PRIVATE /ENTRY:mainCRTStartup)
  endif()
else()
  add_executable(replay_trader ${RT_SOURCES})
endif()

if(NOT RT_DIST)  # the developer's chart font, if it's on this machine (it isn't redistributable)
  set(RT_FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../strategy_report/pdf_src/fonts/BerkeleyMono-Regular.ttf")
  if(EXISTS ${RT_FONT_FILE})
    target_compile_definitions(replay_trader PRIVATE RT_FONT="${RT_FONT_FILE}")
  endif()
endif()

if(WIN32)
  target_link_libraries(replay_trader PRIVATE bui::kit winhttp)
  if(MINGW)  # no libstdc++/libgcc/winpthread DLLs to ship
    target_link_options(replay_trader PRIVATE -static -static-libgcc -static-libstdc++ $<$<BOOL:${RT_DIST}>:-s>)
  endif()
else()
  find_package(CURL REQUIRED)  # part of macOS; libcurl4-openssl-dev on Debian/Ubuntu
  target_link_libraries(replay_trader PRIVATE bui::kit CURL::libcurl)
endif()
