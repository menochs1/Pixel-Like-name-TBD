cmake_minimum_required(VERSION 3.20)
project(PixelSandbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PX_BUILD_APP "Build the SDL2 sandbox app (needs SDL2)" ON)
option(PX_BUILD_TESTS "Build unit tests" ON)
option(PX_BUILD_TOOLS "Build headless tools (benchmark)" ON)

if(MSVC)
  set(PX_WARNINGS /W4)
else()
  set(PX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# --- Core simulation + world generation: no external dependencies, no rendering.
add_library(px_core
  src/core/material.cpp
  src/core/world.cpp
  src/core/simulator.cpp
  src/world/demo_scene.cpp
)
target_include_directories(px_core PUBLIC src)
target_compile_options(px_core PRIVATE ${PX_WARNINGS})

# --- Rendering: turns a World into an ARGB pixel buffer. Still no SDL.
add_library(px_render src/render/pixel_renderer.cpp)
target_link_libraries(px_render PUBLIC px_core)
target_compile_options(px_render PRIVATE ${PX_WARNINGS})

if(PX_BUILD_APP)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    add_executable(pixel_sandbox src/app/main.cpp)
    target_link_libraries(pixel_sandbox PRIVATE px_render)
    if(TARGET SDL2::SDL2)
      target_link_libraries(pixel_sandbox PRIVATE SDL2::SDL2)
      if(TARGET SDL2::SDL2main)
        target_link_libraries(pixel_sandbox PRIVATE SDL2::SDL2main)
      endif()
    else()
      target_include_directories(pixel_sandbox PRIVATE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(pixel_sandbox PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_compile_options(pixel_sandbox PRIVATE ${PX_WARNINGS})
  else()
    message(WARNING "SDL2 not found: skipping pixel_sandbox. Install SDL2 dev files or set -DPX_BUILD_APP=OFF.")
  endif()
endif()

if(PX_BUILD_TOOLS)
  add_executable(pixel_bench tools/bench.cpp)
  target_link_libraries(pixel_bench PRIVATE px_render)
  target_compile_options(pixel_bench PRIVATE ${PX_WARNINGS})
endif()

if(PX_BUILD_TESTS)
  enable_testing()
  add_executable(px_tests tests/test_main.cpp tests/test_simulation.cpp)
  target_link_libraries(px_tests PRIVATE px_core)
  target_compile_options(px_tests PRIVATE ${PX_WARNINGS})
  add_test(NAME px_tests COMMAND px_tests)
endif()
