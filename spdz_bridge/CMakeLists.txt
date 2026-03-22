# Exécutable passerelle entre le consensus local et l'exécution MP-SPDZ.
add_executable(spdz_bridge
  src/spdz_bridge.cpp
  src/semi2k_prep.cpp       # Async-externalized semi2k preprocessing
)

target_include_directories(spdz_bridge
  PRIVATE
    # Headers locaux du module bridge (includes semi2k_prep.hpp).
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(spdz_bridge
  PRIVATE
    # Accès aux types partagés.
    common
    # libsodium: used by semi2k_prep for cryptographic randomness (randombytes_buf).
    PkgConfig::SODIUM
)

target_compile_options(spdz_bridge PRIVATE -Wall -Wextra -Wpedantic)
