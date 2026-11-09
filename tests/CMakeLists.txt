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
  host/game_flags_test.cpp
  host/game_items_test.cpp
  host/session_test.cpp
  host/scene_test.cpp
  overlay/overlay_test.cpp
  lichii/camera_test.cpp
  lichii/settings_test.cpp
  lichii/puppet_test.cpp
  lichii/objects_test.cpp
  # These hook files have no emulator code, so they are tested on their own.
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_camera.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_objects_sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_puppet.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_settings.cpp
)
target_include_directories(coop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook)
target_link_libraries(coop_tests PRIVATE coop coop_presence coop_host coop_overlay doctest::doctest coop_warnings)
# The session tests run the real server in process.
target_link_libraries(coop_tests PRIVATE coop_server_lib)
doctest_discover_tests(coop_tests)

add_subdirectory(server)

# The test tool runs a server and two clients for 3 s and loses nothing.
add_test(NAME coop_test_two_clients COMMAND coop_test --clients 2 --seconds 3 --port 0 --report-every 0)
