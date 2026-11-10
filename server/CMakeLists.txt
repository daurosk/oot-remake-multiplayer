# The co-op relay server (stage 0 step 3). The rules are in a library so the
# tests can drive them without a network; the program is a thin ENet loop.
add_library(coop_server_lib STATIC
  session.cpp
  world.cpp
  enet_server.cpp
  options.cpp
)
target_include_directories(coop_server_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(coop_server_lib PUBLIC coop_protocol enet PRIVATE coop_warnings)

add_executable(coop_server main.cpp)
target_link_libraries(coop_server PRIVATE coop_server_lib coop_warnings)
