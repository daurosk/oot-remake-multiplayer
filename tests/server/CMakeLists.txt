add_executable(coop_server_tests
  ../main.cpp
  session_test.cpp
  options_test.cpp
  loopback_test.cpp
)
target_link_libraries(coop_server_tests PRIVATE coop_server_lib doctest::doctest coop_warnings)
doctest_discover_tests(coop_server_tests)

# The real program starts, listens on a free port and stops by itself.
add_test(NAME server_starts_and_stops COMMAND coop_server --port 0 --run-for-ms 200)
add_test(NAME server_refuses_bad_arguments COMMAND coop_server --frobnicate)
set_tests_properties(server_refuses_bad_arguments PROPERTIES WILL_FAIL TRUE)
