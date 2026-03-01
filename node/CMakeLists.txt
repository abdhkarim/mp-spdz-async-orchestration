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
)

target_compile_options(data_provider PRIVATE -Wall -Wextra -Wpedantic)
