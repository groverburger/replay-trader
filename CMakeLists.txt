cmake_minimum_required(VERSION 3.20)
project(replay_trader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
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

add_executable(replay_trader
  src/main.cpp
  src/panels.cpp
  src/chart.cpp
  src/app.cpp
  src/market.cpp
  src/broker.cpp)
target_compile_definitions(replay_trader PRIVATE
  RT_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/nq_1m.bin"
  RT_SESSIONS="${CMAKE_CURRENT_SOURCE_DIR}/sessions"
  RT_FONT="${CMAKE_CURRENT_SOURCE_DIR}/../strategy_report/pdf_src/fonts/BerkeleyMono-Regular.ttf")
target_link_libraries(replay_trader PRIVATE bui::kit)
