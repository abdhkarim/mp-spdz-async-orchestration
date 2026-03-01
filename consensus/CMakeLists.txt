add_executable(consensus src/consensus.cpp)

target_include_directories(consensus
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(consensus
  PRIVATE
    common
)

target_compile_options(consensus PRIVATE -Wall -Wextra -Wpedantic)
