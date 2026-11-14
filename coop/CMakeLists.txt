# The wire protocol: plain bytes in, plain structs out. No ENet, so the
# server, the client, Lichii and the recomp can all share it.
add_library(coop_protocol STATIC
  protocol/src/codec.cpp
  protocol/src/join_password.cpp
  protocol/src/traffic_crypto.cpp
)
target_include_directories(coop_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/protocol/include")
target_link_libraries(coop_protocol PRIVATE monocypher coop_warnings)
if(WIN32)
  target_link_libraries(coop_protocol PRIVATE bcrypt)  # BCryptGenRandom (join_password.cpp)
endif()

# The client over ENet that the test tool, Lichii and the recomp use.
add_library(coop_net STATIC
  net/src/clock.cpp
  net/src/client.cpp
)
target_include_directories(coop_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/net/include")
target_link_libraries(coop_net PUBLIC coop_protocol PRIVATE enet coop_warnings)

# The co-op library: protocol and networking, filled in by later stage 0 steps.
add_library(coop STATIC
  src/version.cpp
)
target_include_directories(coop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(coop PUBLIC enet coop_protocol coop_net PRIVATE coop_warnings)

# Presence: where to draw the other players, from their decoded messages.
# No sockets and no game, so Lichii and the recomp can both call it.
add_library(coop_presence STATIC
  presence/src/presence.cpp
)
target_include_directories(coop_presence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/presence/include")
target_link_libraries(coop_presence PUBLIC coop_protocol PRIVATE coop_warnings)

# Host: reads the local player from guest memory (through an interface the
# emulator implements) and builds PLAYER_SAMPLE and PLAYER_STATUS. No sockets.
add_library(coop_host STATIC
  host/src/flags.cpp
  host/src/flag_watcher.cpp
  host/src/game_flags.cpp
  host/src/game_flag_events.cpp
  host/src/game_items.cpp
  host/src/notify.cpp
  host/src/one_way_keeper.cpp
  host/src/game_memory.cpp
  host/src/player_reader.cpp
  host/src/sampler.cpp
  host/src/save_transfer.cpp
  host/src/session.cpp
)
target_include_directories(coop_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host/include")
# Session (session.hpp) ties the client, the sampler, the flag watcher and
# presence together, so the host library also links those two.
target_link_libraries(coop_host PUBLIC coop_protocol coop_net coop_presence PRIVATE coop_warnings)

# Overlay: the other players as markers with name tags, as a plain draw list.
# No rendering and no game addresses; the caller hands over the camera.
add_library(coop_overlay STATIC
  overlay/src/overlay.cpp
  overlay/src/map_icons.cpp
)
target_include_directories(coop_overlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/overlay/include")
# Header only: the scene rules (coop/host/scene.hpp), so a marker shows in the
# same places as a puppet.
target_include_directories(coop_overlay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host/include")
target_link_libraries(coop_overlay PUBLIC coop_presence PRIVATE coop_warnings)

# Proximity voice chat (docs/voice.md): jitter buffer, talk gate, 3D
# placement and the mixer of the others' voices. No sockets, no audio device,
# no codec of its own (the game hands Opus in): Hookshot's module and the
# tests both use it.
add_library(coop_voice STATIC
  voice/src/engine.cpp
  voice/src/gate.cpp
  voice/src/jitter.cpp
  voice/src/spatial.cpp
)
target_include_directories(coop_voice PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/voice/include")
target_link_libraries(coop_voice PUBLIC coop_protocol PRIVATE coop_warnings)
