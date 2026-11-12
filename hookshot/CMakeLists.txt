# hookshot_coop: co-op as a module Hookshot loads (docs/hookshot.md). Our
# patch to Hookshot (patches/) loads it from beside the game and calls the four
# exports in src/hos_coop_api.h. Copy the built hookshot_coop.dll next to
# hookshot.exe.
set(_coop_hook "${CMAKE_CURRENT_SOURCE_DIR}/../lichii/hook")
add_library(hookshot_coop MODULE
  src/coop_hookshot.cpp
  "${_coop_hook}/coop_callers.cpp"
  "${_coop_hook}/coop_camera.cpp"
  "${_coop_hook}/coop_pose.cpp"
  "${_coop_hook}/coop_puppet.cpp"
  "${_coop_hook}/coop_settings.cpp"
)
target_include_directories(hookshot_coop PRIVATE src "${_coop_hook}")
target_link_libraries(hookshot_coop PRIVATE coop coop_host coop_presence coop_warnings)
set_target_properties(hookshot_coop PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
