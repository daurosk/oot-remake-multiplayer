# A ';' in a doctest name splits the name for ctest: doctest_discover_tests
# then registers a test that runs nothing. Refuse such names at configure time.
file(GLOB_RECURSE coop_test_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
foreach(source IN LISTS coop_test_sources)
  file(STRINGS "${source}" bad_names REGEX "(TEST_CASE|SUBCASE|SCENARIO|TEST_CASE_FIXTURE)[(]\"[^\"]*;[^\"]*\"")
  if(bad_names)
    message(FATAL_ERROR "A test name in ${source} has a ';' (ctest would run nothing for it): ${bad_names}")
  endif()
endforeach()

add_executable(coop_tests
  main.cpp
  smoke_test.cpp
  protocol/codec_test.cpp
  protocol/fuzz_test.cpp
  protocol/join_password_test.cpp
  net/clock_test.cpp
  net/client_test.cpp
  presence/presence_test.cpp
  host/reader_test.cpp
  host/sampler_test.cpp
  host/flags_test.cpp
  host/flag_watcher_test.cpp
  host/game_flags_test.cpp
  host/game_items_test.cpp
  host/notify_test.cpp
  host/save_transfer_test.cpp
  host/session_test.cpp
  host/scene_test.cpp
  overlay/overlay_test.cpp
  lichii/camera_test.cpp
  lichii/settings_test.cpp
  lichii/puppet_test.cpp
  lichii/callers_test.cpp
  lichii/fairy_test.cpp
  lichii/objects_test.cpp
  lichii/spawn_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_spawn.cpp
  lichii/projectiles_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_projectiles.cpp
  lichii/drops_test.cpp
  lichii/guard_test.cpp
  lichii/chests_test.cpp
  lichii/dungeon_test.cpp
  lichii/story_test.cpp
  lichii/pose_test.cpp
  lichii/world_test.cpp
  lichii/ocarina_test.cpp
  lichii/pickups_test.cpp
  lichii/testinput_test.cpp
  lichii/time_test.cpp
  lichii/every_test.cpp
  lichii/game_build_test.cpp
  lichii/boss_test.cpp
  lichii/teleport_test.cpp
  lichii/live_events_test.cpp
  lichii/cheat_items_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_cheat_items.cpp
  # These hook files have no emulator code, so they are tested on their own.
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_boss.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_callers.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_camera.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_drops.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_chests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_dungeon.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_story.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_live_events.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_live_places.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_fairy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_objects_sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_pose.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_puppet.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_settings.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_world.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_ocarina.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_pickups.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_testinput.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_teleport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_time_sync.cpp
)
target_include_directories(coop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook)
# The Hookshot module's own code (hookshot/src), when it is built.
if(COOP_BUILD_HOOKSHOT)
  target_sources(coop_tests PRIVATE hookshot/module_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../hookshot/src/coop_hookshot.cpp)
  target_include_directories(coop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../hookshot/src)
  target_compile_definitions(coop_tests PRIVATE COOP_HOOKSHOT_TEST)
endif()
target_link_libraries(coop_tests PRIVATE coop coop_presence coop_host coop_overlay doctest::doctest coop_warnings)
# The session tests run the real server in process.
target_link_libraries(coop_tests PRIVATE coop_server_lib)
doctest_discover_tests(coop_tests)

add_subdirectory(server)

# The test tool runs a server and two clients for 3 s and loses nothing.
add_test(NAME coop_test_two_clients COMMAND coop_test --clients 2 --seconds 3 --port 0 --report-every 0)

# Where the test data files are (source paths are trimmed out of __FILE__, cmake/trim_paths.cmake).
target_compile_definitions(coop_tests PRIVATE COOP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
