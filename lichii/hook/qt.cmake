# Included by the patched src/yuzu/CMakeLists.txt when LICHII_COOP is on.
# Adds the co-op window and the debug item cheat window to the Qt frontend.
set(_coop_hook "${LICHII_COOP_DIR}/lichii/hook")
target_sources(yuzu PRIVATE
    "${_coop_hook}/coop_cheats_window.cpp"
    "${_coop_hook}/coop_window.cpp"
)
target_include_directories(yuzu PRIVATE "${_coop_hook}")
target_compile_definitions(yuzu PRIVATE LICHII_COOP)
# GCC and Clang (the Linux build, issue 127): Fritchii's own src/yuzu/fritchii/fritchii_ui.cpp copies a
# QString in two range loops, which its -Werror=all turns into an error with GCC 13. MSVC
# (Windows) never sees it. Kept as a warning for the Qt frontend only; no Fritchii file changes.
if(NOT MSVC)
    target_compile_options(yuzu PRIVATE -Wno-error=range-loop-construct)
endif()
