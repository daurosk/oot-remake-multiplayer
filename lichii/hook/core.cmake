# Included by the patched src/core/CMakeLists.txt when LICHII_COOP is on.
# Adds the co-op hook to the emulator's core library. Changing what the hook
# builds needs no new patch.
set(_coop_hook "${LICHII_COOP_DIR}/lichii/hook")
target_sources(core PRIVATE
    "${_coop_hook}/coop_hook.cpp"
    "${_coop_hook}/coop_settings.cpp"
)
target_include_directories(core PRIVATE "${_coop_hook}")
target_compile_definitions(core PRIVATE LICHII_COOP)
target_link_libraries(core PRIVATE coop coop_net coop_host coop_presence coop_overlay)
