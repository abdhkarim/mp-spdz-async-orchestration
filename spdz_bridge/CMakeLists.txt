# Exécutable passerelle entre le consensus local et l'exécution MP-SPDZ.
add_executable(spdz_bridge src/spdz_bridge.cpp)

target_include_directories(spdz_bridge
  PRIVATE
    # Headers locaux du module bridge.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(spdz_bridge
  PRIVATE
    # Accès aux types partagés si le bridge est enrichi plus tard.
    common
)

target_compile_options(spdz_bridge PRIVATE -Wall -Wextra -Wpedantic)
