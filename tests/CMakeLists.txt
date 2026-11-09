add_executable(coop_tests
  main.cpp
  smoke_test.cpp
  protocol/codec_test.cpp
  protocol/fuzz_test.cpp
  net/clock_test.cpp
  net/client_test.cpp
  presence/presence_test.cpp
  host/reader_test.cpp
  host/sampler_test.cpp
  host/flags_test.cpp
  host/flag_watcher_test.cpp
  overlay/overlay_test.cpp
)
target_link_libraries(coop_tests PRIVATE coop coop_presence coop_host coop_overlay doctest::doctest coop_warnings)
doctest_discover_tests(coop_tests)

add_subdirectory(server)

# The test tool runs a server and two clients for 3 s and loses nothing.
add_test(NAME coop_test_two_clients COMMAND coop_test --clients 2 --seconds 3 --port 0 --report-every 0)
