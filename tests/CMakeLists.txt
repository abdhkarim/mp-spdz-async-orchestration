# Cible de tests unitaires (désactivée par défaut au niveau racine).
add_executable(unit_tests
  test_messages.cpp
  test_crypto.cpp
)

# Les tests consomment l'API commune.
target_link_libraries(unit_tests PRIVATE common)

# Enregistrement CTest.
add_test(NAME unit_tests COMMAND unit_tests)
