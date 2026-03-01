add_library(common STATIC
  src/crypto.cpp
  src/serialize.cpp
  src/messages.cpp
)

target_include_directories(common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(common PRIVATE -Wall -Wextra -Wpedantic)
