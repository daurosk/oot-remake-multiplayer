cmake_minimum_required(VERSION 3.24)

project(oot_coop VERSION 0.0.1 LANGUAGES C CXX)

option(COOP_BUILD_TESTS "Build the unit tests" ON)
option(COOP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC or Clang)" OFF)
option(COOP_TSAN "Build with ThreadSanitizer (GCC or Clang; not with COOP_SANITIZE)" OFF)

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

include(cmake/enet.cmake)

add_subdirectory(coop)
add_subdirectory(server)
add_subdirectory(tools/coop_test)

if(COOP_BUILD_TESTS)
  enable_testing()
  include(cmake/doctest.cmake)
  add_subdirectory(tests)
endif()
