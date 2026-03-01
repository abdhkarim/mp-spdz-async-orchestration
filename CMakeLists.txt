cmake_minimum_required(VERSION 3.20)

project(mp_spdz_async_orchestration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" OFF)

add_subdirectory(common)
add_subdirectory(consensus)
add_subdirectory(node)
add_subdirectory(spdz_bridge)

if (BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests)
endif()
