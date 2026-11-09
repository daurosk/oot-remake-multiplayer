# Included by the patched src/yuzu/CMakeLists.txt when LICHII_COOP is on.
# Adds the co-op window and the debug item cheat window to the Qt frontend.
set(_coop_hook "${LICHII_COOP_DIR}/lichii/hook")
target_sources(yuzu PRIVATE
    "${_coop_hook}/coop_cheats_window.cpp"
    "${_coop_hook}/coop_window.cpp"
)
target_include_directories(yuzu PRIVATE "${_coop_hook}")
target_compile_definitions(yuzu PRIVATE LICHII_COOP)
