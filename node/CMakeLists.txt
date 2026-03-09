# Exécutable de simulation d'un fournisseur de données.
add_executable(data_provider src/data_provider.cpp)

target_include_directories(data_provider
  PRIVATE
    # Headers locaux du module node.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(data_provider
  PRIVATE
    # Accès aux types/messages partagés.
    common
    # Hash BLAKE2b pour la preuve cryptographique.
    PkgConfig::SODIUM
)

target_compile_options(data_provider PRIVATE -Wall -Wextra -Wpedantic)
# ============================================================================
# Tests for data_provider
# ============================================================================

add_executable(test_data_provider src/test_data_provider.cpp)

target_include_directories(test_data_provider
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_data_provider
  PRIVATE
    PkgConfig::SODIUM
)

target_compile_options(test_data_provider PRIVATE -Wall -Wextra -Wpedantic)

# Register tests with CTest
add_test(NAME data_provider_tests COMMAND test_data_provider)