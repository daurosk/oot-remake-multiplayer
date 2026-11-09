# ENet (MIT licence), fetched at a fixed commit and never copied into the tree.
# v1.3.18 = commit 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
include(FetchContent)

# ENet's own CMakeLists asks for a CMake older than 3.5, which CMake 4 refuses.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
)
FetchContent_MakeAvailable(enet)

unset(CMAKE_POLICY_VERSION_MINIMUM)

# ENet's CMakeLists sets its include path per directory, not on the target,
# and does not link the Windows socket libraries. Add both here.
target_include_directories(enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
if(WIN32)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

# ENet's own warnings are not ours to fix; keep the build log for our code.
if(MSVC)
  target_compile_options(enet PRIVATE /w)
else()
  target_compile_options(enet PRIVATE -w)
endif()

# ENet finds header offsets by member access through a null pointer
# (protocol.c, "& ((ENetProtocolHeader *) 0) -> sentTime"), an old offsetof
# idiom that UBSan reports. Turn off only that check, only for ENet; ASan and
# the rest of UBSan still cover it.
if(COOP_SANITIZE)
  target_compile_options(enet PRIVATE -fno-sanitize=null)
endif()
