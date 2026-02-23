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



# On colle ensemble : Ton Nœud + Ton Pont + Common + OpenSSL + Les Threads



target_compile_options(mpc_node PRIVATE -Wall -Wextra -Wpedantic)

find_package(OpenSSL REQUIRED)
# On demande à CMake de chercher Boost
find_package(Boost REQUIRED COMPONENTS system filesystem)


find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system filesystem)

# On indique où se trouve la grosse librairie pré-compilée de MP-SPDZ
set(SPDZ_LIB ${CMAKE_SOURCE_DIR}/third_party/MP-SPDZ/libSPDZ.so)

target_link_libraries(mpc_node PUBLIC 
    spdz_bridge 
    common 
    ${SPDZ_LIB}   # <--- La magie est ici !
    OpenSSL::SSL 
    OpenSSL::Crypto 
    Boost::system 
    Boost::filesystem 
    pthread 
    gmp 
    gmpxx 
    sodium
)