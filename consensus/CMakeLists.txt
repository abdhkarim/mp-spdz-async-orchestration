# Exécutable de consensus centralisé simulé (décision du core set).
add_executable(consensus
  src/consensus.cpp
  src/type_proof.cpp
)
add_executable(ack_crypto_tool src/ack_crypto_tool.cpp)
add_executable(share_verifier src/share_verifier.cpp)

target_include_directories(consensus
  PRIVATE
    # Headers locaux du module consensus.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(consensus
  PRIVATE
    # Réutilise les types partagés (common).
    common
    # Vérification des preuves cryptographiques BLAKE2b.
    PkgConfig::SODIUM
)

target_compile_options(consensus PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(ack_crypto_tool PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(share_verifier PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(ack_crypto_tool
  PRIVATE
    PkgConfig::SODIUM
)

target_include_directories(share_verifier
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(share_verifier
  PRIVATE
    PkgConfig::SODIUM
)
