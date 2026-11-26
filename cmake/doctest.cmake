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

# Every test target gets doctest's real std headers (<ostream>, <string_view>, ...) instead of its forward
# declarations: a CHECK on a std::string_view or std::optional stringifies through std::ostream, and MSVC 14.51
# rejects that on the incomplete forward-declared type (C2027). Set once here so no test file has to remember it.
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)
