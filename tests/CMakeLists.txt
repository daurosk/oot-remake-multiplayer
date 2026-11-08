add_executable(coop_tests
  main.cpp
  smoke_test.cpp
  protocol/codec_test.cpp
  protocol/fuzz_test.cpp
  net/clock_test.cpp
  net/client_test.cpp
)
target_link_libraries(coop_tests PRIVATE coop doctest::doctest coop_warnings)
doctest_discover_tests(coop_tests)

add_subdirectory(server)
