cmake_minimum_required(VERSION 3.20)
project(replay_trader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.4 GIT_SHALLOW TRUE)
FetchContent_Declare(implot GIT_REPOSITORY https://github.com/epezent/implot.git GIT_TAG v0.16 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw imgui implot)

find_package(OpenGL REQUIRED)

add_executable(replay_trader
  src/main.cpp
  src/market.cpp
  src/broker.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp)
target_include_directories(replay_trader PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${implot_SOURCE_DIR})
target_compile_definitions(replay_trader PRIVATE GL_SILENCE_DEPRECATION
  RT_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/nq_1m.bin"
  RT_SESSIONS="${CMAKE_CURRENT_SOURCE_DIR}/sessions"
  RT_FONT="${CMAKE_CURRENT_SOURCE_DIR}/../strategy_report/pdf_src/fonts/BerkeleyMono-Regular.ttf")
target_link_libraries(replay_trader PRIVATE glfw OpenGL::GL)
