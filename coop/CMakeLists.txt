# The co-op library: protocol and networking, filled in by later stage 0 steps.
add_library(coop STATIC
  src/version.cpp
)
target_include_directories(coop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(coop PUBLIC enet PRIVATE coop_warnings)
