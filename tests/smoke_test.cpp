#include <doctest/doctest.h>
#include <enet/enet.h>

#include "coop/version.hpp"

TEST_CASE("the pinned ENet version is linked") {
  CHECK(coop::enet_linked_version() == ENET_VERSION_CREATE(1, 3, 18));
}

TEST_CASE("ENet starts and stops") {
  CHECK(coop::enet_starts());
}
