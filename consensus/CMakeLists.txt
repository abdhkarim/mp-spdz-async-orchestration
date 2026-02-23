add_executable(consensus main.cpp)

target_include_directories(common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SODIUM_INCLUDE_DIRS}
)

target_link_directories(common PUBLIC ${SODIUM_LIBRARY_DIRS})
target_link_libraries(common
  PUBLIC
    nlohmann_json::nlohmann_json
    ${SODIUM_LIBRARIES}
)

# Options de compilation "safe"
target_compile_options(common PRIVATE -Wall -Wextra -Wpedantic)
