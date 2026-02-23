add_executable(mpc_node
  src/main.cpp
  src/node.cpp
  src/transport_http.cpp
  src/prephase.cpp
  src/share_store.cpp
  src/crash_sim.cpp
  src/mp_spdz_runner.cpp
)

target_include_directories(mpc_node
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mpc_node
  PRIVATE
    common
)

target_compile_options(mpc_node PRIVATE -Wall -Wextra -Wpedantic)