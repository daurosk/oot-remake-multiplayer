# doctest (MIT licence), header-only test framework, fetched at a fixed commit.
# v2.5.3 = commit 2d0a9359a60c51affe2a9bebb1be1dca47868151
include(FetchContent)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
