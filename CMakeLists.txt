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
if(APPLE)
  enable_language(OBJC)    # src/mac_input.m
  enable_language(OBJCXX)  # src/mac_metal.mm, ImGui's Metal backend
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(RT_DIST)  # no build-machine paths (assert messages use __FILE__) in a binary that gets shared
  add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=build" "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=src")
endif()

# Dear ImGui (UI), GLFW (window, input, OpenGL context) and the Inter font: MIT / zlib / OFL, fetched at
# exact versions. The chart is drawn with ImGui's draw lists; --screenshot renders on the CPU (src/softraster.cpp).
include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_Declare(inter URL https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip
  URL_HASH SHA256=ff970a5d4561a04f102a7cb781adbd6ac4e9b6c460914c7a101f15acb7f7d1a4 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glfw imgui inter)
add_library(imgui_static STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)
target_include_directories(imgui_static PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_static PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS "ImDrawIdx=unsigned int")
target_link_libraries(imgui_static PUBLIC glfw)
if(APPLE)  # macOS renders with Metal
  target_sources(imgui_static PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
  set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(imgui_static PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
endif()

# Lua 5.4 for indicator and strategy scripts
FetchContent_Declare(lua URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
  URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
if(UNIX)
  target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)
endif()

# Built-in scripts, the scripting reference and the Inter font, compiled in (src/embedded_files.h)
file(GLOB_RECURSE RT_SCRIPTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.lua)
set(RT_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)
add_custom_command(OUTPUT ${RT_EMBEDDED}
  COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/scripts -DOUT=${RT_EMBEDDED}
          "-DEXTRA=README.md=${CMAKE_CURRENT_SOURCE_DIR}/docs/SCRIPTING.md|fonts/Inter-Regular.ttf=${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf|fonts/Inter-SemiBold.ttf=${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
  DEPENDS ${RT_SCRIPTS} ${CMAKE_CURRENT_SOURCE_DIR}/docs/SCRIPTING.md ${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
  COMMENT "Embedding scripts and fonts" VERBATIM)

set(RT_SOURCES
  ${RT_EMBEDDED}
  src/script.cpp
  src/indworker.cpp
  src/strategy.cpp
  src/main.cpp
  src/softraster.cpp
  src/panels.cpp
  src/setup.cpp
  src/chart.cpp
  src/app.cpp
  src/market.cpp
  src/broker.cpp
  src/datafeed.cpp
  src/http.cpp)
if(APPLE)
  list(APPEND RT_SOURCES src/mac_input.m src/mac_metal.mm)
  set_source_files_properties(src/mac_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()

if(RT_DIST AND APPLE)
  add_executable(replay_trader MACOSX_BUNDLE ${RT_SOURCES})
  set_target_properties(replay_trader PROPERTIES
    OUTPUT_NAME "Replay Trader"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Replay Trader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.groverburger.replay-trader"
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

target_include_directories(replay_trader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(replay_trader PRIVATE GL_SILENCE_DEPRECATION)  # macOS: OpenGL is the fallback renderer

if(NOT RT_DIST)  # the developer's chart font, if it's on this machine (it isn't redistributable)
  set(RT_FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../strategy_report/pdf_src/fonts/BerkeleyMono-Regular.ttf")
  if(EXISTS ${RT_FONT_FILE})
    target_compile_definitions(replay_trader PRIVATE RT_FONT="${RT_FONT_FILE}")
  endif()
endif()

if(WIN32)
  target_link_libraries(replay_trader PRIVATE imgui_static lua_static opengl32 winhttp shell32)
  if(MINGW)  # no libstdc++/libgcc/winpthread DLLs to ship
    target_link_options(replay_trader PRIVATE -static -static-libgcc -static-libstdc++ $<$<BOOL:${RT_DIST}>:-s>)
  endif()
else()
  find_package(CURL REQUIRED)  # part of macOS; libcurl4-openssl-dev on Debian/Ubuntu
  find_package(OpenGL REQUIRED)
  target_link_libraries(replay_trader PRIVATE imgui_static lua_static OpenGL::GL CURL::libcurl)
  if(APPLE)
    target_link_libraries(replay_trader PRIVATE "-framework Cocoa")
  endif()
endif()
