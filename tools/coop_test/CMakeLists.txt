# The stage 0 test tool (docs/soak.md): the real server and test clients.
add_executable(coop_test
  main.cpp
  options.cpp
  probe.cpp
)
target_link_libraries(coop_test PRIVATE coop_server_lib coop_net coop_presence enet coop_warnings)
if(WIN32)
  target_link_libraries(coop_test PRIVATE winmm)
endif()
