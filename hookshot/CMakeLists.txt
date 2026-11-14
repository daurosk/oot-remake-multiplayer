# hookshot_coop: co-op as a module Hookshot loads (docs/hookshot.md). Our patch
# to Hookshot (patches/) loads it from beside the game and calls the four
# exports in src/hos_coop_api.h. It is lichii/hook's co-op, the same files the
# emulator builds (read from lichii/hook/core.cmake, so a new hook file joins
# both), with compat/ answering the emulator API slice they use. Only the
# emulator's cave (coop_svc.cpp) stays out: src/coop_hookshot.cpp does its job
# here. coop_testinput.cpp plays the autotest driver's test commands
# (LICHII_COOP_TEST_COMMANDS); its pad input is the emulator's only.
include(FetchContent)
# fmt (MIT), for the hook's LOG_* lines (compat/common/logging.h), as the
# emulator has it. Fetched at a fixed commit, never copied into the tree.
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        407c905e45ad75fc29bf0f9bb7c5c2fd3475976f # 12.1.0, as Fritchii (its cpmfile.json)
)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)

set(_coop_hook "${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook")
file(STRINGS "${_coop_hook}/core.cmake" _coop_lines REGEX "coop_[a-z0-9_]+\\.cpp")
set(_coop_sources)
foreach(line IN LISTS _coop_lines)
  string(REGEX MATCH "coop_[a-z0-9_]+\\.cpp" name "${line}")
  if(name AND NOT name STREQUAL "coop_svc.cpp")
    list(APPEND _coop_sources "${_coop_hook}/${name}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _coop_sources)

add_library(hookshot_coop MODULE src/coop_hookshot.cpp src/coop_draw.cpp src/coop_dbno_draw.cpp src/coop_png.cpp src/coop_chatbox.cpp src/coop_cheats_panel.cpp
  src/coop_host_menu.cpp src/coop_title.cpp src/coop_title_menu.cpp src/coop_input.cpp src/coop_pause.cpp
  src/coop_pause_menu.cpp src/coop_pause_files.cpp
  src/coop_mod_json.cpp src/coop_mod_library.cpp src/coop_mods.cpp src/coop_mod_plugins.cpp src/coop_mod_guard.cpp
  src/coop_mods_runtime.cpp src/coop_rando.cpp src/coop_mods_tab.cpp src/coop_mods_tab_module.cpp
  src/coop_lobby_tab.cpp src/coop_lobby_tab_module.cpp
  src/coop_look.cpp src/coop_look_live.cpp src/coop_look_tunic.cpp src/coop_look_paint.cpp src/coop_look_parts.cpp src/coop_look_menu.cpp
  ${_coop_sources})
# compat/ first: its core/ and common/ headers stand in for the emulator's.
target_include_directories(hookshot_coop PRIVATE compat src "${_coop_hook}" ../server)
target_compile_definitions(hookshot_coop PRIVATE LICHII_COOP COOP_HOOKSHOT)
# coop_server_lib: the co-op server, run inside the game when the player hosts from the menu.
target_link_libraries(hookshot_coop PRIVATE coop coop_net coop_host coop_presence coop_overlay coop_server_lib
                      monocypher fmt::fmt coop_warnings)
set_target_properties(hookshot_coop PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
# Proximity voice (docs/voice.md): the two exports Hookshot's mixer looks for
# (hos_coop_voice.h, our patch 0009) and the game-free voice library.
target_sources(hookshot_coop PRIVATE src/coop_voice_hookshot.cpp)
target_link_libraries(hookshot_coop PRIVATE coop_voice)
if(MSVC)
  # The emulator's own code builds with these off too (lichii/README.md).
  target_compile_options(hookshot_coop PRIVATE /wd4456 /wd4457 /wd4458 /wd4459 /bigobj)
endif()

execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/.." describe --tags --always --dirty
    OUTPUT_VARIABLE _coop_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _coop_version)
  set(_coop_version "unknown")
endif()
set_property(SOURCE "${_coop_hook}/coop_hook.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "LICHII_COOP_VERSION=\"hookshot ${_coop_version}\"")
