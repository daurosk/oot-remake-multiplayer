# The NX2navi mod (nx2navi/README.md): the game-side DLL and the mod folder.
# Built only with -DCOOP_BUILD_NX2NAVI=ON. nx2navi/build.py builds the same
# without CMake.
if(NOT WIN32)
  message(FATAL_ERROR "COOP_BUILD_NX2NAVI: the NX2navi mod is a Windows DLL")
endif()

# The parts with no DllMain, so the tests can link them.
add_library(coop_nx2navi_lib STATIC
  dll/guest_memory.cpp
  dll/settings.cpp
  dll/overlay_model.cpp
  dll/teleport_auto.cpp
  dll/menu_model.cpp
  dll/save_sync.cpp
  dll/mp_saves.cpp dll/step_switch.cpp dll/item_get.cpp
  dll/steam_model.cpp
  dll/steam_menu.cpp
  # No emulator code in it (tests/CMakeLists.txt builds it the same way).
  ${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook/coop_settings.cpp
)
target_include_directories(coop_nx2navi_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook")
target_link_libraries(coop_nx2navi_lib PUBLIC coop_host coop_net PRIVATE coop_warnings)

add_library(coop_nx2navi SHARED dll/coop_dll.cpp dll/overlay.cpp dll/puppet.cpp dll/teleport.cpp
  dll/title_menu.cpp dll/ai_probe.cpp dll/progress.cpp dll/steam_net.cpp
  ../lichii/hook/coop_chests.cpp ../lichii/hook/coop_story.cpp ../lichii/hook/coop_quests.cpp
  ../lichii/hook/coop_quest.cpp ../lichii/hook/coop_shared_scenes.cpp
  ../lichii/hook/coop_puppet.cpp ../lichii/hook/coop_pose.cpp ../lichii/hook/coop_camera.cpp
  # Teleport's rules and the event request: the hook's own, no emulator code.
  ../lichii/hook/coop_teleport.cpp ../lichii/hook/coop_live_events.cpp
  ../lichii/hook/coop_live_places.cpp
  # Shared enemies and boss rooms (dll/objects/INTEGRATION.md): the hook's files unchanged,
  # against the stand-in emulator headers in dll/objects/shim.
  dll/objects/objects.cpp dll/objects/emu_port.cpp dll/objects/hold.cpp dll/objects/brain.cpp dll/objects/anim_probe.cpp dll/objects/world_probe.cpp
  ../lichii/hook/coop_objects.cpp ../lichii/hook/coop_objects_sync.cpp ../lichii/hook/coop_npc_anim.cpp
  ../lichii/hook/coop_world.cpp
  ../lichii/hook/coop_spawn.cpp ../lichii/hook/coop_projectiles.cpp ../lichii/hook/coop_pickups.cpp
  ../lichii/hook/coop_ocarina.cpp ../lichii/hook/coop_cheat_items.cpp ../lichii/hook/coop_flags.cpp
  ../lichii/hook/coop_boss.cpp ../lichii/hook/coop_boss_game.cpp ../lichii/hook/coop_dungeon.cpp)
target_include_directories(coop_nx2navi PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dll/objects/shim" "${CMAKE_CURRENT_SOURCE_DIR}/dll/objects")
set_target_properties(coop_nx2navi PROPERTIES PREFIX "" OUTPUT_NAME "coop_nx2navi")
target_link_libraries(coop_nx2navi PRIVATE
  coop_nx2navi_lib coop_host coop_net coop_presence enet coop_warnings crypt32 iphlpapi)
if(MINGW)
  # No libstdc++, libgcc or winpthread DLL beside the game. (With MSVC the DLL
  # uses the Visual C++ runtime, as the server in the emulator kit does.)
  target_link_options(coop_nx2navi PRIVATE -static -static-libgcc -static-libstdc++)
  if(TARGET coop_server)
    target_link_options(coop_server PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()

if(COOP_BUILD_TESTS AND TARGET coop_tests)
  target_sources(coop_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/nx2navi_test.cpp")
  target_sources(coop_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/steam_test.cpp")
  target_link_libraries(coop_tests PRIVATE coop_nx2navi_lib)
  # The stand-in game process for nx2navi/tests/test_dll.py.
  add_executable(coop_nx2navi_fake_game tests/fake_game.cpp)
  target_include_directories(coop_nx2navi_fake_game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook")
  target_link_libraries(coop_nx2navi_fake_game PRIVATE coop_host coop_warnings)
  # The same as a game with NX2navi's plugin interface (exports nx2navi_plugin_api).
  add_executable(coop_nx2navi_fake_host tests/fake_game.cpp)
  target_compile_definitions(coop_nx2navi_fake_host PRIVATE COOP_FAKE_PLUGIN_HOST)
  target_include_directories(coop_nx2navi_fake_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook")
  target_link_libraries(coop_nx2navi_fake_host PRIVATE coop_host coop_warnings)
  if(MINGW)
    target_link_options(coop_nx2navi_fake_game PRIVATE -municode -static)
    target_link_options(coop_nx2navi_fake_host PRIVATE -municode -static)
  endif()
endif()

# The folder a player drops into <game>/mods/.
if(TARGET coop_server)
  set(coop_nx2navi_mod "${CMAKE_BINARY_DIR}/nx2navi-mod/OoT Co-op")
  add_custom_target(coop_nx2navi_mod ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${coop_nx2navi_mod}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mod/mod.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/mod/launcher_plugin.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/mod/README.txt"
            "${coop_nx2navi_mod}"
    # The repository's notices followed by this mod's own (nx2navi/build.py does the same).
    COMMAND ${CMAKE_COMMAND}
            "-DOUT=${coop_nx2navi_mod}/THIRD_PARTY_NOTICES.md"
            "-DFIRST=${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
            "-DSECOND=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/join_notices.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${coop_nx2navi_mod}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:coop_nx2navi>" "${coop_nx2navi_mod}/bin/coop_nx2navi.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:coop_server>" "${coop_nx2navi_mod}/bin/oot-coop-server.exe"
    DEPENDS coop_nx2navi coop_server
    VERBATIM)
endif()
