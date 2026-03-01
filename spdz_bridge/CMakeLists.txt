add_executable(spdz_bridge src/spdz_bridge.cpp)

target_include_directories(spdz_bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(spdz_bridge
  PRIVATE
    common
)

target_compile_options(spdz_bridge PRIVATE -Wall -Wextra -Wpedantic)
