# Included by the patched src/core/CMakeLists.txt when LICHII_COOP is on.
# Adds the co-op hook to the emulator's core library. Changing what the hook
# builds needs no new patch.
set(_coop_hook "${LICHII_COOP_DIR}/lichii/hook")
target_sources(core PRIVATE
    "${_coop_hook}/coop_boss.cpp"
    "${_coop_hook}/coop_boss_game.cpp"
    "${_coop_hook}/coop_teleport.cpp"
    "${_coop_hook}/coop_teleport_game.cpp"
    "${_coop_hook}/coop_pvp.cpp"
    "${_coop_hook}/coop_pvp_game.cpp"
    "${_coop_hook}/coop_scene_probe.cpp"
    "${_coop_hook}/coop_callers.cpp"
    "${_coop_hook}/coop_camera.cpp"
    "${_coop_hook}/coop_cheat_items.cpp"
    "${_coop_hook}/coop_cheats.cpp"
    "${_coop_hook}/coop_drops.cpp"
    "${_coop_hook}/coop_chests.cpp"
    "${_coop_hook}/coop_colors.cpp"
    "${_coop_hook}/coop_dungeon.cpp"
    "${_coop_hook}/coop_platforms.cpp"
    "${_coop_hook}/coop_counters.cpp"
    "${_coop_hook}/coop_quest.cpp"
    "${_coop_hook}/coop_story.cpp"
    "${_coop_hook}/coop_quests.cpp"
    "${_coop_hook}/coop_live_events.cpp"
    "${_coop_hook}/coop_live_places.cpp"
    "${_coop_hook}/coop_shared_scenes.cpp"
    "${_coop_hook}/coop_scrubs.cpp"
    "${_coop_hook}/coop_fairy.cpp"
    "${_coop_hook}/coop_rooms.cpp"
    "${_coop_hook}/coop_flags.cpp"
    "${_coop_hook}/coop_pickups.cpp"
    "${_coop_hook}/coop_hook.cpp"
    "${_coop_hook}/coop_local_game.cpp"
    "${_coop_hook}/coop_local_net.cpp"
    "${_coop_hook}/coop_hud.cpp"
    "${_coop_hook}/coop_objects.cpp"
    "${_coop_hook}/coop_objects_sync.cpp"
    "${_coop_hook}/coop_npc_anim.cpp"
    "${_coop_hook}/coop_pose.cpp"
    "${_coop_hook}/coop_save.cpp"
    "${_coop_hook}/coop_enemy_shots.cpp"
    "${_coop_hook}/coop_projectiles.cpp"
    "${_coop_hook}/coop_puppet.cpp"
    "${_coop_hook}/coop_puppet_config.cpp"
    "${_coop_hook}/coop_ride.cpp"
    "${_coop_hook}/coop_settings.cpp"
    "${_coop_hook}/coop_spawn.cpp"
    "${_coop_hook}/coop_svc.cpp"
    "${_coop_hook}/coop_world.cpp"
    "${_coop_hook}/coop_ocarina.cpp"
    "${_coop_hook}/coop_ocarina_sound.cpp"
    "${_coop_hook}/coop_voice.cpp"
    "${_coop_hook}/coop_puppet_sound.cpp"
    "${_coop_hook}/coop_testinput.cpp"
    "${_coop_hook}/coop_time.cpp"
    "${_coop_hook}/coop_time_sync.cpp"
)
# Public, so video_core (which links core) sees them: the patched
# vk_rasterizer.cpp includes coop_camera_track.h under LICHII_COOP.
target_include_directories(core PUBLIC "${_coop_hook}")
target_compile_definitions(core PUBLIC LICHII_COOP)
target_link_libraries(core PRIVATE coop coop_net coop_host coop_presence coop_overlay coop_voice)
# Which co-op commit is built in, for the log ("coop: build ..."). Read when
# CMake configures, so reconfigure after a pull to keep it current.
execute_process(COMMAND git -C "${LICHII_COOP_DIR}" describe --tags --always --dirty
    OUTPUT_VARIABLE _coop_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _coop_version)
    set(_coop_version "unknown")
endif()
set_property(SOURCE "${_coop_hook}/coop_hook.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "LICHII_COOP_VERSION=\"${_coop_version}\"")
# GCC and Clang (the Linux build, issue 127): Fritchii's src/CMakeLists.txt makes -Wall,
# -Wextra, -Wmissing-declarations, -Wshadow and -Wunused errors for everything in core. The
# hook is built with MSVC on Windows, which never sees them, so here they stay warnings for
# the hook's own files (they still show in the build log). Fritchii's code is unchanged.
if(NOT MSVC)
    # The hook's own files: those under _coop_hook (a plain prefix test, so a path with
    # regex characters such as + or ( still matches).
    get_target_property(_coop_all_sources core SOURCES)
    set(_coop_core_sources "")
    foreach(_coop_src IN LISTS _coop_all_sources)
        string(FIND "${_coop_src}" "${_coop_hook}/" _coop_at)
        if(_coop_at EQUAL 0)
            list(APPEND _coop_core_sources "${_coop_src}")
        endif()
    endforeach()
    set_property(SOURCE ${_coop_core_sources} APPEND PROPERTY COMPILE_OPTIONS
        -Wno-error=all -Wno-error=extra -Wno-error=missing-declarations -Wno-error=shadow
        -Wno-error=unused -Wno-error=range-loop-construct
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error=maybe-uninitialized>)
endif()
