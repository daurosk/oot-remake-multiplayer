cmake_minimum_required(VERSION 3.24)

project(oot_coop VERSION 0.0.1 LANGUAGES C CXX)

option(COOP_BUILD_TESTS "Build the unit tests" ON)
option(COOP_BUILD_PROGRAMS "Build the server and tools/coop_test" ON)
option(COOP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC or Clang)" OFF)
option(COOP_TSAN "Build with ThreadSanitizer (GCC or Clang; not with COOP_SANITIZE)" OFF)

# No build machine folders in the programs (the server goes into the kit).
include(cmake/trim_paths.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings for our own targets only; fetched code keeps its own flags.
add_library(coop_warnings INTERFACE)
if(MSVC)
  target_compile_options(coop_warnings INTERFACE /W4 /permissive- /utf-8)
else()
  target_compile_options(coop_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Sanitizers apply to everything, ENet included, so its memory is checked too.
if(COOP_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "COOP_SANITIZE is for GCC or Clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer cannot be combined with ASan, so it is its own build.
if(COOP_TSAN)
  if(MSVC)
    message(FATAL_ERROR "COOP_TSAN is for GCC or Clang")
  endif()
  if(COOP_SANITIZE)
    message(FATAL_ERROR "COOP_TSAN and COOP_SANITIZE cannot be on together")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

# The emulator brings its own ENet target (the same v1.3.18); use that one
# when it is there, so the two do not clash. It sets the include path itself.
if(TARGET enet)
  if(WIN32)
    target_link_libraries(enet INTERFACE ws2_32 winmm)
  endif()
else()
  include(cmake/enet.cmake)
endif()

add_subdirectory(coop)
if(COOP_BUILD_PROGRAMS)
  add_subdirectory(server)
  add_subdirectory(tools/coop_test)
endif()

if(COOP_BUILD_TESTS)
  enable_testing()
  include(cmake/doctest.cmake)
  add_subdirectory(tests)
endif()
