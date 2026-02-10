add_library(spdz_bridge STATIC
  src/playersdata.cpp
  src/spdz_paths.cpp
  src/compile_program.cpp
)

target_include_directories(spdz_bridge
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(spdz_bridge
  PUBLIC
    common
)

target_compile_options(spdz_bridge PRIVATE -Wall -Wextra -Wpedantic)