# Monocypher 4.0.2 (Loup Vaillant and others; BSD-2-Clause or CC0-1.0, the
# user's choice), fetched at a fixed commit and never copied into the tree.
# Used for the traffic encryption (coop/protocol traffic_crypto): X25519,
# BLAKE2b and XChaCha20-Poly1305. No crypto primitive is our own.
include(FetchContent)

FetchContent_Declare(monocypher
  GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git
  GIT_TAG        0d85f98c9d9b0227e42cf795cb527dff372b40a4 # 4.0.2
  # It has no CMakeLists: this only fetches; the one file is built below.
  SOURCE_SUBDIR  no-cmake-here
)
FetchContent_MakeAvailable(monocypher)

add_library(monocypher STATIC "${monocypher_SOURCE_DIR}/src/monocypher.c")
target_include_directories(monocypher SYSTEM PUBLIC "${monocypher_SOURCE_DIR}/src")
set_target_properties(monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(monocypher PRIVATE /w)
else()
  target_compile_options(monocypher PRIVATE -w)
endif()
