add_executable(coop_tests
  main.cpp
  smoke_test.cpp
  protocol/codec_test.cpp
  protocol/fuzz_test.cpp
)
target_link_libraries(coop_tests PRIVATE coop doctest::doctest coop_warnings)
doctest_discover_tests(coop_tests)

# The placeholder server must start ENet and exit 0.
add_test(NAME server_exits_cleanly COMMAND coop_server)
