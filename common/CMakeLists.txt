# Bibliothèque utilitaire partagée entre les exécutables du prototype.
add_library(common STATIC
  src/crypto.cpp
  src/serialize.cpp
  src/messages.cpp
)

target_include_directories(common
  PUBLIC
    # Expose les headers de common aux autres modules.
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Niveau d'avertissements strict pour garder un code pédagogique propre.
target_compile_options(common PRIVATE -Wall -Wextra -Wpedantic)
