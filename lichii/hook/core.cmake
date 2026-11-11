# Included by the patched src/core/CMakeLists.txt when LICHII_COOP is on.
# Adds the co-op hook to the emulator's core library. Changing what the hook
# builds needs no new patch.
set(_coop_hook "${LICHII_COOP_DIR}/lichii/hook")
target_sources(core PRIVATE
    "${_coop_hook}/coop_boss.cpp"
    "${_coop_hook}/coop_boss_game.cpp"
    "${_coop_hook}/coop_camera.cpp"
    "${_coop_hook}/coop_cheat_items.cpp"
    "${_coop_hook}/coop_cheats.cpp"
    "${_coop_hook}/coop_drops.cpp"
    "${_coop_hook}/coop_chests.cpp"
    "${_coop_hook}/coop_dungeon.cpp"
    "${_coop_hook}/coop_live_events.cpp"
    "${_coop_hook}/coop_fairy.cpp"
    "${_coop_hook}/coop_flags.cpp"
    "${_coop_hook}/coop_pickups.cpp"
    "${_coop_hook}/coop_hook.cpp"
    "${_coop_hook}/coop_objects.cpp"
    "${_coop_hook}/coop_objects_sync.cpp"
    "${_coop_hook}/coop_pose.cpp"
    "${_coop_hook}/coop_projectiles.cpp"
    "${_coop_hook}/coop_puppet.cpp"
    "${_coop_hook}/coop_settings.cpp"
    "${_coop_hook}/coop_spawn.cpp"
    "${_coop_hook}/coop_svc.cpp"
    "${_coop_hook}/coop_world.cpp"
    "${_coop_hook}/coop_ocarina.cpp"
    "${_coop_hook}/coop_testinput.cpp"
    "${_coop_hook}/coop_time.cpp"
    "${_coop_hook}/coop_time_sync.cpp"
)
# Public, so video_core (which links core) sees them: the patched
# vk_rasterizer.cpp includes coop_camera_track.h under LICHII_COOP.
target_include_directories(core PUBLIC "${_coop_hook}")
target_compile_definitions(core PUBLIC LICHII_COOP)
target_link_libraries(core PRIVATE coop coop_net coop_host coop_presence coop_overlay)
# Which co-op commit is built in, for the log ("coop: build ..."). Read when
# CMake configures, so reconfigure after a pull to keep it current.
execute_process(COMMAND git -C "${LICHII_COOP_DIR}" describe --tags --always --dirty
    OUTPUT_VARIABLE _coop_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _coop_version)
    set(_coop_version "unknown")
endif()
set_property(SOURCE "${_coop_hook}/coop_hook.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "LICHII_COOP_VERSION=\"${_coop_version}\"")
