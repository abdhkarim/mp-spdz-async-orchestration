cmake_minimum_required(VERSION 3.20)

project(mp_spdz_async_orchestration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# Enable testing globally
enable_testing()

add_subdirectory(common)
add_subdirectory(consensus)
add_subdirectory(node)
add_subdirectory(spdz_bridge)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  # Only add tests subdirectory if the test source files exist
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_messages.cpp")
    add_subdirectory(tests)
  endif()
endif()