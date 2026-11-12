find_package(Threads REQUIRED)

add_executable(coop_server_tests
  ../main.cpp
  session_test.cpp
  options_test.cpp
  loopback_test.cpp
  relay_test.cpp
  upnp_test.cpp
  nat_test.cpp
)
# upnp_test runs a fake router on its own thread.
target_link_libraries(coop_server_tests PRIVATE coop_server_lib doctest::doctest coop_warnings Threads::Threads)
doctest_discover_tests(coop_server_tests)

# The real program starts, listens on a free port and stops by itself.
add_test(NAME server_starts_and_stops COMMAND coop_server --port 0 --run-for-ms 200)
add_test(NAME server_refuses_bad_arguments COMMAND coop_server --frobnicate)
set_tests_properties(server_refuses_bad_arguments PROPERTIES WILL_FAIL TRUE)
# The relay starts and stops the same way; --upnp without a password or
# --allow is refused before anything opens.
add_test(NAME relay_starts_and_stops COMMAND coop_server --relay --port 0 --run-for-ms 200)
add_test(NAME server_refuses_upnp_without_access_control COMMAND coop_server --port 0 --upnp --run-for-ms 200)
set_tests_properties(server_refuses_upnp_without_access_control PROPERTIES WILL_FAIL TRUE)
# --allow is not enough for --upnp: the forward needs the join password.
add_test(NAME server_refuses_upnp_with_only_allow COMMAND coop_server --port 0 --upnp --allow 25.1.2.3 --run-for-ms 200)
set_tests_properties(server_refuses_upnp_with_only_allow PROPERTIES WILL_FAIL TRUE)
# A relay needs a join password; --allow is not enough (every relayed guest is 127.0.0.1).
add_test(NAME server_refuses_relay_without_password COMMAND coop_server --port 0 --relay-via 127.0.0.1:27530 --allow 25.1.2.3 --run-for-ms 200)
set_tests_properties(server_refuses_relay_without_password PROPERTIES WILL_FAIL TRUE)
