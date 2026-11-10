# Keeps the build machine's folders out of the programs.
#
# A compiler writes each source file's full path into the program (__FILE__,
# which the emulator's log prints with every line), and the linker writes the
# full path of the .pdb. A released build then shows where it was compiled on
# the build machine. This file cuts those paths down to the part below the
# source root: `src\core\core.cpp`, `lichii\hook\coop_hook.cpp`.
#
# Our own build includes it (CMakeLists.txt). For the emulator, pass it at
# configure time (lichii/README.md):
#   -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<this repo>/cmake/trim_paths.cmake
# CMake then includes it at the emulator's first project() call, so it covers
# every target, the fetched libraries too.
#
# Roots that are cut off, longest first: this repo, LICHII_COOP_DIR, the source
# and build folders of the build, and last the user's home folder (whatever is
# left under it, such as a package cache).
#
# MSVC: /d1trimfile:<root>/ (it matches either slash and any letter case; the
# root is given with forward slashes, because a quoted argument that ends in a
# backslash would swallow its closing quote) and /PDBALTPATH:%_PDB% (the .pdb
# is named without its folder). GCC and Clang: -ffile-prefix-map=<root>/=.
if(COOP_TRIM_PATHS_DONE)
  return()
endif()
set(COOP_TRIM_PATHS_DONE ON)

set(_coop_trim_roots "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
if(LICHII_COOP_DIR)
  list(APPEND _coop_trim_roots "${LICHII_COOP_DIR}")
endif()
foreach(_coop_home_var USERPROFILE HOME)
  if(DEFINED ENV{${_coop_home_var}} AND NOT "$ENV{${_coop_home_var}}" STREQUAL "")
    list(APPEND _coop_trim_roots "$ENV{${_coop_home_var}}")
  endif()
endforeach()

# Absolute, forward slashes, no doubles, longest first.
set(_coop_trim_clean "")
foreach(_coop_root IN LISTS _coop_trim_roots)
  get_filename_component(_coop_root "${_coop_root}" ABSOLUTE)
  file(TO_CMAKE_PATH "${_coop_root}" _coop_root)
  string(REGEX REPLACE "/+$" "" _coop_root "${_coop_root}")
  if(_coop_root AND NOT _coop_root MATCHES "^([A-Za-z]:)?$")
    list(APPEND _coop_trim_clean "${_coop_root}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _coop_trim_clean)
set(_coop_trim_sorted "")
while(_coop_trim_clean)
  set(_coop_longest "")
  foreach(_coop_root IN LISTS _coop_trim_clean)
    string(LENGTH "${_coop_root}" _coop_len)
    string(LENGTH "${_coop_longest}" _coop_best)
    if(_coop_len GREATER _coop_best)
      set(_coop_longest "${_coop_root}")
    endif()
  endforeach()
  list(APPEND _coop_trim_sorted "${_coop_longest}")
  list(REMOVE_ITEM _coop_trim_clean "${_coop_longest}")
endwhile()

# Generator expressions, because this may run before any compiler is known.
# Only C and C++ get the flag (the resource compiler and assemblers do not
# know it).
foreach(_coop_root IN LISTS _coop_trim_sorted)
  foreach(_coop_lang C CXX)
    add_compile_options(
      "$<$<COMPILE_LANG_AND_ID:${_coop_lang},MSVC>:/d1trimfile:${_coop_root}/>"
      "$<$<COMPILE_LANG_AND_ID:${_coop_lang},GNU,Clang,AppleClang>:-ffile-prefix-map=${_coop_root}/=>")
  endforeach()
endforeach()
foreach(_coop_lang C CXX)
  add_link_options("$<$<LINK_LANG_AND_ID:${_coop_lang},MSVC>:/PDBALTPATH:%_PDB%>")
endforeach()
message(STATUS "Source paths trimmed below: ${_coop_trim_sorted}")
