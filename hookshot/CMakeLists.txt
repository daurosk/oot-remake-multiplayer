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

# COOP_HOOK_STRICT (CI's "Compile the hook code" step, GCC only): the lichii/hook files get the
# flags the emulator's core target gives them on Linux (Fritchii's -O3 and -Werror=all/extra, with
# the same -Wno-* list and the hook's de-errors from lichii/hook/core.cmake), so a warning that
# fails the emulator build (0.11.8: -Werror=stringop-truncation, which only shows at -O3) fails
# linux (gcc) on the branch push instead of at the release gate. Our own hookshot/src files keep coop_warnings.
option(COOP_HOOK_STRICT "Compile lichii/hook files with the emulator's GCC flags, warnings as errors" OFF)
if(COOP_HOOK_STRICT AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_property(SOURCE ${_coop_sources} APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Werror=all -Werror=extra -Werror=missing-declarations -Werror=shadow -Werror=unused
    -Wno-attributes -Wno-invalid-offsetof -Wno-unused-parameter -Wno-missing-field-initializers
    -Wno-error=unused -Wno-error=unused-function
    -Wno-array-bounds -Wno-stringop-overread -Wno-stringop-overflow
    -Wno-cast-function-type -Wno-shadow -Wno-implicit-fallthrough
    -Wno-error=all -Wno-error=extra -Wno-error=missing-declarations -Wno-error=shadow -Wno-error=unused
    -Wno-error=range-loop-construct -Wno-error=maybe-uninitialized)
endif()

add_library(hookshot_coop MODULE src/coop_hookshot.cpp src/coop_draw.cpp src/coop_map_page.cpp src/coop_dbno_draw.cpp src/coop_png.cpp src/coop_window_brand.cpp src/coop_back_window_os.cpp src/coop_chatbox.cpp src/coop_players_open.cpp src/coop_native_ui.cpp src/coop_party_hud.cpp src/coop_cheats_panel.cpp
  src/coop_host_menu.cpp src/coop_title.cpp src/coop_title_menu.cpp src/coop_input.cpp src/coop_pause.cpp
  src/coop_pause_menu.cpp src/coop_original.cpp src/coop_pause_files.cpp src/coop_i18n.cpp src/coop_null_page.cpp
  src/coop_mod_json.cpp src/coop_mod_library.cpp src/coop_mods.cpp src/coop_mod_plugins.cpp src/coop_mod_guard.cpp
  src/coop_mod_screens.cpp src/coop_playtest.cpp src/coop_playtest_core.cpp src/coop_talk_text.cpp
  src/coop_mods_runtime.cpp src/coop_rando.cpp src/coop_mods_tab.cpp src/coop_mods_tab_module.cpp
  src/coop_lobby_tab.cpp src/coop_lobby_tab_module.cpp src/coop_local_start.cpp src/coop_local_start_os.cpp
  src/coop_pause_tab.cpp src/coop_pause_tab_module.cpp src/coop_title_native.cpp src/coop_problem_ask.cpp src/coop_native_tags.cpp
  src/coop_auto_rejoin.cpp src/coop_lan.cpp src/coop_lan_net.cpp src/coop_room_list.cpp src/coop_room_list_text.cpp src/coop_browse.cpp src/coop_longshot_settings.cpp
  src/coop_look.cpp src/coop_look_live.cpp src/coop_look_tunic.cpp src/coop_look_paint.cpp src/coop_look_parts.cpp src/coop_look_menu.cpp src/coop_snail_custom.cpp src/coop_menu_state.cpp src/coop_look_view.cpp
  ${_coop_sources})
# compat/ first: its core/ and common/ headers stand in for the emulator's.
target_include_directories(hookshot_coop PRIVATE compat src "${_coop_hook}" ../server)
target_compile_definitions(hookshot_coop PRIVATE LICHII_COOP COOP_HOOKSHOT)
# COOP_DEBUG_PANELS: the drawn debug panels, F7 (co-op panel) and F8 (item cheats); off, so release kits have no drawn
# boxes (src/coop_debug_panels.h). A developer's or test build: -DCOOP_DEBUG_PANELS=ON.
option(COOP_DEBUG_PANELS "Build the F7 co-op panel and the F8 item cheats panel into hookshot_coop" OFF)
if(COOP_DEBUG_PANELS)
  target_compile_definitions(hookshot_coop PRIVATE COOP_DEBUG_PANELS)
endif()
# coop_server_lib: the co-op server, run inside the game when the player hosts from the menu.
target_link_libraries(hookshot_coop PRIVATE coop coop_net coop_host coop_presence coop_overlay coop_server_lib
                      monocypher fmt::fmt coop_warnings)
set_target_properties(hookshot_coop PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
# Proximity voice (docs/voice.md): the two exports Hookshot's mixer looks for
# (hos_coop_voice.h, our patch 0009) and the game-free voice library.
target_sources(hookshot_coop PRIVATE src/coop_voice_hookshot.cpp)
target_link_libraries(hookshot_coop PRIVATE coop_voice)
if(WIN32)
  # GetAdaptersAddresses: the Host dialog's Same network and VPN rows name each network card.
  target_link_libraries(hookshot_coop PRIVATE iphlpapi ws2_32)
else()
  # Linux: the clipboard through the desktop's tools (wl-copy, xclip, xsel); Windows has its own.
  target_sources(hookshot_coop PRIVATE src/coop_clipboard_posix.cpp)
  # The game's bridge opens the module by its full path (RTLD_LOCAL); the renderer's menu tabs and
  # drawing (patches 0001, 0012) and the world hooks (0005) then find it with
  # dlopen("hookshot_coop.so", RTLD_NOLOAD), and glibc matches a bare name to a loaded library only
  # by its SONAME. Without one they got nothing: no Multiplayer tab, chat or name tags on Linux.
  target_link_options(hookshot_coop PRIVATE -Wl,-soname,hookshot_coop.so)
  # The C++ runtime inside the module (none of its symbols exported), so it loads into the game on any
  # distribution from the one it was built on (CI: Ubuntu 22.04, glibc 2.35, as Longshot's own) up,
  # whatever libstdc++ that system has.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hookshot_coop PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--no-undefined)
  endif()
  # dlsym: Ctrl+V reads the modifier keys from the game's own SDL (coop_pause.cpp Modifiers).
  target_link_libraries(hookshot_coop PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  # The emulator's own code builds with these off too (lichii/README.md).
  target_compile_options(hookshot_coop PRIVATE /wd4456 /wd4457 /wd4458 /wd4459 /bigobj)
endif()

# The co-op commit for the log, from git describe on every build (cmake/coop_version.cmake;
# a configure-time string went stale: 0.11.3's module said v0.11.2-18-gab129f5b).
set(_coop_version_dir "${CMAKE_CURRENT_BINARY_DIR}/coop_version")
add_custom_target(coop_version_hookshot ALL
  COMMAND "${CMAKE_COMMAND}" "-DCOOP_GIT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.." "-DCOOP_VERSION_PREFIX=hookshot"
          "-DCOOP_VERSION_OUT=${_coop_version_dir}/coop_version.h"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/coop_version.cmake"
  BYPRODUCTS "${_coop_version_dir}/coop_version.h"
  COMMENT "co-op version (git describe)"
  VERBATIM)
add_dependencies(hookshot_coop coop_version_hookshot)
target_include_directories(hookshot_coop PRIVATE "${_coop_version_dir}")
