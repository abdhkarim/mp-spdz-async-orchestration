# Exécutable de consensus centralisé simulé (décision du core set).
add_executable(consensus src/consensus.cpp)

target_include_directories(consensus
  PRIVATE
    # Headers locaux du module consensus.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(consensus
  PRIVATE
    # Réutilise les types partagés (common).
    common
)

target_compile_options(consensus PRIVATE -Wall -Wextra -Wpedantic)
