# The co-op relay server, filled in by stage 0 step 3.
add_executable(coop_server main.cpp)
target_link_libraries(coop_server PRIVATE coop coop_warnings)
