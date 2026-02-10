add_executable(unit_tests
  test_messages.cpp
  test_crypto.cpp
)

target_link_libraries(unit_tests PRIVATE common)

add_test(NAME unit_tests COMMAND unit_tests)